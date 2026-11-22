#include "ime_indicator.h"
#include "shape.h"
#include "image.h"
#include "uipaint.h"
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <math.h>
#include <commctrl.h>
#include <commdlg.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")   /* AlphaBlend：预览贴抗锯齿图形用 */

/* ================= IMEIndicator 设置程序 =================
   独立 exe，纯 Win32。分页表单（原型变体 C）：自定义页签 + 预览条 + 表单。

   为什么是独立 exe：
   - 主程序常以管理员运行，设置程序需要能以普通权限启动并写配置；
   - 设置 UI 崩了不该带崩指示器；不跑设置时也不占主程序体积。

   保存流程：写 IMEIndicator.json -> 调 IMEIndicator.exe --restart。
   重启失败（典型：设置程序未提权、主程序是管理员）时提示手动重启 ——
   这是有意接受的降级，不做提权。

   ★ 控件**只创建一次**，切页只改内容与显隐。早先是切页时 Destroy 再重建，
     控件逐个消失/出现，看起来就是自上而下的扫描。现在每个页面各有一个面板
     子窗口，切页时整块面板一次 ShowWindow，视觉上是原子的。

   ★ 布局全部按 96 DPI 的逻辑坐标写死，建完后由 ScaleControls() 统一乘 DPI。
     窗口尺寸用 AdjustWindowRectExForDpi 按**客户区**反推 —— 把逻辑尺寸当整窗
     尺寸用，最底下的状态块会被底部按钮压住。 */

#define ID_SAVE         100
#define ID_SAVE_RESTART 101
#define ID_CLOSE        102
#define ID_ADD_IGNORE   103
#define ID_DEL_IGNORE   104
#define ID_RESET_ALL    105   /* 把所有配置项恢复出厂默认 */
#define ID_PICK_IMAGE   110   /* 110..112：三个状态块各一个「图片…」按钮 */
#define ID_RESET        120   /* 120..135：每项数值一个「重置」按钮 */
#define ID_PREVIEW_STATE 140  /* 预览条右侧的状态切换 */

#define F_ENABLED   1
#define F_INHERIT   2
#define F_COLOR     3
#define F_SHAPE     4
#define F_SIZE      5
#define F_OFFX      6
#define F_OFFY      7
#define F_ALPHA     8
#define F_TRANSIENT 9
/* 尺寸/偏移/透明度的「自定义」勾选框字段（10..13）：不勾=跟随 cn_cn */
#define F_CUSTOM_SIZE  10
#define F_CUSTOM_OFFX  11
#define F_CUSTOM_OFFY  12
#define F_CUSTOM_ALPHA 13
#define F_POLL      20
#define F_TRACK     21
#define F_STRATEGY  22
#define F_TIMEOUT   23
#define F_FULLSCREEN 24
#define F_COMPOSITION 25
#define F_WPSCOM    26
#define F_AUTOHIDE  27

/* 重置按钮按「块 × 字段」编号，这里给出两组的字段顺序 */
static const int kStateFields[4]  = { F_SIZE, F_OFFX, F_OFFY, F_ALPHA };
static const int kGlobalFields[4] = { F_POLL, F_TRACK, F_TIMEOUT, F_AUTOHIDE };

/* 尺寸/偏移/透明度 四项对应的"显式设过"位；顺序与 Block 里的滑块一致 */
static const unsigned kNumBits[4] = { SF_SIZE, SF_OFFSETX, SF_OFFSETY, SF_ALPHA };

/* 「自定义」勾选框字段 -> 对应数值项的 have 位；不是这类字段返回 0 */
static unsigned CustomBit(int field) {
    switch (field) {
    case F_CUSTOM_SIZE:  return SF_SIZE;
    case F_CUSTOM_OFFX:  return SF_OFFSETX;
    case F_CUSTOM_OFFY:  return SF_OFFSETY;
    case F_CUSTOM_ALPHA: return SF_ALPHA;
    }
    return 0;
}

#define PAGE_COUNT  7
#define PAGE_GLOBAL 5
#define PAGE_IGNORE 6

/* 逻辑坐标（96 DPI 基准） */
#define WIN_W      860
#define WIN_H      556
#define TAB_X      14
#define TAB_Y      8
#define TAB_H      28
#define PREVIEW_X  14
#define PREVIEW_Y  42
#define PREVIEW_H  66
/* 预览条右侧的状态切换（下拉 + 左边的说明文字） */
#define PREVIEW_SW_W      160
#define PREVIEW_SW_H      24
#define PREVIEW_LBL_W     56
#define PREVIEW_SW_MARGIN 10
#define CONTENT_Y  116
#define BOTTOM_H   44

static ImeCfg  s_cfg;
static HWND    g_hwnd = NULL;
static HWND    g_preview = NULL;
static HWND    g_swState = NULL;   /* 预览的状态切换下拉（预览条的子窗口） */
static int     g_page = 0;
static int     g_selState = 0;
static HFONT   g_font = NULL;
static HBRUSH  g_whiteBrush = NULL;
static int     g_dirty = 0;
static int     g_dpi = 96;
static UINT    g_startupDpi = 96;
static int     g_loadedWidth = 0;   /* 启动时从配置里恢复的窗口宽度（逻辑像素） */
static int     g_widthDirty = 0;    /* 用户改过窗口宽度：关闭时补写进配置 */

/* ---------- 页定义 ---------- */
typedef struct {
    const WCHAR* name;
    ImeState     states[3];
} PageDef;

static const PageDef kPages[PAGE_COUNT] = {
    /* 中文输入法排第一：绝大多数人只调它，打开设置默认就落在这一页。
       其余顺序不变；「全局」「黑名单」必须在最后（PAGE_GLOBAL / PAGE_IGNORE）。 */
    { L"中文输入法", { IMEST_CN_CN, IMEST_CN_EN, IMEST_CN_CAPS } },
    { L"美式键盘",   { IMEST_US, IMEST_US_CAPS, (ImeState)-1 } },
    { L"日文输入法", { IMEST_JP, IMEST_JP_CAPS, (ImeState)-1 } },
    { L"韩文输入法", { IMEST_KR, IMEST_KR_CAPS, (ImeState)-1 } },
    { L"其他布局",   { IMEST_OTHER, IMEST_OTHER_CAPS, (ImeState)-1 } },
    { L"全局",       { (ImeState)-1, (ImeState)-1, (ImeState)-1 } },
    { L"黑名单",     { (ImeState)-1, (ImeState)-1, (ImeState)-1 } },
};

static const WCHAR* StateLabel(ImeState s) {
    switch (s) {
    case IMEST_CN_CN: return L"中文";
    case IMEST_CN_EN: return L"英文";
    case IMEST_US_CAPS: case IMEST_CN_CAPS: case IMEST_JP_CAPS:
    case IMEST_KR_CAPS: case IMEST_OTHER_CAPS: return L"大写";
    default: return L"普通";
    }
}

static int IsCapsState(ImeState s) {
    return s == IMEST_US_CAPS || s == IMEST_CN_CAPS || s == IMEST_JP_CAPS ||
           s == IMEST_KR_CAPS || s == IMEST_OTHER_CAPS;
}

static int CapsBase(ImeState s) {
    switch (s) {
    case IMEST_US_CAPS:    return IMEST_US;
    case IMEST_CN_CAPS:    return IMEST_CN_CN;
    case IMEST_JP_CAPS:    return IMEST_JP;
    case IMEST_KR_CAPS:    return IMEST_KR;
    case IMEST_OTHER_CAPS: return IMEST_OTHER;
    default:               return -1;
    }
}

static int S(int v) {
    if (g_dpi <= 0) g_dpi = 96;
    return MulDiv(v, g_dpi, 96);
}

/* 客户区尺寸 -> 整窗尺寸（含标题栏/边框）。按传入 DPI 算：AdjustWindowRectEx
   用的是系统 DPI，150%/175% 屏上会把最小尺寸算小。 */
static void ClientToWindowSize(RECT* rc, DWORD style, DWORD exstyle, UINT dpi) {
    typedef BOOL (WINAPI* PFN_AdjForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static PFN_AdjForDpi pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) pfn = (PFN_AdjForDpi)GetProcAddress(u, "AdjustWindowRectExForDpi");
    }
    BOOL ok = FALSE;
    if (pfn) ok = pfn(rc, style, FALSE, exstyle, dpi);
    if (!ok) AdjustWindowRectEx(rc, style, FALSE, exstyle);
}

static HFONT g_fontBold = NULL;

/* 字体：Microsoft YaHei UI 9pt + ClearType。系统消息字体在中文 Windows 上
   也是雅黑，但英文系统会落到 Segoe UI，中英混排的字重/字宽不一致；这里
   显式指定，保证各机器上观感一致。粗体用真 Bold 字面（msyhbd.ttc）。 */
static BYTE FontQuality(void) {
    BOOL smooth = FALSE;
    SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smooth, 0);
    return smooth ? CLEARTYPE_QUALITY : NONANTIALIASED_QUALITY;
}

static HFONT MakeFont(int bold) {
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(9, g_dpi > 0 ? g_dpi : 96, 72);
    lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = FontQuality();
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    lstrcpynW(lf.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE);
    HFONT f = CreateFontIndirectW(&lf);
    if (!f) f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    return f;
}

static HFONT BaseFont(void) {
    if (!g_font) g_font = MakeFont(0);
    return g_font;
}

static HFONT BoldFont(void) {
    if (!g_fontBold) g_fontBold = MakeFont(1);
    return g_fontBold;
}

/* ---------- 抗锯齿小工具 ----------
   GDI 的 RoundRect/Ellipse/LineTo 是硬边光栅化，圆角和圆形在高 DPI 下会露出
   台阶（滑块圆点尤其明显）。所有圆角/圆形/粗线都改走 uipaint 的覆盖率图层，
   内部还是同一个 HDC，只是先画进 DIB 再整块贴上去。 */
static DWORD RgbToPix(COLORREF c) {
    return ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | (DWORD)GetBValue(c);
}

/* 圆角矩形：先按边色画整块，再按填充色画内缩 1px 的内芯 —— 两层都有抗锯齿，
   边线不会因为缩放变成虚线。建不出 DIB 时退回 GDI 硬边，功能优先。 */
static void PaintRoundRectAA(HDC dc, const RECT* rc, int radius,
                             COLORREF border, COLORREF fill) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    if (w <= 0 || h <= 0) return;
    UILayer lay;
    if (UILayerBegin(&lay, dc, w, h)) {
        UIPixRoundRect(lay.px, w, h, 0, 0, w, h, radius, RgbToPix(border), 255);
        UIPixRoundRect(lay.px, w, h, 1, 1, w - 1, h - 1,
                       radius > 1 ? radius - 1 : radius, RgbToPix(fill), 255);
        UILayerBlend(&lay, dc, rc->left, rc->top);
        UILayerEnd(&lay);
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, br);
    RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

/* 抗锯齿粗线（下拉箭头这种小折线） */
static void PaintLineAA(HDC dc, POINT a, POINT b, double halfWidth, COLORREF c) {
    int pad = (int)(halfWidth + 1.5);
    int x0 = (a.x < b.x ? a.x : b.x) - pad;
    int y0 = (a.y < b.y ? a.y : b.y) - pad;
    int x1 = (a.x > b.x ? a.x : b.x) + pad;
    int y1 = (a.y > b.y ? a.y : b.y) + pad;
    UILayer lay;
    if (UILayerBegin(&lay, dc, x1 - x0, y1 - y0)) {
        UIPixCapsule(lay.px, x1 - x0, y1 - y0,
                     a.x - x0, a.y - y0, b.x - x0, b.y - y0,
                     halfWidth, RgbToPix(c), 255);
        UILayerBlend(&lay, dc, x0, y0);
        UILayerEnd(&lay);
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, (int)(halfWidth * 2), c);
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, a.x, a.y, NULL);
    LineTo(dc, b.x, b.y);
    SelectObject(dc, op);
    DeleteObject(pen);
}

/* 双缓冲：把整块控件画进内存位图，最后一次性贴回。
   自绘控件里既有背景填充又有图层混合，直接画到窗口 DC 上，鼠标一移动、
   悬停高亮一变，屏幕就会亮一帧空白再补上内容 —— 表现得像闪烁。 */
typedef struct {
    HDC     mem;
    HBITMAP bmp;
    HGDIOBJ old;
    int     w, h;
} BufDC;

static int BufBegin(BufDC* b, HDC ref, int w, int h) {
    ZeroMemory(b, sizeof(*b));
    if (w <= 0 || h <= 0) return 0;
    b->mem = CreateCompatibleDC(ref);
    if (!b->mem) return 0;
    b->bmp = CreateCompatibleBitmap(ref, w, h);
    if (!b->bmp) {
        DeleteDC(b->mem);
        ZeroMemory(b, sizeof(*b));
        return 0;
    }
    b->old = SelectObject(b->mem, b->bmp);
    b->w = w;
    b->h = h;
    return 1;
}

static void BufEnd(BufDC* b, HDC dc) {
    if (!b || !b->mem) return;
    BitBlt(dc, 0, 0, b->w, b->h, b->mem, 0, 0, SRCCOPY);
    SelectObject(b->mem, b->old);
    DeleteObject(b->bmp);
    DeleteDC(b->mem);
    ZeroMemory(b, sizeof(*b));
}

/* ---------- 自绘按钮 ----------
   参考 CapsEnhance 设置程序的观感：圆角、主按钮蓝底白字、次按钮浅底灰边，
   悬停淡蓝、按下加深。系统默认按钮是 Win95 的浮凸方块，和现在的页签/预览
   放在一起很割裂。 */
#define BTN_HOVER_PROP L"IMEIndicatorBtnHover"
#define CHK_PROP       L"IMEIndicatorCheck"      /* 是自绘复选框 */
#define CHK_STATE_PROP L"IMEIndicatorCheckState" /* 复选框的勾选状态 */
#define HINT_PROP      L"IMEIndicatorHint"       /* 悬停说明的文字（静态字面量） */
#define HINT_CLASS     L"IMEIndicatorHintWnd"
#define HINT_TIMER     1
#define HINT_DELAY_MS  400

/* ---------- 悬停说明气泡 ----------
   自绘控件用不上普通工具提示的那套：comctl32 的 TTF_SUBCLASS 挂在 ownerdraw
   按钮上不会弹出。这里自己开一个小弹出窗：鼠标停住 400ms 后贴在控件下方，
   离开或按下就收。文字用静态字面量，窗口只存指针。 */
static HWND g_hint = NULL;

static void HintHide(void) {
    if (g_hint) {
        DestroyWindow(g_hint);
        g_hint = NULL;
    }
}

static LRESULT CALLBACK HintProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        /* 白底 + 1px 灰边，跟界面里其它自绘控件同一套配色（不用系统提示的淡黄） */
        HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xAD, 0xAD, 0xAD));
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, rc.right, rc.bottom);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);

        const WCHAR* t = (const WCHAR*)GetPropW(h, HINT_PROP);
        if (t) {
            RECT tr = rc;
            InflateRect(&tr, -S(10), -S(6));
            HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
            if (!f) f = BaseFont();
            HGDIOBJ of = SelectObject(dc, f);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0x33, 0x33, 0x33));
            DrawTextW(dc, t, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK);
            SelectObject(dc, of);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static void HintShow(HWND owner, const WCHAR* text) {
    HintHide();
    if (!owner || !text) return;

    RECT wr;
    GetWindowRect(owner, &wr);

    /* 先量文字要多大（宽度有上限，超了自动折行） */
    int maxW = S(300);
    RECT calc = { 0, 0, maxW - S(16), 0 };
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, BaseFont());
    DrawTextW(dc, text, -1, &calc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
    SelectObject(dc, of);
    ReleaseDC(NULL, dc);

    int w = calc.right + S(10) * 2;
    int ht = calc.bottom + S(6) * 2;
    int x = wr.left;
    int y = wr.bottom + S(4);

    /* 贴边时收回屏幕内；下方不够高就翻到控件上方 */
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(mon, &mi)) {
        if (y + ht > mi.rcWork.bottom) y = wr.top - S(4) - ht;
        if (x + w > mi.rcWork.right)   x = mi.rcWork.right - w;
        if (x < mi.rcWork.left)        x = mi.rcWork.left;
        if (y < mi.rcWork.top)         y = mi.rcWork.top;
    }

    g_hint = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, HINT_CLASS, NULL,
                             WS_POPUP, x, y, w, ht, g_hwnd, NULL,
                             GetModuleHandleW(NULL), NULL);
    if (!g_hint) return;
    SetPropW(g_hint, HINT_PROP, (HANDLE)text);
    SendMessageW(g_hint, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    ShowWindow(g_hint, SW_SHOWNOACTIVATE);
    InvalidateRect(g_hint, NULL, FALSE);
}

static LRESULT CALLBACK BtnSub(HWND h, UINT m, WPARAM w, LPARAM l,
                               UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    /* BS_OWNERDRAW 的按钮不保存勾选状态，BM_SETCHECK/BM_GETCHECK 由这里接管 */
    if (GetPropW(h, CHK_PROP)) {
        if (m == BM_SETCHECK) {
            SetPropW(h, CHK_STATE_PROP, (HANDLE)(INT_PTR)(w == BST_CHECKED ? 1 : 0));
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (m == BM_GETCHECK)
            return GetPropW(h, CHK_STATE_PROP) ? BST_CHECKED : BST_UNCHECKED;
    }
    switch (m) {
    /* 自绘收全部像素，别让系统先拿类背景刷擦一遍 —— 那层灰底就是悬停闪的源头 */
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
        if (!GetPropW(h, BTN_HOVER_PROP)) {
            SetPropW(h, BTN_HOVER_PROP, (HANDLE)1);
            TRACKMOUSEEVENT t;
            ZeroMemory(&t, sizeof(t));
            t.cbSize = sizeof(t);
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            InvalidateRect(h, NULL, FALSE);
        }
        /* 每次移动都重新计时：停下来才算"悬停"，划过去不弹 */
        if (GetPropW(h, HINT_PROP) && !g_hint)
            SetTimer(h, HINT_TIMER, HINT_DELAY_MS, NULL);
        break;
    case WM_TIMER:
        if (w == HINT_TIMER) {
            KillTimer(h, HINT_TIMER);
            const WCHAR* t = (const WCHAR*)GetPropW(h, HINT_PROP);
            if (t && GetPropW(h, BTN_HOVER_PROP) && !g_hint) HintShow(h, t);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        /* 不吞掉消息：按钮还要靠它发 BN_CLICKED */
        KillTimer(h, HINT_TIMER);
        HintHide();
        break;
    case WM_MOUSELEAVE:
        KillTimer(h, HINT_TIMER);
        HintHide();
        RemovePropW(h, BTN_HOVER_PROP);
        InvalidateRect(h, NULL, FALSE);
        break;
    case WM_NCDESTROY:
        KillTimer(h, HINT_TIMER);
        HintHide();
        RemovePropW(h, BTN_HOVER_PROP);
        RemovePropW(h, CHK_STATE_PROP);
        RemoveWindowSubclass(h, BtnSub, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

static void MakePushButton(HWND h) {
    SetWindowSubclass(h, BtnSub, 1, 0);
}

/* 先铺父窗口底色，圆角外的四个角才像"透明"的 */
static void PaintPushButton(const DRAWITEMSTRUCT* di) {
    HWND h = di->hwndItem;
    RECT rc = di->rcItem;
    BOOL primary = (GetDlgCtrlID(h) == ID_SAVE_RESTART);
    BOOL hover = GetPropW(h, BTN_HOVER_PROP) != NULL;
    BOOL pressed = (di->itemState & ODS_SELECTED) != 0;
    BOOL disabled = (di->itemState & ODS_DISABLED) != 0;

    HBRUSH bg = CreateSolidBrush(GetParent(h) == g_hwnd
                                     ? RGB(0xF3, 0xF3, 0xF3)
                                     : RGB(0xFF, 0xFF, 0xFF));
    FillRect(di->hDC, &rc, bg);
    DeleteObject(bg);

    COLORREF fill, border, text;
    if (disabled) {
        fill = RGB(0xF6, 0xF6, 0xF6);
        border = RGB(0xBD, 0xBD, 0xBD);
        text = RGB(0xA0, 0xA0, 0xA0);
    } else if (primary) {
        fill = pressed ? RGB(0x00, 0x5A, 0x9E)
                       : hover ? RGB(0x1A, 0x86, 0xD9) : RGB(0x00, 0x78, 0xD7);
        border = fill;
        text = RGB(0xFF, 0xFF, 0xFF);
    } else {
        fill = pressed ? RGB(0xCC, 0xE4, 0xF7)
                       : hover ? RGB(0xE5, 0xF1, 0xFB) : RGB(0xFC, 0xFC, 0xFC);
        border = pressed ? RGB(0x00, 0x54, 0x99)
                         : hover ? RGB(0x00, 0x78, 0xD7) : RGB(0xAD, 0xAD, 0xAD);
        text = RGB(0x00, 0x00, 0x00);
    }

    int rad = S(4);
    PaintRoundRectAA(di->hDC, &rc, rad, border, fill);

    WCHAR txt[96];
    GetWindowTextW(h, txt, 96);
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (!f) f = BaseFont();
    HGDIOBJ of = SelectObject(di->hDC, f);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, text);
    RECT tr = rc;
    if (pressed && !primary) OffsetRect(&tr, 0, S(1));
    DrawTextW(di->hDC, txt, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(di->hDC, of);
}

/* 自绘复选框：16px 圆角方块 + 白色对勾，悬停描边变蓝。
   BS_OWNERDRAW 不会自动切换勾选状态，点击时由 WM_COMMAND 里手动翻转。 */
static void PaintCheck(const DRAWITEMSTRUCT* di) {
    HWND h = di->hwndItem;
    RECT rc = di->rcItem;
    BOOL checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    BOOL hover = GetPropW(h, BTN_HOVER_PROP) != NULL;
    BOOL disabled = (di->itemState & ODS_DISABLED) != 0;

    HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(di->hDC, &rc, bg);
    DeleteObject(bg);

    int box = S(16);
    int by = rc.top + (rc.bottom - rc.top - box) / 2;
    RECT br = { rc.left, by, rc.left + box, by + box };

    COLORREF fill, border;
    if (disabled) {
        fill = RGB(0xF6, 0xF6, 0xF6);
        border = RGB(0xBD, 0xBD, 0xBD);
    } else if (checked) {
        fill = RGB(0x00, 0x78, 0xD7);
        border = fill;
    } else {
        fill = RGB(0xFF, 0xFF, 0xFF);
        border = hover ? RGB(0x00, 0x78, 0xD7) : RGB(0xAD, 0xAD, 0xAD);
    }

    /* 方块和对勾一起画进同一块图层，只贴一次 */
    int lw = rc.right - rc.left, lh = rc.bottom - rc.top;
    int lx = br.left - rc.left, ly = br.top - rc.top;
    int rad = box * 3 / 16;                 /* 96 DPI 下 16px 方块圆角 3px */
    UILayer lay;
    if (lw > 0 && lh > 0 && UILayerBegin(&lay, di->hDC, lw, lh)) {
        UIPixRoundRect(lay.px, lw, lh, lx, ly, lx + box, ly + box,
                       rad, RgbToPix(border), 255);
        UIPixRoundRect(lay.px, lw, lh, lx + 1, ly + 1, lx + box - 1, ly + box - 1,
                       rad > 1 ? rad - 1 : rad, RgbToPix(fill), 255);
        if (checked) {
            double hw = S(2) / 2.0;         /* 96 DPI 下笔画宽 2px */
            DWORD white = RgbToPix(RGB(0xFF, 0xFF, 0xFF));
            UIPixCapsule(lay.px, lw, lh, lx + S(4), ly + S(8),
                         lx + S(7), ly + S(11), hw, white, 255);
            UIPixCapsule(lay.px, lw, lh, lx + S(7), ly + S(11),
                         lx + S(12), ly + S(5), hw, white, 255);
        }
        UILayerBlend(&lay, di->hDC, 0, 0);
        UILayerEnd(&lay);
    } else {
        HBRUSH b = CreateSolidBrush(fill);
        HPEN p = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ ob = SelectObject(di->hDC, b);
        HGDIOBJ op = SelectObject(di->hDC, p);
        RoundRect(di->hDC, br.left, br.top, br.right, br.bottom, S(6), S(6));
        SelectObject(di->hDC, ob);
        SelectObject(di->hDC, op);
        DeleteObject(b);
        DeleteObject(p);

        if (checked) {
            HPEN cp = CreatePen(PS_SOLID, S(2), RGB(0xFF, 0xFF, 0xFF));
            HGDIOBJ oc = SelectObject(di->hDC, cp);
            POINT pts[3] = {
                { br.left + S(4),  br.top + S(8) },
                { br.left + S(7),  br.top + S(11) },
                { br.left + S(12), br.top + S(5) },
            };
            Polyline(di->hDC, pts, 3);
            SelectObject(di->hDC, oc);
            DeleteObject(cp);
        }
    }

    WCHAR txt[128];
    GetWindowTextW(h, txt, 128);
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (!f) f = BaseFont();
    HGDIOBJ of = SelectObject(di->hDC, f);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? RGB(0xA0, 0xA0, 0xA0) : RGB(0x00, 0x00, 0x00));
    RECT tr = rc;
    tr.left += box + S(6);
    DrawTextW(di->hDC, txt, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(di->hDC, of);
}

/* ---------- 控件元数据（建一次，用于按 hwnd 反查字段 / 统一缩放） ---------- */
typedef struct {
    HWND hwnd;
    int  block;   /* 布局块 0..2；-1 全局页；-2 纯装饰 */
    int  field;
    HWND label;
    int  x, y, w, h;
} Meta;

static Meta g_meta[200];
static int  g_metaCount = 0;

static void RegMeta(HWND h, int block, int field, HWND label, int x, int y, int w, int hh) {
    if (!h || g_metaCount >= (int)(sizeof(g_meta) / sizeof(g_meta[0]))) return;
    Meta* m = &g_meta[g_metaCount++];
    m->hwnd = h; m->block = block; m->field = field; m->label = label;
    m->x = x; m->y = y; m->w = w; m->h = hh;
}

static Meta* FindMeta(HWND h) {
    for (int i = 0; i < g_metaCount; i++) if (g_meta[i].hwnd == h) return &g_meta[i];
    return NULL;
}

/* 数值框与滑块双向同步：程序改文本时置位，避免 EN_CHANGE 反过来又写一遍 */
static int g_syncEdit = 0;

static HWND SliderFor(int block, int field);
static void ApplySliderValue(int block, int field, int v);
static int  DefaultFor(int block, int field);
static void SetSliderValue(int block, int field, int v);

static int SliderPos(HWND s) { return (int)SendMessageW(s, TBM_GETPOS, 0, 0); }

/* 设位置并立刻重画。redraw 一律传 FALSE：让 trackbar 自己失效一次会先擦一遍
   系统灰底再等我们铺白，拖动时那一下就闪；这里只标记脏区，我们自己画。 */
static void SetSliderPos(HWND sl, int v) {
    if (!sl) return;
    SendMessageW(sl, TBM_SETPOS, FALSE, (LPARAM)v);
    InvalidateRect(sl, NULL, FALSE);
}

static void SetNumText(HWND e, int v) {
    if (!e) return;
    WCHAR vb[32];
    _snwprintf_s(vb, 32, _TRUNCATE, L"%d", v);
    /* 内容没变就别 SetWindowText：编辑框会重画一遍整行文字，滚动时白费 */
    WCHAR cur[32];
    if (GetWindowTextW(e, cur, 32) > 0 && wcscmp(cur, vb) == 0) return;
    g_syncEdit = 1;
    SetWindowTextW(e, vb);
    g_syncEdit = 0;
}

static void ScaleControls(void) {
    for (int i = 0; i < g_metaCount; i++) {
        Meta* m = &g_meta[i];
        if (!m->hwnd || !IsWindow(m->hwnd)) continue;
        SetWindowPos(m->hwnd, NULL, S(m->x), S(m->y), S(m->w), S(m->h),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* ---------- 面板 ---------- */
/* 图片文件名标签：文件缺失/解码失败时标红（WM_CTLCOLORSTATIC 里用）。
   放在这里是因为面板过程要读，而 Block 结构在后面才定义。 */
static HWND g_imgLabel[3];
static int  g_imgBad[3];

/* 「颜色」标签：形状=图片时图片不染色，颜色项整行置灰（STATIC 的
   EnableWindow 不改字色，所以要在 WM_CTLCOLORSTATIC 里自己换） */
static HWND g_colorLabel[3];
static int  g_colorDim[3];

/* 四个数值项的标签：跟随 cn_cn 时置灰（STATIC 的 EnableWindow 不改字色，
   所以要在 WM_CTLCOLORSTATIC 里自己换） */
static HWND g_customLabel[3][4];
static int  g_customDim[3][4];

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    /* 控件的命令/滚动/自绘通知发到**父窗口**（现在父窗口是面板），
       统一转给主窗口处理，逻辑只写一份。 */
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_DRAWITEM:
        return SendMessageW(g_hwnd, m, w, l);
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, g_whiteBrush);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)w;
        SetBkColor(dc, RGB(0xFF, 0xFF, 0xFF));
        SetTextColor(dc, RGB(0x00, 0x00, 0x00));
        HWND ctl = (HWND)l;
        for (int i = 0; i < 3; i++) {
            if (ctl == g_imgLabel[i] && g_imgBad[i]) {
                SetTextColor(dc, RGB(0xC0, 0x00, 0x00));
                break;
            }
        }
        /* 形状=图片时颜色项不生效：标签跟色块一起变灰 */
        for (int i = 0; i < 3; i++) {
            if (ctl == g_colorLabel[i] && g_colorDim[i]) {
                SetTextColor(dc, RGB(0xA0, 0xA0, 0xA0));
                break;
            }
        }
        /* 跟随 cn_cn 的项：标签跟滑块一起变灰，一眼看出这行不生效 */
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 4; k++)
                if (ctl == g_customLabel[i][k] && g_customDim[i][k])
                    SetTextColor(dc, RGB(0xA0, 0xA0, 0xA0));
        return (LRESULT)g_whiteBrush;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void RegisterPanelClass(HINSTANCE hi) {
    static const WCHAR cls[] = L"IMEIndicatorPanel";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = hi;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
}

static HWND MakePanel(void) {
    return CreateWindowExW(0, L"IMEIndicatorPanel", NULL,
                           WS_CHILD | WS_CLIPCHILDREN, 0, 0, 100, 100,
                           g_hwnd, NULL, NULL, NULL);
}

/* ---------- 控件工厂 ---------- */
static HWND P;   /* 当前创建控件用的父面板 */

static HWND CLabel(const WCHAR* t, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             x, y, w, h, P, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(c, -2, 0, NULL, x, y, w, h);
    return c;
}

static HWND CCheck(const WCHAR* t, int x, int y, int w, int h,
                   int block, int field, int checked) {
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             x, y, w, h, P, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    /* 先把属性与子类装上，BM_SETCHECK 才拦得到（拦截逻辑见 BtnSub） */
    SetPropW(c, CHK_PROP, (HANDLE)1);
    MakePushButton(c);          /* 复用悬停跟踪 */
    SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    RegMeta(c, block, field, NULL, x, y, w, h);
    return c;
}

static HWND CButton(const WCHAR* t, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             x, y, w, h, P, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    MakePushButton(c);
    RegMeta(c, -2, 0, NULL, x, y, w, h);
    return c;
}

static HWND CEdit(int x, int y, int w, int h) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             x, y, w, h, P, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(c, -2, 0, NULL, x, y, w, h);
    return c;
}

/* 数值框：直接输入，和滑块双向同步（滑块手抖时靠它精确调）。
   不用 ES_NUMBER：偏移量允许负数，而 ES_NUMBER 连负号一起挡掉；
   非法字符由失焦时的回填纠正。 */
static HWND CEditNum(int x, int y, int w, int h, int block, int field) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                             WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
                             x, y, w, h, P, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(c, block, field, NULL, x, y, w, h);
    return c;
}

/* ---------- 自绘下拉（替代系统 COMBOBOX） ----------
   系统下拉的 3D 边框与方形箭头改不掉，和圆角按钮/下划线页签放一起很割裂。
   这里自绘：闭合态是圆角框 + 当前项 + 右侧箭头，点开弹出一份圆角列表
   （悬停高亮、当前项蓝色文字）。
   对外仍认 CB_ADDSTRING / CB_SETCURSEL / CB_GETCURSEL，调用处不用改。 */
#define SEL_MAX_ITEMS 16
#define SEL_MAX_TEXT  32
#define SEL_ITEM_H    26
#define SEL_CLASS     L"IMEIndicatorSelect"
#define SEL_POPUP     L"IMEIndicatorSelectPopup"

typedef struct {
    int   block, field;
    int   count, sel, hot;
    WCHAR items[SEL_MAX_ITEMS][SEL_MAX_TEXT];
} SelData;

static HWND g_selPopup = NULL;
static HWND g_selOwner = NULL;

static SelData* SelOf(HWND h) {
    return (SelData*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

static void CloseSelPopup(void) {
    HWND p = g_selPopup;
    g_selPopup = NULL;
    g_selOwner = NULL;
    if (!p) return;
    if (GetCapture() == p) ReleaseCapture();
    DestroyWindow(p);
}

static void SelPaint(HWND h, HDC dc) {
    SelData* d = SelOf(h);
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    BOOL enabled = IsWindowEnabled(h);
    BOOL hover = enabled && GetPropW(h, BTN_HOVER_PROP) != NULL;
    COLORREF border = !enabled ? RGB(0xD0, 0xD0, 0xD0)
                    : hover ? RGB(0x00, 0x78, 0xD7) : RGB(0xAD, 0xAD, 0xAD);
    RECT brc = { rc.left, rc.top, rc.right, rc.bottom };
    PaintRoundRectAA(dc, &brc, S(3), border, RGB(0xFF, 0xFF, 0xFF));

    int ax = rc.right - S(16), ay = (rc.top + rc.bottom) / 2;
    COLORREF ac = enabled ? RGB(0x54, 0x54, 0x54) : RGB(0xB0, 0xB0, 0xB0);
    POINT a1 = { ax - S(4), ay - S(2) };
    POINT a2 = { ax, ay + S(2) };
    POINT a3 = { ax + S(4), ay - S(2) };
    PaintLineAA(dc, a1, a2, S(2) / 2.0, ac);
    PaintLineAA(dc, a2, a3, S(2) / 2.0, ac);

    if (d && d->count > 0) {
        HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        if (!f) f = BaseFont();
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? RGB(0x00, 0x00, 0x00) : RGB(0xA0, 0xA0, 0xA0));
        RECT tr = rc;
        tr.left += S(10);
        tr.right -= S(26);
        DrawTextW(dc, d->items[d->sel], -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);
    }
}

static void SelPopupPaint(HWND h, HDC dc) {
    SelData* d = SelOf(h);
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    /* 圆角外缘由窗口区域裁剪，那条边本来就是硬的；这里只画 1px 内边框。
       （别在这里叠抗锯齿图层：整个列表每次鼠标移动都要重画一遍，白算。） */
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xAD, 0xAD, 0xAD));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 0, 0, rc.right - 1, rc.bottom - 1, S(8), S(8));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);

    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (!f) f = BaseFont();
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    int ih = S(SEL_ITEM_H);
    for (int i = 0; d && i < d->count; i++) {
        RECT r = { S(4), S(4) + i * ih, rc.right - S(4), S(4) + (i + 1) * ih };
        if (i == d->hot) {
            HBRUSH hb = CreateSolidBrush(RGB(0xE5, 0xF1, 0xFB));
            FillRect(dc, &r, hb);
            DeleteObject(hb);
        }
        SetTextColor(dc, i == d->sel ? RGB(0x00, 0x5A, 0x9E) : RGB(0x00, 0x00, 0x00));
        RECT tr = r;
        tr.left += S(8);
        DrawTextW(dc, d->items[i], -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, of);
}

static LRESULT CALLBACK SelPopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    SelData* d = SelOf(h);
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        /* 双缓冲：悬停高亮在列表里上下移动时不能闪 */
        BufDC buf;
        if (BufBegin(&buf, dc, rc.right, rc.bottom)) {
            SelPopupPaint(h, buf.mem);
            BufEnd(&buf, dc);
        } else {
            SelPopupPaint(h, dc);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!d) return 0;
        int ih = S(SEL_ITEM_H);
        int y = (short)HIWORD(l) - S(4);
        int i = (y < 0) ? -1 : y / ih;
        if (i >= d->count) i = -1;
        if (i != d->hot) {
            d->hot = i;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!d) return 0;
        int ih = S(SEL_ITEM_H);
        int y = (short)HIWORD(l) - S(4);
        int i = (y < 0) ? -1 : y / ih;
        if (i >= 0 && i < d->count) {
            d->sel = i;
            HWND owner = g_selOwner;
            CloseSelPopup();
            if (owner) {
                InvalidateRect(owner, NULL, FALSE);
                SendMessageW(GetParent(owner), WM_COMMAND,
                             MAKEWPARAM(GetDlgCtrlID(owner), CBN_SELCHANGE), (LPARAM)owner);
            }
        } else {
            CloseSelPopup();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_selPopup && (HWND)l != h) CloseSelPopup();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void OpenSelPopup(HWND owner) {
    SelData* d = SelOf(owner);
    if (!d || d->count <= 0 || !IsWindowEnabled(owner)) return;
    if (g_selPopup) CloseSelPopup();

    RECT wr;
    GetWindowRect(owner, &wr);
    int ih = S(SEL_ITEM_H);
    int w = wr.right - wr.left;
    int h = d->count * ih + S(8);

    g_selPopup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, SEL_POPUP, NULL,
                                 WS_POPUP, wr.left, wr.bottom, w, h,
                                 owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_selPopup) return;
    g_selOwner = owner;
    d->hot = -1;
    SetWindowLongPtrW(g_selPopup, GWLP_USERDATA, (LONG_PTR)d);
    SendMessageW(g_selPopup, WM_SETFONT, (WPARAM)BaseFont(), TRUE);

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, S(8), S(8));
    SetWindowRgn(g_selPopup, rgn, TRUE);

    ShowWindow(g_selPopup, SW_SHOWNOACTIVATE);
    SetCapture(g_selPopup);
    InvalidateRect(g_selPopup, NULL, FALSE);
}

static LRESULT CALLBACK SelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    SelData* d = SelOf(h);
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        BufDC buf;
        if (BufBegin(&buf, dc, rc.right, rc.bottom)) {
            SelPaint(h, buf.mem);
            BufEnd(&buf, dc);
        } else {
            SelPaint(h, dc);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case CB_ADDSTRING:
        if (d && d->count < SEL_MAX_ITEMS) {
            lstrcpynW(d->items[d->count], (const WCHAR*)l, SEL_MAX_TEXT);
            int idx = d->count++;
            InvalidateRect(h, NULL, FALSE);
            return idx;
        }
        return CB_ERR;
    case CB_SETCURSEL:
        if (d && (int)w >= 0 && (int)w < d->count) {
            d->sel = (int)w;
            InvalidateRect(h, NULL, FALSE);
            return d->sel;
        }
        return CB_ERR;
    case CB_GETCURSEL:
        return (d && d->count > 0) ? d->sel : CB_ERR;
    case WM_LBUTTONDOWN:
        OpenSelPopup(h);
        return 0;
    case WM_MOUSEMOVE:
        if (!GetPropW(h, BTN_HOVER_PROP)) {
            SetPropW(h, BTN_HOVER_PROP, (HANDLE)1);
            TRACKMOUSEEVENT t;
            ZeroMemory(&t, sizeof(t));
            t.cbSize = sizeof(t);
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        RemovePropW(h, BTN_HOVER_PROP);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_NCDESTROY:
        if (g_selPopup && g_selOwner == h) CloseSelPopup();
        if (d) {
            free(d);
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- 滑块（自绘 + 自己管拖拽） ----------
   默认 trackbar 是 Win95 的蚀刻槽 + 小方块滑块，和圆角按钮/下划线页签放在
   一起很割裂。所以槽和圆点都自绘。

   鼠标行为也接管，因为默认 trackbar 拖动的手感有两个"莫名其妙"：

   1. 点轨道不是跳到点到的位置，而是按**页步进**跳（0..255 一页就是 51）；
   2. 按住圆点边缘时，控件会把圆心强制吸到光标正中 —— 按下的一瞬间
      值就跳了一段。

   现在的做法：几何只算一份（SliderGeomOf），画出来的圆点就是点得到的圆点；
   按下时记住"光标相对圆心"的偏移再拖，圆点不会先吸附；点轨道则直接跳到该处。
   值按像素四舍五入，同一像素内不会来回翻转。

   绘制整块先画进图层（白底也画进图层）再一次贴屏：原来的 _画白底_ -> _画轨道_
   -> _贴图形_ 是三次往屏幕写，拖动时中间那几帧就是闪的根源。 */
typedef struct {
    double xL, xR;     /* 圆点圆心能到达的水平范围 */
    double cy;         /* 轨道中线 */
    double trackH;     /* 轨道粗 */
    double thumbR;     /* 圆点半径 */
    int    lo, hi;
} SliderGeom;

static void SliderGeomOf(HWND s, const RECT* rc, SliderGeom* g) {
    int lo = (int)SendMessageW(s, TBM_GETRANGEMIN, 0, 0);
    int hi = (int)SendMessageW(s, TBM_GETRANGEMAX, 0, 0);
    g->lo = lo;
    g->hi = (hi > lo) ? hi : lo + 1;
    g->thumbR = S(14) / 2.0;
    g->trackH = S(4) > 0 ? (double)S(4) : 4.0;
    g->cy = (rc->top + rc->bottom) / 2.0;
    g->xL = rc->left + g->thumbR;
    g->xR = rc->right - g->thumbR;
    if (g->xR < g->xL + 1.0) g->xR = g->xL + 1.0;
}

/* 值 -> 圆心横坐标 */
static double SliderValueX(const SliderGeom* g, int v) {
    if (v < g->lo) v = g->lo;
    if (v > g->hi) v = g->hi;
    double t = (double)(v - g->lo) / (double)(g->hi - g->lo);
    return g->xL + t * (g->xR - g->xL);
}

/* 圆心横坐标 -> 值（四舍五入：换算出半像素误差时不会在两个值之间来回跳） */
static int SliderXValue(const SliderGeom* g, double x) {
    double t = (x - g->xL) / (g->xR - g->xL);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return g->lo + (int)(t * (g->hi - g->lo) + 0.5);
}

static void PaintSlider(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w <= 0 || ht <= 0) return;

    SliderGeom g;
    SliderGeomOf(h, &rc, &g);
    BOOL on = IsWindowEnabled(h);
    COLORREF kTrack  = on ? RGB(0xE0, 0xE0, 0xE0) : RGB(0xEC, 0xEC, 0xEC);
    COLORREF kAccent = on ? RGB(0x00, 0x78, 0xD7) : RGB(0xBC, 0xBC, 0xBC);

    double cx  = SliderValueX(&g, SliderPos(h));
    double top = g.cy - g.trackH / 2.0, bot = g.cy + g.trackH / 2.0;

    /* 轨道两端、已选段、圆点画进同一块图层，白底也铺在图层里：整块只往屏幕
       贴一次。GDI 的 Ellipse 在 14px 上锯齿肉眼可见，而悬浮窗里的同一个圆点
       走 shape.c 的覆盖率光栅化，两边不该一个毛糙一个干净。 */
    UILayer lay;
    if (UILayerBegin(&lay, dc, w, ht)) {
        DWORD* px = (DWORD*)lay.px;
        for (int i = 0; i < w * ht; i++) px[i] = 0xFFFFFFFFu;   /* 不透明白，预乘 */
        UIPixRoundRect(lay.px, w, ht, g.xL, top, g.xR, bot,
                       g.trackH / 2.0, RgbToPix(kTrack), 255);
        if (cx > g.xL)
            UIPixRoundRect(lay.px, w, ht, g.xL, top, cx, bot,
                           g.trackH / 2.0, RgbToPix(kAccent), 255);
        UIPixCircle(lay.px, w, ht, cx, g.cy, g.thumbR, RgbToPix(kAccent), 255);
        UILayerBlend(&lay, dc, rc.left, rc.top);
        UILayerEnd(&lay);
        return;
    }

    /* 兜底：建不出 DIB 就退回 GDI 硬边画法 */
    HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    int icy = (int)g.cy;
    RECT track = { (int)g.xL, icy - S(2), (int)g.xR, icy + S(2) };
    HBRUSH gb = CreateSolidBrush(kTrack);
    HPEN gp = CreatePen(PS_SOLID, 1, kTrack);
    HGDIOBJ ob = SelectObject(dc, gb);
    HGDIOBJ op = SelectObject(dc, gp);
    RoundRect(dc, track.left, track.top, track.right, track.bottom, S(4), S(4));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(gb);
    DeleteObject(gp);

    int icx = (int)(cx + 0.5);
    if (icx > track.left) {
        HBRUSH bb = CreateSolidBrush(kAccent);
        HPEN bp = CreatePen(PS_SOLID, 1, kAccent);
        ob = SelectObject(dc, bb);
        op = SelectObject(dc, bp);
        RoundRect(dc, track.left, track.top, icx, track.bottom, S(4), S(4));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(bb);
        DeleteObject(bp);
    }

    int d = S(14);
    HBRUSH b = CreateSolidBrush(kAccent);
    HPEN p = CreatePen(PS_SOLID, 1, kAccent);
    ob = SelectObject(dc, b);
    op = SelectObject(dc, p);
    Ellipse(dc, icx - d / 2, icy - d / 2, icx + d / 2, icy + d / 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

/* 一次拖动只可能有一个滑块在跟（鼠标捕获是全局的），状态就放静态里 */
static HWND   g_dragSlider = NULL;
static double g_dragGrab = 0.0;   /* 按下时光标相对圆心的偏移 */

static void SliderDragTo(HWND h, double x) {
    RECT rc;
    GetClientRect(h, &rc);
    SliderGeom g;
    SliderGeomOf(h, &rc, &g);
    int v = SliderXValue(&g, x);
    if (v == SliderPos(h)) return;                 /* 值没变就别重画 */
    SendMessageW(h, TBM_SETPOS, FALSE, (LPARAM)v);
    InvalidateRect(h, NULL, FALSE);
    /* 通知父窗口（面板再转给主窗口），和 trackbar 自己的滚动通知同一套 */
    SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, v), (LPARAM)h);
}

static LRESULT CALLBACK SliderSub(HWND h, UINT m, WPARAM w, LPARAM l,
                                  UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintSlider(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;                   /* 整块自己画：别让系统先擦一层灰底再画白 */
    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(h, &rc);
        SliderGeom g;
        SliderGeomOf(h, &rc, &g);
        double x = (double)(short)LOWORD(l);
        double cx = SliderValueX(&g, SliderPos(h));
        /* 按在圆点上：记住偏移，圆点不会先吸附到光标正中；
           按在轨道上：直接跳到该处（不做按页步进） */
        g_dragGrab = (fabs(x - cx) <= g.thumbR + S(2)) ? x - cx : 0.0;
        g_dragSlider = h;
        SetFocus(h);
        SetCapture(h);
        SliderDragTo(h, x - g_dragGrab);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragSlider == h && GetCapture() == h) {
            SliderDragTo(h, (double)(short)LOWORD(l) - g_dragGrab);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_dragSlider == h) {
            g_dragSlider = NULL;
            if (GetCapture() == h) ReleaseCapture();
            SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0), (LPARAM)h);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (g_dragSlider == h) g_dragSlider = NULL;
        break;
    case WM_NCDESTROY:
        if (g_dragSlider == h) g_dragSlider = NULL;
        RemoveWindowSubclass(h, SliderSub, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

static HWND CCombo(int x, int y, int w, int h, int block, int field) {
    (void)h;   /* 系统下拉的"展开高度"对自绘控件没有意义 */
    SelData* d = (SelData*)calloc(1, sizeof(SelData));
    HWND c = CreateWindowExW(0, SEL_CLASS, NULL, WS_CHILD | WS_VISIBLE,
                             x, y, w, 22, P, NULL, NULL, NULL);
    if (d) {
        d->block = block;
        d->field = field;
        d->sel = 0;
        d->hot = -1;
        SetWindowLongPtrW(c, GWLP_USERDATA, (LONG_PTR)d);
    }
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(c, block, field, NULL, x, y, w, 22);
    return c;
}

static HWND CColorBtn(int x, int y, int w, int h, int block) {
    HWND c = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             x, y, w, h, P, NULL, NULL, NULL);
    RegMeta(c, block, F_COLOR, NULL, x, y, w, h);
    return c;
}

static HWND CSlider(int x, int y, int w, int h, int block, int field,
                    int lo, int hi, int val, HWND label) {
    HWND c = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
                             WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                             x, y, w, h, P, NULL, NULL, NULL);
    /* ★ 上下限分开设，不用 TBM_SETRANGE + MAKELPARAM：上限超过 32767 时
       MAKELPARAM 拼出来的 32 位值在符号扩展后会被控件当成负数，范围被截成
       lo..lo，滑块就永远卡在最小值。 */
    SendMessageW(c, TBM_SETRANGEMIN, FALSE, (LPARAM)lo);
    SendMessageW(c, TBM_SETRANGEMAX, FALSE, (LPARAM)hi);
    SendMessageW(c, TBM_SETPOS, FALSE, (LPARAM)val);
    SetWindowSubclass(c, SliderSub, 1, 0);
    RegMeta(c, block, field, label, x, y, w, h);
    return c;
}

/* ---------- 布局面板：3 个固定块 ---------- */
typedef struct {
    HWND title, enable, inherit, color, shape;
    HWND imgBtn, imgLabel;
    HWND sep;         /* 块底分隔线：最后一块的分隔线不显示（会变成悬空线） */
    HWND lbl[6];      /* 颜色/形状/尺寸/偏移X/偏移Y/透明度 的静态标签 */
    HWND reset[4];    /* 尺寸/偏移X/偏移Y/透明度 的重置按钮 */
    HWND custom[4];   /* 尺寸/偏移X/偏移Y/透明度 行首的勾选框（勾上=本状态单独设） */
    HWND slSize, vSize, slOffX, vOffX, slOffY, vOffY, slAlpha, vAlpha;
    HWND transient;
} Block;

static Block g_blk[3];
static ImeState g_blkState[3] = { (ImeState)-1, (ImeState)-1, (ImeState)-1 };

/* 每个状态「上一次选择的非图片形状」：形状=图片但没选到可用图片时，
   保存时回退到它，避免存下一个显示不出来的配置。 */
static int g_lastShape[IMEST_COUNT];
static HWND  g_layoutPanel = NULL;
static HWND  g_globalPanel = NULL;
static HWND  g_ignorePanel = NULL;

/* 行首勾选框的悬停说明（静态存储：气泡只存指针）。只讲未勾选时的效果。 */
static WCHAR kHintCustom[] = L"未勾选时，数值跟随「中文输入法 (cn_cn)」";

static void BuildStateBlock(int i, int y) {
    Block* b = &g_blk[i];
    const int x0 = 14;
    const int lx = x0 + 6;
    const int rx = x0 + 428;   /* 右列：两列各 398 宽，列间距 24 */

    b->title = CLabel(L"", lx, y + 2, 150, 20);
    SendMessageW(b->title, WM_SETFONT, (WPARAM)BoldFont(), TRUE);
    b->enable = CCheck(L"启用", lx + 156, y, 60, 22, i, F_ENABLED, 1);
    b->inherit = CCheck(L"跟随基础状态", lx + 220, y, 120, 22, i, F_INHERIT, 0);
    y += 24;

    b->lbl[0] = CLabel(L"颜色", lx, y + 4, 54, 20);
    g_colorLabel[i] = b->lbl[0];
    b->color = CColorBtn(lx + 58, y, 60, 22, i);
    b->lbl[1] = CLabel(L"形状", rx, y + 4, 54, 20);
    b->shape = CCombo(rx + 58, y, 110, 180, i, F_SHAPE);
    SendMessageW(b->shape, CB_ADDSTRING, 0, (LPARAM)L"不显示");
    SendMessageW(b->shape, CB_ADDSTRING, 0, (LPARAM)L"圆");
    SendMessageW(b->shape, CB_ADDSTRING, 0, (LPARAM)L"三角形");
    SendMessageW(b->shape, CB_ADDSTRING, 0, (LPARAM)L"图片");
    /* 「图片…」按钮：选文件 -> 复制进 shapes\ -> 形状自动切到图片。
       注册成 block=i 的控件，跟随基础状态时会被整块置灰。 */
    b->imgBtn = CreateWindowExW(0, L"BUTTON", L"图片…",
                                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                rx + 174, y, 76, 22, P,
                                (HMENU)(INT_PTR)(ID_PICK_IMAGE + i), NULL, NULL);
    SendMessageW(b->imgBtn, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    MakePushButton(b->imgBtn);
    RegMeta(b->imgBtn, i, 0, NULL, rx + 174, y, 76, 22);
    b->imgLabel = CLabel(L"", rx + 256, y + 4, 140, 20);
    g_imgLabel[i] = b->imgLabel;
    y += 26;

    struct { const WCHAR* nm; int field; int custom; int lo, hi; } sl[4] = {
        { L"尺寸",   F_SIZE,  F_CUSTOM_SIZE,   4,  96 },
        { L"偏移 X", F_OFFX,  F_CUSTOM_OFFX, -40,  40 },
        { L"偏移 Y", F_OFFY,  F_CUSTOM_OFFY, -40,  40 },
        { L"透明度", F_ALPHA, F_CUSTOM_ALPHA,  0, 255 },
    };
    HWND* psl[4] = { &b->slSize, &b->slOffX, &b->slOffY, &b->slAlpha };
    HWND* pvl[4] = { &b->vSize,  &b->vOffX,  &b->vOffY,  &b->vAlpha };
    for (int k = 0; k < 4; k++) {
        int cx = (k % 2 == 0) ? lx : rx;
        int cy = y + (k / 2) * 23;
        /* 行首一个**没有文字**的勾选框：不勾 = 跟随中文输入法(cn_cn)的这项值
           （滑块/数值/重置整行置灰），勾上 = 本状态单独设置。含义靠悬停气泡说明。
           挂在 block=i 上，整块跟随基础状态时会一起置灰。 */
        b->custom[k] = CCheck(L"", cx, cy, 18, 22, i, sl[k].custom, 0);
        SetPropW(b->custom[k], HINT_PROP, (HANDLE)kHintCustom);
        b->lbl[2 + k] = CLabel(sl[k].nm, cx + 22, cy + 3, 54, 20);
        g_customLabel[i][k] = b->lbl[2 + k];
        *pvl[k] = CEditNum(cx + 302, cy, 48, 22, i, sl[k].field);
        *psl[k] = CSlider(cx + 78, cy, 220, 22, i, sl[k].field,
                          sl[k].lo, sl[k].hi, sl[k].lo, *pvl[k]);
        b->reset[k] = CreateWindowExW(0, L"BUTTON", L"重置",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      cx + 354, cy, 44, 22, P,
                                      (HMENU)(INT_PTR)(ID_RESET + i * 4 + k), NULL, NULL);
        SendMessageW(b->reset[k], WM_SETFONT, (WPARAM)BaseFont(), TRUE);
        MakePushButton(b->reset[k]);
        RegMeta(b->reset[k], i, 0, NULL, cx + 354, cy, 44, 22);
    }
    y += 46;

    b->transient = CCheck(L"", lx, y, 330, 22, i, F_TRANSIENT, 0);
    y += 22;

    HWND sep = CreateWindowExW(0, L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, x0, y + 2, WIN_W - 28, 2,
        P, NULL, NULL, NULL);
    RegMeta(sep, -2, 0, NULL, x0, y + 2, WIN_W - 28, 2);
    b->sep = sep;
}

/* 隐藏一个块的全部控件 */
static void HideBlock(int i) {
    Block* b = &g_blk[i];
    HWND all[] = { b->title, b->enable, b->inherit, b->color, b->shape,
                   b->imgBtn, b->imgLabel,
                   b->slSize, b->vSize, b->slOffX, b->vOffX,
                   b->slOffY, b->vOffY, b->slAlpha, b->vAlpha, b->transient };
    for (size_t k = 0; k < sizeof(all)/sizeof(all[0]); k++)
        if (all[k]) ShowWindow(all[k], SW_HIDE);
    for (int k = 0; k < 6; k++)
        if (b->lbl[k]) ShowWindow(b->lbl[k], SW_HIDE);
    for (int k = 0; k < 4; k++)
        if (b->reset[k]) ShowWindow(b->reset[k], SW_HIDE);
    for (int k = 0; k < 4; k++)
        if (b->custom[k]) ShowWindow(b->custom[k], SW_HIDE);
    if (b->sep) ShowWindow(b->sep, SW_HIDE);
    g_blkState[i] = (ImeState)-1;
}

static void ShowBlock(int i) {
    Block* b = &g_blk[i];
    HWND all[] = { b->title, b->enable, b->color, b->shape,
                   b->imgBtn, b->imgLabel,
                   b->slSize, b->vSize, b->slOffX, b->vOffX,
                   b->slOffY, b->vOffY, b->slAlpha, b->vAlpha, b->transient };
    for (size_t k = 0; k < sizeof(all)/sizeof(all[0]); k++)
        if (all[k]) ShowWindow(all[k], SW_SHOW);
    for (int k = 0; k < 6; k++)
        if (b->lbl[k]) ShowWindow(b->lbl[k], SW_SHOW);
    for (int k = 0; k < 4; k++)
        if (b->reset[k]) ShowWindow(b->reset[k], SW_SHOW);
    for (int k = 0; k < 4; k++)
        if (b->custom[k]) ShowWindow(b->custom[k], SW_SHOW);
    if (b->sep) ShowWindow(b->sep, SW_SHOW);
    /* 跟随开关只对 caps 状态有意义；非 caps 必须显式隐藏，
       否则会留着上一页的状态继续显示 */
    ShowWindow(b->inherit, IsCapsState(g_blkState[i]) ? SW_SHOW : SW_HIDE);
}

/* 把某个状态的内容填进第 i 块；s < 0 表示该块本页不用 */
static void FillBlock(int i, ImeState s) {
    if ((int)s < 0) { HideBlock(i); return; }
    Block* b = &g_blk[i];
    g_blkState[i] = s;

    StateStyle* raw = &s_cfg.st[s];
    StateStyle eff;
    CfgStateStyle(&s_cfg, s, &eff);
    int isCaps = IsCapsState(s);
    int inherited = isCaps && raw->inherit >= 0;

    WCHAR title[160];
    _snwprintf_s(title, 160, _TRUNCATE, L"%s（%s）", StateLabel(s), CfgStateName(s));
    SetWindowTextW(b->title, title);

    SendMessageW(b->enable, BM_SETCHECK, raw->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(b->inherit, BM_SETCHECK, inherited ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(b->shape, CB_SETCURSEL,
                 (eff.shape == SHAPE_NONE) ? 0 :
                 (eff.shape == SHAPE_TRIANGLE) ? 2 :
                 (eff.shape == SHAPE_IMAGE) ? 3 : 1, 0);

    SetSliderPos(b->slSize, eff.size);
    SetSliderPos(b->slOffX, eff.offsetX);
    SetSliderPos(b->slOffY, eff.offsetY);
    SetSliderPos(b->slAlpha, eff.alpha);
    WCHAR vb[64];
    SetNumText(b->vSize, eff.size);
    SetNumText(b->vOffX, eff.offsetX);
    SetNumText(b->vOffY, eff.offsetY);
    SetNumText(b->vAlpha, eff.alpha);

    if (s_cfg.autoHideMs > 0)
        _snwprintf_s(vb, 64, _TRUNCATE, L"状态变化后只显示 %d ms", s_cfg.autoHideMs);
    else
        _snwprintf_s(vb, 64, _TRUNCATE, L"状态变化后常显（全局已关闭）");
    SetWindowTextW(b->transient, vb);
    SendMessageW(b->transient, BM_SETCHECK, eff.transient ? BST_CHECKED : BST_UNCHECKED, 0);

    ShowBlock(i);

    /* 图片行：只在形状=图片时露文件名与选择按钮；文件缺失/解码失败标红 */
    g_imgBad[i] = 0;
    if (eff.shape == SHAPE_IMAGE) {
        ShowWindow(b->imgBtn, SW_SHOW);
        ShowWindow(b->imgLabel, SW_SHOW);
        ImgView iv;
        WCHAR err[128];
        if (eff.image[0] && ImgGet(eff.image, 64, &iv, err, 128)) {
            SetWindowTextW(b->imgLabel, eff.image);
        } else {
            SetWindowTextW(b->imgLabel, eff.image[0] ? L"图片不可用" : L"未选择图片");
            g_imgBad[i] = 1;
        }
    } else {
        ShowWindow(b->imgBtn, SW_HIDE);
        ShowWindow(b->imgLabel, SW_HIDE);
    }
    InvalidateRect(b->imgLabel, NULL, TRUE);

    /* 跟随中：除「启用 / 跟随」外整块置灰 */
    for (int k = 0; k < g_metaCount; k++) {
        Meta* m = &g_meta[k];
        if (m->block != i) continue;
        if (m->field == F_INHERIT || m->field == F_ENABLED) continue;
        EnableWindow(m->hwnd, inherited ? FALSE : TRUE);
        if (m->label) EnableWindow(m->label, inherited ? FALSE : TRUE);
    }

    /* 形状=图片：图片原样使用、不染色，颜色项整行置灰（跟随中已整块置灰） */
    int colorOn = !inherited && eff.shape != SHAPE_IMAGE;
    EnableWindow(b->color, colorOn ? TRUE : FALSE);
    if (b->lbl[0]) EnableWindow(b->lbl[0], colorOn ? TRUE : FALSE);
    g_colorDim[i] = colorOn ? 0 : 1;
    if (b->lbl[0]) InvalidateRect(b->lbl[0], NULL, FALSE);
    InvalidateRect(b->color, NULL, TRUE);

    /* 行首勾选框：不勾 = 跟随 cn_cn（滑块/数值/重置置灰，标签也变灰）。
       整块跟随基础状态时轮不到它，cn_cn 自己更是来源 —— 都藏掉。 */
    int customizable = (s != IMEST_CN_CN) && !inherited;
    HWND* psl[4] = { &b->slSize, &b->slOffX, &b->slOffY, &b->slAlpha };
    HWND* pvl[4] = { &b->vSize,  &b->vOffX,  &b->vOffY,  &b->vAlpha };
    for (int k = 0; k < 4; k++) {
        if (!b->custom[k]) continue;
        if (!customizable) {
            ShowWindow(b->custom[k], SW_HIDE);
            g_customDim[i][k] = 0;
            continue;
        }
        ShowWindow(b->custom[k], SW_SHOW);
        int custom = !CfgFieldFollowsCn(&s_cfg, s, kNumBits[k]);
        SendMessageW(b->custom[k], BM_SETCHECK,
                     custom ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(b->custom[k], TRUE);
        EnableWindow(*psl[k], custom ? TRUE : FALSE);
        EnableWindow(*pvl[k], custom ? TRUE : FALSE);
        EnableWindow(b->reset[k], custom ? TRUE : FALSE);
        g_customDim[i][k] = custom ? 0 : 1;
        if (b->lbl[2 + k]) InvalidateRect(b->lbl[2 + k], NULL, FALSE);
    }
}

/* 从当前配置同步「上一次选择的形状」：图片本身不算，回落到圆 */
static void SyncLastShapes(void) {
    for (int i = 0; i < IMEST_COUNT; i++) {
        StateStyle eff;
        CfgStateStyle(&s_cfg, (ImeState)i, &eff);
        g_lastShape[i] = (eff.shape == SHAPE_IMAGE) ? SHAPE_CIRCLE : eff.shape;
    }
}

/* 形状=图片但没选到可用图片（没选 / 文件缺失 / 解码失败）时，回退到
   上一次选择的形状。保存前调用：存下一个显示不出来的配置没有意义。
   返回 1 表示有状态被改。 */
static int FallbackBadImages(void) {
    int changed = 0;
    for (int i = 0; i < IMEST_COUNT; i++) {
        StateStyle* raw = &s_cfg.st[i];
        if (raw->inherit >= 0) continue;      /* 跟随基础状态：由基础状态兜底 */
        StateStyle eff;
        CfgStateStyle(&s_cfg, (ImeState)i, &eff);
        if (eff.shape != SHAPE_IMAGE) continue;
        ImgView iv;
        if (eff.image[0] && ImgGet(eff.image, 64, &iv, NULL, 0)) continue;
        int back = g_lastShape[i];
        if (back == SHAPE_IMAGE) back = SHAPE_CIRCLE;
        raw->shape = back;
        raw->have |= SF_SHAPE;
        changed = 1;
    }
    return changed;
}

/* ---------- 全局面板 ---------- */
static HWND g_slPoll, g_vPoll, g_slTrack, g_vTrack, g_slTimeout, g_vTimeout,
            g_slAutoHide, g_vAutoHide, g_cbStrategy, g_ckFull, g_ckComp, g_ckWps;

static void BuildGlobalPanel(void) {
    P = g_globalPanel;
    int y = 0, x0 = 14;
    HWND hDetect = CLabel(L"检测", x0, y, 200, 22);
    SendMessageW(hDetect, WM_SETFONT, (WPARAM)BoldFont(), TRUE);
    y += 24;

    struct { const WCHAR* nm; int field; int lo, hi; HWND* sl; HWND* vl; } sl[4] = {
        { L"状态检测间隔", F_POLL,      5, 2000,  &g_slPoll,    &g_vPoll },
        { L"光标追踪间隔", F_TRACK,     5, 200,   &g_slTrack,   &g_vTrack },
        { L"光标查询超时", F_TIMEOUT,  20, 2000,  &g_slTimeout, &g_vTimeout },
        { L"临时显示时长", F_AUTOHIDE,  0, 10000, &g_slAutoHide,&g_vAutoHide },
    };
    int vals[4] = { s_cfg.pollMs, s_cfg.trackMs, s_cfg.caretTimeoutMs, s_cfg.autoHideMs };
    for (int i = 0; i < 4; i++) {
        CLabel(sl[i].nm, x0 + 6, y + 3, 110, 20);
        *sl[i].vl = CEditNum(x0 + 346, y, 64, 22, -1, sl[i].field);
        CLabel(L"ms", x0 + 414, y + 3, 30, 20);
        *sl[i].sl = CSlider(x0 + 120, y, 220, 22, -1, sl[i].field,
                            sl[i].lo, sl[i].hi, vals[i], *sl[i].vl);
        HWND rb = CreateWindowExW(0, L"BUTTON", L"重置",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  x0 + 448, y, 44, 22, P,
                                  (HMENU)(INT_PTR)(ID_RESET + 12 + i), NULL, NULL);
        SendMessageW(rb, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
        MakePushButton(rb);
        RegMeta(rb, -1, 0, NULL, x0 + 448, y, 44, 22);
        y += 26;
    }

    y += 8;
    CLabel(L"中英判定策略", x0 + 6, y + 3, 110, 20);
    g_cbStrategy = CCombo(x0 + 120, y, 220, 180, -1, F_STRATEGY);
    SendMessageW(g_cbStrategy, CB_ADDSTRING, 0, (LPARAM)L"自动学习（推荐）");
    SendMessageW(g_cbStrategy, CB_ADDSTRING, 0, (LPARAM)L"只看 IME open 状态");
    SendMessageW(g_cbStrategy, CB_ADDSTRING, 0, (LPARAM)L"只看转换模式");
    SendMessageW(g_cbStrategy, CB_SETCURSEL, s_cfg.imeStrategy, 0);
    y += 34;

    HWND hHide = CLabel(L"隐藏", x0, y, 200, 22);
    SendMessageW(hHide, WM_SETFONT, (WPARAM)BoldFont(), TRUE);
    y += 24;
    g_ckFull = CCheck(L"前景窗口全屏时隐藏", x0 + 6, y, 300, 22, -1, F_FULLSCREEN, s_cfg.hideFullscreen);
    y += 24;
    g_ckComp = CCheck(L"输入法组合窗出现时隐藏", x0 + 6, y, 300, 22, -1, F_COMPOSITION, s_cfg.hideComposition);
    y += 24;
    g_ckWps = CCheck(L"启用 WPS 光标通道", x0 + 6, y, 300, 22, -1, F_WPSCOM, s_cfg.wpsCom);
    y += 34;

    CLabel(L"尺寸与偏移按 96 DPI 的逻辑像素计算，高 DPI 屏上会自动放大。",
           x0, y, 640, 40);
}

static void FillGlobalPanel(void) {
    SetSliderPos(g_slPoll, s_cfg.pollMs);
    SetSliderPos(g_slTrack, s_cfg.trackMs);
    SetSliderPos(g_slTimeout, s_cfg.caretTimeoutMs);
    SetSliderPos(g_slAutoHide, s_cfg.autoHideMs);
    SetNumText(g_vPoll, s_cfg.pollMs);
    SetNumText(g_vTrack, s_cfg.trackMs);
    SetNumText(g_vTimeout, s_cfg.caretTimeoutMs);
    SetNumText(g_vAutoHide, s_cfg.autoHideMs);
    SendMessageW(g_cbStrategy, CB_SETCURSEL, s_cfg.imeStrategy, 0);
    SendMessageW(g_ckFull, BM_SETCHECK, s_cfg.hideFullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_ckComp, BM_SETCHECK, s_cfg.hideComposition ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_ckWps, BM_SETCHECK, s_cfg.wpsCom ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* ---------- 黑名单面板 ---------- */
static HWND g_ignoreList = NULL, g_ignoreEdit = NULL;

static void BuildIgnorePanel(void) {
    P = g_ignorePanel;
    int y = 0, x0 = 14;
    CLabel(L"命中名单的程序彻底隐身：不做状态检测、不做光标查询、不显示提示。",
           x0, y, 740, 22);
    y += 22;
    CLabel(L"每行一个进程名，.exe 可写可不写。", x0, y, 740, 22);
    y += 26;

    g_ignoreList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        x0, y, 420, 258, P, NULL, NULL, NULL);
    SendMessageW(g_ignoreList, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(g_ignoreList, -2, 0, NULL, x0, y, 420, 258);
    y += 268;

    g_ignoreEdit = CEdit(x0, y, 300, 24);
    CButton(L"添加", x0 + 310, y, 70, 24, ID_ADD_IGNORE);
    CButton(L"删除选中", x0 + 386, y, 90, 24, ID_DEL_IGNORE);
}

static void FillIgnorePanel(void) {
    SendMessageW(g_ignoreList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < s_cfg.ignoreCount; i++)
        SendMessageW(g_ignoreList, LB_ADDSTRING, 0, (LPARAM)s_cfg.ignore[i]);
}

/* ---------- 预览窗口 ---------- */

/* 预览的状态切换：11 个状态都能看，不然预览永远只有当前页第一个状态。
   下拉沿用自绘的 SEL_CLASS，挂在预览条自己身上 —— 几何跟着预览走，缩放时
   不用主窗口再摆一次。 */
static void InvalidatePreview(void);
static void ShowPage(int page);
static int PreviewStates(ImeState* out, int cap) {
    int n = 0;
    for (int p = 0; p < 5; p++) {
        for (int k = 0; k < 3; k++) {
            ImeState s = kPages[p].states[k];
            if ((int)s < 0) continue;
            if (n >= cap) return n;
            out[n++] = s;
        }
    }
    return n;
}

static ImeState PreviewStateAt(int idx) {
    ImeState list[IMEST_COUNT];
    int n = PreviewStates(list, IMEST_COUNT);
    return (idx >= 0 && idx < n) ? list[idx] : IMEST_US;
}

static int PreviewIndexOf(ImeState s) {
    ImeState list[IMEST_COUNT];
    int n = PreviewStates(list, IMEST_COUNT);
    for (int i = 0; i < n; i++)
        if (list[i] == s) return i;
    return -1;
}

/* 标签与下拉的几何；画标签、摆控件共用一份，避免两边错位 */
static void PreviewSwitchRects(int cw, int ch, RECT* sw, RECT* lbl) {
    int w = S(PREVIEW_SW_W), h = S(PREVIEW_SW_H);
    int x = cw - S(PREVIEW_SW_MARGIN) - w;
    int y = (ch - h) / 2;
    sw->left = x;
    sw->top = y;
    sw->right = x + w;
    sw->bottom = y + h;
    lbl->right = x - S(8);
    lbl->left = lbl->right - S(PREVIEW_LBL_W);
    lbl->top = y;
    lbl->bottom = y + h;
}

static void SyncPreviewSwitch(void) {
    if (!g_swState) return;
    int idx = PreviewIndexOf((ImeState)g_selState);
    if (idx >= 0) SendMessageW(g_swState, CB_SETCURSEL, (WPARAM)idx, 0);
}

/* 滚轮翻状态：-1 上一个，+1 下一个。到头就停住，不绕回 —— 不然一直滚
   下去会不知道自己在列表的哪个位置。 */
static void StepPreviewState(int dir) {
    ImeState list[IMEST_COUNT];
    int n = PreviewStates(list, IMEST_COUNT);
    if (n <= 0) return;
    int idx = PreviewIndexOf((ImeState)g_selState);
    if (idx < 0) idx = 0;
    int next = idx + dir;
    if (next < 0) next = 0;
    if (next >= n) next = n - 1;
    if (next == idx) return;
    g_selState = (int)list[next];
    SyncPreviewSwitch();
    InvalidatePreview();
}

static void BuildPreviewSwitch(HWND parent) {
    g_swState = CreateWindowExW(0, SEL_CLASS, NULL,
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                0, 0, 10, 10, parent,
                                (HMENU)(INT_PTR)ID_PREVIEW_STATE, NULL, NULL);
    if (!g_swState) return;
    SelData* d = (SelData*)calloc(1, sizeof(SelData));
    if (d) {
        d->sel = 0;
        d->hot = -1;
        SetWindowLongPtrW(g_swState, GWLP_USERDATA, (LONG_PTR)d);
    }
    SendMessageW(g_swState, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    /* 项的顺序必须与 PreviewStates() 一致（都按状态枚举顺序） */
    for (int p = 0; p < 5; p++) {
        for (int k = 0; k < 3; k++) {
            ImeState s = kPages[p].states[k];
            if ((int)s < 0) continue;
            WCHAR t[SEL_MAX_TEXT];
            _snwprintf_s(t, SEL_MAX_TEXT, _TRUNCATE, L"%s · %s",
                         kPages[p].name, StateLabel(s));
            SendMessageW(g_swState, CB_ADDSTRING, 0, (LPARAM)t);
        }
    }
    SyncPreviewSwitch();
}

/* 预览条的整块内容。单独拆出来是为了能画进内存位图（双缓冲）：
   底色、示例文字、光标竖线、圆点分四次往同一块 DC 写，直接画到窗口上时
   拖动滑块（每次滚动都要重画整个预览条）会看到中间那几帧。 */
static void PaintPreview(HDC dc, const RECT* prc) {
    RECT rc = *prc;

    HBRUSH bg = CreateSolidBrush(RGB(0xFA, 0xFA, 0xFA));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    /* 右侧状态切换的几何：画标签和避让文字都用这一份 */
    RECT sw, lbl;
    PreviewSwitchRects(rc.right, rc.bottom, &sw, &lbl);

    int baseY = rc.bottom / 2 + S(2);
    HFONT old = (HFONT)SelectObject(dc, BaseFont());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x33, 0x33, 0x33));
    const WCHAR* sample = L"输入法状态";
    TextOutW(dc, S(16), baseY - S(18), sample, 5);
    SIZE ts;
    GetTextExtentPoint32W(dc, sample, 5, &ts);

    int caretX = S(16) + ts.cx;
    RECT caret = { caretX, baseY - S(18), caretX + S(2), baseY };
    HBRUSH cb = CreateSolidBrush(RGB(0x22, 0x22, 0x22));
    FillRect(dc, &caret, cb);
    DeleteObject(cb);
    SelectObject(dc, old);

    StateStyle st;
    CfgStateStyle(&s_cfg, (ImeState)g_selState, &st);
    if (st.enabled && st.shape != SHAPE_NONE) {
        int sz = S(st.size);
        if (sz < 4) sz = 4;
        if (sz > S(96)) sz = S(96);
        int x = caretX + S(st.offsetX);
        int y = baseY + S(st.offsetY);

        int shape = st.shape;
        ImgView iv = { 0, 0, NULL };
        int haveImg = 0;
        if (shape == SHAPE_IMAGE) {
            if (ImgGet(st.image, sz, &iv, NULL, 0)) haveImg = 1;
            else shape = SHAPE_CIRCLE;   /* 图片缺失/解码失败：回退圆点 */
        }
        int dw = haveImg ? iv.w : sz;
        int dh = haveImg ? iv.h : sz;

        /* 走和悬浮窗**同一份**光栅化再 AlphaBlend 贴上去：
           用 GDI 的 Ellipse/Polygon 是无抗锯齿的，预览就会比实际毛糙。 */
        BITMAPINFO bmi;
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = dw;
        bmi.bmiHeader.biHeight = -dh;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = NULL;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = mem ? CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
        if (mem && bmp && bits) {
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            if (haveImg) ImgBlit((BYTE*)bits, &iv, st.alpha);
            else ShapeRasterize((BYTE*)bits, sz, st.rgb, st.alpha, shape);
            BLENDFUNCTION bf;
            bf.BlendOp = AC_SRC_OVER;
            bf.BlendFlags = 0;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(dc, x, y, dw, dh, mem, 0, 0, dw, dh, bf);
            SelectObject(mem, oldBmp);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
    } else {
        /* 不显示的状态也要能看出来：否则会以为预览坏了 */
        RECT hr = { S(16), baseY + S(5), lbl.left - S(12), rc.bottom };
        if (hr.right > hr.left) {
            HFONT oldf = (HFONT)SelectObject(dc, BaseFont());
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0x99, 0x99, 0x99));
            DrawTextW(dc, L"该状态不显示提示", -1, &hr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, oldf);
        }
    }

    /* 右侧切换的说明文字 */
    {
        HFONT oldf = (HFONT)SelectObject(dc, BaseFont());
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x66, 0x66, 0x66));
        DrawTextW(dc, L"预览状态", -1, &lbl,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldf);
    }
}

static LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        BuildPreviewSwitch(h);
        return 0;
    case WM_SIZE: {
        RECT pr;
        GetClientRect(h, &pr);
        RECT sw, lbl;
        PreviewSwitchRects(pr.right, pr.bottom, &sw, &lbl);
        if (g_swState)
            SetWindowPos(g_swState, HWND_TOP, sw.left, sw.top,
                         sw.right - sw.left, sw.bottom - sw.top, SWP_NOACTIVATE);
        return 0;
    }
    case WM_COMMAND:            /* 下拉选中：转给主窗口 @see WM_COMMAND */
        return SendMessageW(g_hwnd, m, w, l);
    case WM_ERASEBKGND:
        return 1;               /* 底色算在 PaintPreview 里，别擦第二遍 */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        BufDC buf;
        if (BufBegin(&buf, dc, rc.right, rc.bottom)) {
            PaintPreview(buf.mem, &rc);
            BufEnd(&buf, dc);
        } else {
            PaintPreview(dc, &rc);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void InvalidatePreview(void) {
    if (g_preview) InvalidateRect(g_preview, NULL, FALSE);
}

/* ---------- 提交 ---------- */
static int Checked(HWND c) { return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED; }

/* 把当前页的控件值写回 s_cfg（用 g_blkState 记住每块当前对应哪个状态） */
static void CommitForm(void) {
    if (g_page < 5) {
        for (int i = 0; i < 3; i++) {
            ImeState s = g_blkState[i];
            if ((int)s < 0) continue;
            Block* b = &g_blk[i];
            StateStyle* raw = &s_cfg.st[s];
            raw->enabled = Checked(b->enable);
            raw->have |= SF_ENABLED;
            /* 跟随中：除「启用 / 跟随」外的控件显示的是基础状态的值，不能写回
               —— 写回去会把这几个字段变成显式值，之后再改基础状态就不跟了。 */
            int inherited = 0;
            if (IsCapsState(s)) {
                inherited = Checked(b->inherit);
                raw->inherit = inherited ? CapsBase(s) : -1;
            }
            if (inherited) continue;
            int sel = (int)SendMessageW(b->shape, CB_GETCURSEL, 0, 0);
            int shape = (sel == 0) ? SHAPE_NONE :
                        (sel == 2) ? SHAPE_TRIANGLE :
                        (sel == 3) ? SHAPE_IMAGE : SHAPE_CIRCLE;
            raw->shape = shape;
            raw->have |= SF_SHAPE;
            if (shape != SHAPE_IMAGE) g_lastShape[s] = shape;
            /* 跟随 cn_cn 的项不能写回：写回去就变成显式值，之后改 cn_cn 不跟了 */
            if (!CfgFieldFollowsCn(&s_cfg, s, SF_SIZE)) {
                raw->size = SliderPos(b->slSize);       raw->have |= SF_SIZE;
            }
            if (!CfgFieldFollowsCn(&s_cfg, s, SF_OFFSETX)) {
                raw->offsetX = SliderPos(b->slOffX);    raw->have |= SF_OFFSETX;
            }
            if (!CfgFieldFollowsCn(&s_cfg, s, SF_OFFSETY)) {
                raw->offsetY = SliderPos(b->slOffY);    raw->have |= SF_OFFSETY;
            }
            if (!CfgFieldFollowsCn(&s_cfg, s, SF_ALPHA)) {
                raw->alpha = SliderPos(b->slAlpha);     raw->have |= SF_ALPHA;
            }
            raw->transient = Checked(b->transient); raw->have |= SF_TRANSIENT;
        }
    } else if (g_page == PAGE_GLOBAL) {
        s_cfg.pollMs = SliderPos(g_slPoll);
        s_cfg.trackMs = SliderPos(g_slTrack);
        s_cfg.caretTimeoutMs = SliderPos(g_slTimeout);
        s_cfg.autoHideMs = SliderPos(g_slAutoHide);
        s_cfg.imeStrategy = (int)SendMessageW(g_cbStrategy, CB_GETCURSEL, 0, 0);
        s_cfg.hideFullscreen = Checked(g_ckFull);
        s_cfg.hideComposition = Checked(g_ckComp);
        s_cfg.wpsCom = Checked(g_ckWps);
    }
    /* 黑名单的增删是即时写进 s_cfg 的，不用提交 */
}

/* ---------- 脏标记 ----------
   只比"值"，不比 have 位：勾选再取消、把数字改回原值都不算修改。
   保存按钮与标题星号都跟着这个判断走。 */
static ImeCfg g_base;

static int StyleSame(const StateStyle* a, const StateStyle* b) {
    return a->enabled == b->enabled && a->rgb == b->rgb && a->shape == b->shape &&
           wcscmp(a->image, b->image) == 0 && a->size == b->size &&
           a->offsetX == b->offsetX && a->offsetY == b->offsetY &&
           a->alpha == b->alpha && a->transient == b->transient &&
           a->inherit == b->inherit;
}

static int CfgSameValues(const ImeCfg* a, const ImeCfg* b) {
    if (a->pollMs != b->pollMs || a->trackMs != b->trackMs ||
        a->imeStrategy != b->imeStrategy ||
        a->caretTimeoutMs != b->caretTimeoutMs ||
        a->hideFullscreen != b->hideFullscreen ||
        a->hideComposition != b->hideComposition ||
        a->wpsCom != b->wpsCom || a->autoHideMs != b->autoHideMs)
        return 0;
    if (!StyleSame(&a->def, &b->def)) return 0;
    for (int i = 0; i < IMEST_COUNT; i++)
        if (!StyleSame(&a->st[i], &b->st[i])) return 0;
    if (a->ignoreCount != b->ignoreCount) return 0;
    for (int i = 0; i < a->ignoreCount; i++)
        if (_wcsicmp(a->ignore[i], b->ignore[i]) != 0) return 0;
    return 1;
}

static void RefreshDirty(void) {
    if (!g_hwnd) return;
    CommitForm();
    g_dirty = !CfgSameValues(&s_cfg, &g_base);

    /* 只在真的变了才动标题与按钮：每次滚动都 SetWindowText/EnableWindow 一遍，
       即便值没变也会让非客户区与按钮重画，拖动滑块时看起来就是一直在闪。 */
    WCHAR t[96];
    _snwprintf_s(t, 96, _TRUNCATE, L"IMEIndicator 设置%s", g_dirty ? L" *" : L"");
    WCHAR cur[96];
    if (GetWindowTextW(g_hwnd, cur, 96) <= 0 || wcscmp(cur, t) != 0)
        SetWindowTextW(g_hwnd, t);

    static int lastDirty = -1;
    if (lastDirty != g_dirty) {
        lastDirty = g_dirty;
        HWND b1 = GetDlgItem(g_hwnd, ID_SAVE);
        HWND b2 = GetDlgItem(g_hwnd, ID_SAVE_RESTART);
        if (b1) EnableWindow(b1, g_dirty);
        if (b2) EnableWindow(b2, g_dirty);
    }
}

/* 窗口宽度属于界面状态，不是配置项：单独补写一次，不把界面上没保存的改动
   带出去 —— 重新读盘、只改这一个字段、写回。 */
static void SaveSettingsWidth(int w) {
    ImeCfg tmp;
    CfgLoad(&tmp);
    if (tmp.settingsWidth == w) return;
    tmp.settingsWidth = w;
    CfgSave(&tmp);
}

/* ---------- 保存 / 重启 ---------- */
static void DoSave(int restart) {
    CommitForm();
    /* 形状=图片但没选到可用图片：回退到上一次选择的形状再存 */
    if (FallbackBadImages()) {
        if (g_page < 5) ShowPage(g_page);
        InvalidatePreview();
    }
    if (!CfgSave(&s_cfg)) {
        MessageBoxW(g_hwnd, L"写入 IMEIndicator.json 失败。\n\n"
                            L"请确认程序所在目录可写（放在 Program Files 下时需要管理员权限）。",
                    L"保存失败", MB_ICONERROR | MB_OK);
        return;
    }
    g_base = s_cfg;
    RefreshDirty();

    if (!restart) {
        MessageBoxW(g_hwnd, L"已保存。\n\n重启 IMEIndicator 后生效。",
                    L"IMEIndicator 设置", MB_ICONINFORMATION | MB_OK);
        return;
    }

    /* 调主程序 --restart。退出码 2 = 有实例在跑但通知失败（典型：权限不一致）。 */
    WCHAR dir[MAX_PATH], exe[MAX_PATH];
    CfgDir(dir, MAX_PATH);
    _snwprintf_s(exe, MAX_PATH, _TRUNCATE, L"%s\\IMEIndicator.exe", dir);

    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwnd, L"已保存，但找不到 IMEIndicator.exe。\n\n请手动启动主程序。",
                    L"IMEIndicator 设置", MB_ICONWARNING | MB_OK);
        return;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    WCHAR cmd[MAX_PATH + 32];
    _snwprintf_s(cmd, MAX_PATH + 32, _TRUNCATE, L"\"%s\" --restart", exe);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, dir, &si, &pi)) {
        MessageBoxW(g_hwnd, L"已保存，但无法启动 IMEIndicator.exe 来重启。\n\n"
                            L"请从托盘菜单点「重启程序」。",
                    L"IMEIndicator 设置", MB_ICONWARNING | MB_OK);
        return;
    }
    WaitForSingleObject(pi.hProcess, 3000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code == 2) {
        MessageBoxW(g_hwnd, L"已保存，但无法通知正在运行的 IMEIndicator 重启。\n\n"
                            L"这通常是因为主程序以管理员身份运行、而设置程序没有。\n"
                            L"请从托盘菜单点「重启程序」。",
                    L"IMEIndicator 设置", MB_ICONWARNING | MB_OK);
    }
}

/* ---------- 页切换 ---------- */
static void ShowPage(int page) {
    ShowWindow(g_layoutPanel, page < 5 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_globalPanel, page == PAGE_GLOBAL ? SW_SHOW : SW_HIDE);
    ShowWindow(g_ignorePanel, page == PAGE_IGNORE ? SW_SHOW : SW_HIDE);
    if (page < 5) {
        for (int i = 0; i < 3; i++)
            FillBlock(i, kPages[page].states[i]);
        /* 最后一块的分隔线会变成页面底部的悬空线，藏掉 */
        int last = -1;
        for (int i = 0; i < 3; i++)
            if ((int)kPages[page].states[i] >= 0) last = i;
        for (int i = 0; i < 3; i++)
            if (g_blk[i].sep)
                ShowWindow(g_blk[i].sep, i < last ? SW_SHOW : SW_HIDE);
    } else if (page == PAGE_GLOBAL) {
        FillGlobalPanel();
    } else {
        FillIgnorePanel();
    }
    InvalidatePreview();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void SwitchPage(int page) {
    if (page < 0 || page >= PAGE_COUNT || page == g_page) return;
    CommitForm();                     /* 先把当前页的值收回去 */
    g_page = page;
    if (page < 5) {
        /* 切页时预览跟着落到本页第一个状态；之后可以再用下拉挑别的 */
        g_selState = (int)kPages[page].states[0];
        SyncPreviewSwitch();
    }
    ShowPage(page);
}

/* ---------- 页签绘制 ---------- */
static int TabWidth(HDC dc, int i) {
    HFONT old = (HFONT)SelectObject(dc, BaseFont());
    SIZE sz;
    GetTextExtentPoint32W(dc, kPages[i].name, (int)wcslen(kPages[i].name), &sz);
    SelectObject(dc, old);
    return sz.cx + S(26);
}

static int g_hotTab = -1;   /* 鼠标悬停的页签；-1 = 没有 */

static void PaintTabs(HDC dc) {
    RECT crc;
    GetClientRect(g_hwnd, &crc);
    int x = S(TAB_X);
    int top = S(TAB_Y), bot = S(TAB_Y + TAB_H);
    HFONT old = (HFONT)SelectObject(dc, BaseFont());
    SetBkMode(dc, TRANSPARENT);

    /* 分隔线：所有页签都"站"在它上面（现代设置页的下划线页签） */
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xD6, 0xD6, 0xD6));
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, 0, bot - 1, NULL);
    LineTo(dc, crc.right, bot - 1);
    SelectObject(dc, op);
    DeleteObject(pen);

    for (int i = 0; i < PAGE_COUNT; i++) {
        int w = TabWidth(dc, i);
        RECT r = { x, top, x + w, bot };
        if (i == g_page) {
            /* 选中页：强调条从页签脚下长出来，贴住分隔线 */
            RECT ul = { x + S(8), bot - S(3), x + w - S(8), bot - 1 };
            HBRUSH b = CreateSolidBrush(RGB(0x00, 0x78, 0xD7));
            FillRect(dc, &ul, b);
            DeleteObject(b);
        } else if (i == g_hotTab) {
            RECT hr = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
            COLORREF hc = RGB(0xE5, 0xF1, 0xFB);
            PaintRoundRectAA(dc, &hr, S(4), hc, hc);
        }
        /* 选中页不加粗：粗体只留给段头，切换时文字宽度不跳 */
        SetTextColor(dc, i == g_page ? RGB(0x00, 0x5A, 0x9E)
                        : i == g_hotTab ? RGB(0x00, 0x00, 0x00)
                                        : RGB(0x54, 0x54, 0x54));
        DrawTextW(dc, kPages[i].name, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += w;
    }
    SelectObject(dc, old);
}

static int HitTab(int mx, int my) {
    if (my < S(TAB_Y) || my > S(TAB_Y + TAB_H)) return -1;
    HDC dc = GetDC(g_hwnd);
    int x = S(TAB_X);
    int hit = -1;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int w = TabWidth(dc, i);
        if (mx >= x && mx < x + w) { hit = i; break; }
        x += w;
    }
    ReleaseDC(g_hwnd, dc);
    return hit;
}

/* ---------- 选图片 ---------- */
/* 选一个文件 -> 复制进 shapes\（重名自动加序号）-> 该状态切到图片形状。
   复制而不是引用原路径：配置里只存文件名，换机器/挪目录都不会断。 */
static void PickImage(int block) {
    ImeState s = g_blkState[block];
    if ((int)s < 0) return;

    WCHAR file[MAX_PATH];
    file[0] = 0;
    WCHAR title[128];
    _snwprintf_s(title, 128, _TRUNCATE,
                 L"选择图片（文件 ≤ %u MB，长边 ≤ %u 像素）",
                 IMG_MAX_FILE_BYTES / (1024u * 1024u), IMG_MAX_EDGE);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"图片 (*.png;*.ico;*.bmp;*.jpg)\0*.png;*.ico;*.bmp;*.jpg\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    WCHAR name[CFG_NAME_MAX];
    WCHAR err[256];
    if (!ImgImport(file, name, CFG_NAME_MAX, err, 256)) {
        MessageBoxW(g_hwnd, err[0] ? err : L"导入图片失败。",
                    L"导入图片失败", MB_ICONERROR | MB_OK);
        return;
    }

    StateStyle* raw = &s_cfg.st[s];
    lstrcpynW(raw->image, name, CFG_NAME_MAX);
    raw->shape = SHAPE_IMAGE;
    raw->have |= SF_IMAGE | SF_SHAPE;
    g_dirty = 1;
    FillBlock(block, s);
    InvalidatePreview();
}

/* 恢复出厂默认：只改内存里的配置，界面立刻反映；点「保存」才写盘，
   直接关窗口等于没重置。窗口宽度是界面状态，不参与重置。 */
static void ResetAll(void) {
    if (MessageBoxW(g_hwnd,
                    L"把所有设置恢复为默认值？\n\n"
                    L"恢复后要点「保存」才会写入配置文件；直接关闭窗口不改动。",
                    L"恢复默认值",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    int keepW = MulDiv(cr.right, 96, g_dpi > 0 ? g_dpi : 96);
    CfgDefaults(&s_cfg);
    s_cfg.settingsWidth = keepW;
    SyncLastShapes();
    ShowPage(g_page);        /* 重填当前页（含预览） */
    RefreshDirty();
}

/* ---------- 命令处理 ---------- */
static void OnCommand(int id, HWND ctl) {
    if (id == ID_RESET_ALL) { ResetAll(); return; }
    if (id >= ID_RESET && id < ID_RESET + 16) {
        int k = id - ID_RESET;
        int block = (k < 12) ? k / 4 : -1;
        int field = (k < 12) ? kStateFields[k % 4] : kGlobalFields[k - 12];
        SetSliderValue(block, field, DefaultFor(block, field));
        return;
    }
    if (id >= ID_PICK_IMAGE && id < ID_PICK_IMAGE + 3) {
        PickImage(id - ID_PICK_IMAGE);
        return;
    }
    if (id == ID_SAVE)         { DoSave(0); return; }
    if (id == ID_SAVE_RESTART) { DoSave(1); return; }
    if (id == ID_CLOSE)        { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }

    if (id == ID_ADD_IGNORE && g_ignoreEdit) {
        WCHAR buf[CFG_NAME_MAX];
        GetWindowTextW(g_ignoreEdit, buf, CFG_NAME_MAX);
        WCHAR* q = buf;
        while (*q == L' ') q++;
        size_t len = wcslen(q);
        while (len > 0 && q[len-1] == L' ') q[--len] = 0;
        if (!q[0] || s_cfg.ignoreCount >= CFG_MAX_IGNORE) return;
        lstrcpynW(s_cfg.ignore[s_cfg.ignoreCount], q, CFG_NAME_MAX);
        s_cfg.ignoreCount++;
        SendMessageW(g_ignoreList, LB_ADDSTRING, 0, (LPARAM)q);
        SetWindowTextW(g_ignoreEdit, L"");
        g_dirty = 1;
        return;
    }
    if (id == ID_DEL_IGNORE && g_ignoreList) {
        int sel = (int)SendMessageW(g_ignoreList, LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= s_cfg.ignoreCount) return;
        for (int i = sel; i + 1 < s_cfg.ignoreCount; i++)
            lstrcpynW(s_cfg.ignore[i], s_cfg.ignore[i+1], CFG_NAME_MAX);
        s_cfg.ignoreCount--;
        SendMessageW(g_ignoreList, LB_DELETESTRING, (WPARAM)sel, 0);
        g_dirty = 1;
        return;
    }

    Meta* m = FindMeta(ctl);
    if (!m || m->block < 0) return;

    /* 尺寸/偏移/透明度 的「自定义」：不勾 = 跟随中文输入法(cn_cn)。
       勾上时把当前显示的值（cn_cn 的）落成本状态自己的显式值，观感不变；
       取消勾选则清掉显式位，求值时自然回落到 cn_cn。 */
    unsigned cbit = CustomBit(m->field);
    if (cbit) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        StateStyle* raw = &s_cfg.st[s];
        if (Checked(ctl)) {
            StateStyle eff;
            CfgStateStyle(&s_cfg, s, &eff);
            switch (cbit) {
            case SF_SIZE:    raw->size = eff.size;       break;
            case SF_OFFSETX: raw->offsetX = eff.offsetX; break;
            case SF_OFFSETY: raw->offsetY = eff.offsetY; break;
            case SF_ALPHA:   raw->alpha = eff.alpha;     break;
            }
            raw->have |= cbit;
        } else {
            raw->have &= ~cbit;
        }
        g_dirty = 1;
        FillBlock(m->block, s);
        InvalidatePreview();
        return;
    }

    /* 颜色按钮：弹系统取色器 */
    if (m->field == F_COLOR) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        StateStyle* raw = &s_cfg.st[s];
        CHOOSECOLORW cc;
        ZeroMemory(&cc, sizeof(cc));
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = g_hwnd;
        cc.rgbResult = RGB((raw->rgb >> 16) & 0xFF, (raw->rgb >> 8) & 0xFF, raw->rgb & 0xFF);
        static COLORREF custom[16];
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            raw->rgb = ((DWORD)GetRValue(cc.rgbResult) << 16) |
                       ((DWORD)GetGValue(cc.rgbResult) << 8) |
                       (DWORD)GetBValue(cc.rgbResult);
            raw->have |= SF_COLOR;
            g_dirty = 1;
            InvalidateRect(ctl, NULL, TRUE);
            InvalidatePreview();
        }
        return;
    }

    /* 跟随开关：立刻把整块置灰/解灰；取消跟随时把基础状态的值拷进来 */
    if (m->field == F_INHERIT) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        StateStyle* raw = &s_cfg.st[s];
        int on = Checked(ctl);
        if (on) {
            raw->inherit = CapsBase(s);
        } else {
            StateStyle eff;
            CfgStateStyle(&s_cfg, s, &eff);
            raw->inherit = -1;
            raw->enabled = eff.enabled;     raw->have |= SF_ENABLED;
            raw->rgb = eff.rgb;             raw->have |= SF_COLOR;
            raw->shape = eff.shape;         raw->have |= SF_SHAPE;
            raw->size = eff.size;           raw->have |= SF_SIZE;
            raw->offsetX = eff.offsetX;     raw->have |= SF_OFFSETX;
            raw->offsetY = eff.offsetY;     raw->have |= SF_OFFSETY;
            raw->alpha = eff.alpha;         raw->have |= SF_ALPHA;
            raw->transient = eff.transient; raw->have |= SF_TRANSIENT;
        }
        g_dirty = 1;
        FillBlock(m->block, s);
        InvalidatePreview();
        return;
    }

    if (m->field == F_SHAPE) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        int shape = (sel == 0) ? SHAPE_NONE :
                    (sel == 2) ? SHAPE_TRIANGLE :
                    (sel == 3) ? SHAPE_IMAGE : SHAPE_CIRCLE;
        s_cfg.st[s].shape = shape;
        s_cfg.st[s].have |= SF_SHAPE;
        if (shape != SHAPE_IMAGE) g_lastShape[s] = shape;
        g_dirty = 1;
        /* 切到/切离「图片」要同步文件名标签的显隐与标红 */
        FillBlock(m->block, s);
        InvalidatePreview();
        return;
    }
    if (m->field == F_ENABLED) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        s_cfg.st[s].enabled = Checked(ctl);
        s_cfg.st[s].have |= SF_ENABLED;
        g_dirty = 1;
        InvalidatePreview();
        return;
    }
    if (m->field == F_TRANSIENT) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        s_cfg.st[s].transient = Checked(ctl);
        s_cfg.st[s].have |= SF_TRANSIENT;
        g_dirty = 1;
        return;
    }

    /* 全局页的复选/下拉 */
    if (m->block == -1) {
        if (m->field == F_FULLSCREEN)  s_cfg.hideFullscreen = Checked(ctl);
        if (m->field == F_COMPOSITION) s_cfg.hideComposition = Checked(ctl);
        if (m->field == F_WPSCOM)      s_cfg.wpsCom = Checked(ctl);
        if (m->field == F_STRATEGY)    s_cfg.imeStrategy = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        g_dirty = 1;
    }
}

/* 滑块与数值框共用的"落到配置"逻辑 */
static HWND SliderFor(int block, int field) {
    if (block >= 0 && block < 3) {
        Block* b = &g_blk[block];
        switch (field) {
        case F_SIZE:  return b->slSize;
        case F_OFFX:  return b->slOffX;
        case F_OFFY:  return b->slOffY;
        case F_ALPHA: return b->slAlpha;
        }
    } else if (block == -1) {
        switch (field) {
        case F_POLL:     return g_slPoll;
        case F_TRACK:    return g_slTrack;
        case F_TIMEOUT:  return g_slTimeout;
        case F_AUTOHIDE: return g_slAutoHide;
        }
    }
    return NULL;
}

static void ApplySliderValue(int block, int field, int v) {
    if (block >= 0) {
        ImeState s = g_blkState[block];
        if ((int)s < 0) return;
        StateStyle* raw = &s_cfg.st[s];
        switch (field) {
        case F_SIZE:  raw->size = v;    raw->have |= SF_SIZE;    break;
        case F_OFFX:  raw->offsetX = v; raw->have |= SF_OFFSETX; break;
        case F_OFFY:  raw->offsetY = v; raw->have |= SF_OFFSETY; break;
        case F_ALPHA: raw->alpha = v;   raw->have |= SF_ALPHA;   break;
        default: return;
        }
        InvalidatePreview();
    } else if (block == -1) {
        switch (field) {
        case F_POLL:     s_cfg.pollMs = v; break;
        case F_TRACK:    s_cfg.trackMs = v; break;
        case F_TIMEOUT:  s_cfg.caretTimeoutMs = v; break;
        case F_AUTOHIDE:
            s_cfg.autoHideMs = v;
            if (g_page < 5) { for (int i = 0; i < 3; i++) FillBlock(i, g_blkState[i]); }
            break;
        default: return;
        }
    }
    g_dirty = 1;

    /* 拖动时整条重画：只重画滑块矩形的话，旧滑块位置会留下残影 */
    HWND sl = SliderFor(block, field);
    if (sl) InvalidateRect(sl, NULL, FALSE);
}

/* 重置到默认值：状态字段用 defaults 段，全局字段用出厂值 */
static int DefaultFor(int block, int field) {
    if (block >= 0) {
        switch (field) {
        case F_SIZE:  return s_cfg.def.size;
        case F_OFFX:  return s_cfg.def.offsetX;
        case F_OFFY:  return s_cfg.def.offsetY;
        case F_ALPHA: return s_cfg.def.alpha;
        }
    } else {
        switch (field) {
        case F_POLL:     return 100;
        case F_TRACK:    return 15;
        case F_TIMEOUT:  return 150;
        case F_AUTOHIDE: return 1000;
        }
    }
    return 0;
}

static void SetSliderValue(int block, int field, int v) {
    HWND sl = SliderFor(block, field);
    if (!sl) return;
    SetSliderPos(sl, v);
    Meta* m = FindMeta(sl);
    if (m && m->label) SetNumText(m->label, v);
    ApplySliderValue(block, field, v);
}

static void OnHScroll(HWND slider) {
    Meta* m = FindMeta(slider);
    if (!m) return;
    int v = SliderPos(slider);
    SetNumText(m->label, v);
    ApplySliderValue(m->block, m->field, v);
}

/* 数值框输入：夹到滑块值域后同步回去 */
static void OnNumEditChanged(HWND e) {
    if (g_syncEdit) return;
    Meta* m = FindMeta(e);
    if (!m) return;
    HWND sl = SliderFor(m->block, m->field);
    if (!sl) return;
    WCHAR buf[32];
    GetWindowTextW(e, buf, 32);
    if (!buf[0]) return;                  /* 清空/输入中：先不动 */
    int v = _wtoi(buf);
    int lo = (int)SendMessageW(sl, TBM_GETRANGEMIN, 0, 0);
    int hi = (int)SendMessageW(sl, TBM_GETRANGEMAX, 0, 0);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    SetSliderPos(sl, v);
    ApplySliderValue(m->block, m->field, v);
}

/* 失焦：用滑块当前值把文本纠正回来（越界/空值不留在框里） */
static void OnNumEditDone(HWND e) {
    Meta* m = FindMeta(e);
    if (!m) return;
    HWND sl = SliderFor(m->block, m->field);
    if (!sl) return;
    SetNumText(e, (int)SendMessageW(sl, TBM_GETPOS, 0, 0));
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        g_dpi = (int)g_startupDpi;
        if (g_dpi <= 0) g_dpi = 96;
        g_whiteBrush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));

        /* 预览：WS_CLIPCHILDREN 是必须的 —— 状态切换下拉是它的子窗口，
           铺底色时不能盖掉 */
        g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"IMEIndicatorPreview", NULL,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, 100,
            h, NULL, NULL, NULL);

        /* 底部按钮（常驻，不随页销毁） */
        {
            P = h;
            HWND b;
            b = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_SAVE, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            MakePushButton(b);
            b = CreateWindowExW(0, L"BUTTON", L"保存并重启", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_SAVE_RESTART, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            MakePushButton(b);
            b = CreateWindowExW(0, L"BUTTON", L"退出", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_CLOSE, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            MakePushButton(b);
            /* 恢复默认值：靠左放，跟右下角的提交类按钮分开，免得误点 */
            b = CreateWindowExW(0, L"BUTTON", L"恢复默认值", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_RESET_ALL, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            MakePushButton(b);
        }

        /* 三个页面板：全部建好，切页只改显隐 */
        g_layoutPanel = MakePanel();
        P = g_layoutPanel;
        for (int i = 0; i < 3; i++) BuildStateBlock(i, i * 126 + 4);

        g_globalPanel = MakePanel();
        BuildGlobalPanel();

        g_ignorePanel = MakePanel();
        BuildIgnorePanel();

        ScaleControls();
        g_page = 0;
        g_selState = (int)kPages[0].states[0];
        SyncPreviewSwitch();
        ShowPage(0);
        RefreshDirty();          /* 初始无改动：保存按钮置灰，标题不带星号 */
        return 0;

    case WM_SIZE: {
        RECT rc;
        GetClientRect(h, &rc);
        MoveWindow(g_preview, S(PREVIEW_X), S(PREVIEW_Y),
                   rc.right - S(PREVIEW_X * 2), S(PREVIEW_H), TRUE);
        int pw = rc.right - S(28);
        int ph = rc.bottom - S(CONTENT_Y) - S(BOTTOM_H) + S(4);
        MoveWindow(g_layoutPanel, S(14), S(CONTENT_Y), pw, ph, TRUE);
        MoveWindow(g_globalPanel, S(14), S(CONTENT_Y), pw, ph, TRUE);
        MoveWindow(g_ignorePanel, S(14), S(CONTENT_Y), pw, ph, TRUE);

        int bh = S(28);
        int by = rc.bottom - S(BOTTOM_H) + (S(BOTTOM_H) - bh) / 2;
        HWND b0 = GetDlgItem(h, ID_RESET_ALL);
        HWND b1 = GetDlgItem(h, ID_SAVE);
        HWND b2 = GetDlgItem(h, ID_SAVE_RESTART);
        HWND b3 = GetDlgItem(h, ID_CLOSE);
        /* 左侧一个「恢复默认值」，右侧 Submit 那组靠右：保存 / 保存并重启 / 退出 */
        if (b0) SetWindowPos(b0, HWND_TOP, S(14), by, S(96), bh, SWP_NOACTIVATE);
        int bx3 = rc.right - S(14) - S(84);
        int bx2 = bx3 - S(8) - S(116);
        int bx1 = bx2 - S(8) - S(84);
        if (b1) SetWindowPos(b1, HWND_TOP, bx1, by, S(84), bh, SWP_NOACTIVATE);
        if (b2) SetWindowPos(b2, HWND_TOP, bx2, by, S(116), bh, SWP_NOACTIVATE);
        if (b3) SetWindowPos(b3, HWND_TOP, bx3, by, S(84), bh, SWP_NOACTIVATE);

        /* 记住窗口宽度（存逻辑像素，换 DPI 显示器也成立） */
        int lw = MulDiv(rc.right, 96, g_dpi > 0 ? g_dpi : 96);
        if (lw != s_cfg.settingsWidth) {
            s_cfg.settingsWidth = lw;
            if (lw != g_loadedWidth) g_widthDirty = 1;
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        /* 内容按固定逻辑坐标摆，窗口窄于设计宽度时右列会被裁掉 —— 直接不让缩 */
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        RECT rc = { 0, 0, S(WIN_W), S(WIN_H) };
        ClientToWindowSize(&rc, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), 0,
                           (UINT)(g_dpi > 0 ? g_dpi : 96));
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }

    case WM_COMMAND: {
        int code = HIWORD(w);
        HWND ctl = (HWND)l;
        /* 预览的状态切换：只看不改配置，不能影响脏标记 */
        if (code == CBN_SELCHANGE && ctl && ctl == g_swState) {
            ImeState s = PreviewStateAt((int)SendMessageW(ctl, CB_GETCURSEL, 0, 0));
            if ((int)s >= 0 && (int)s != g_selState) {
                g_selState = (int)s;
                InvalidatePreview();
            }
            return 0;
        }
        if (code == EN_CHANGE)    { OnNumEditChanged(ctl); RefreshDirty(); return 0; }
        if (code == EN_KILLFOCUS) { OnNumEditDone(ctl); RefreshDirty(); return 0; }
        if (code == BN_CLICKED && GetPropW(ctl, CHK_PROP)) {
            /* 自绘复选框不会自己翻转状态，这里补上 */
            int st = (int)SendMessageW(ctl, BM_GETCHECK, 0, 0);
            SendMessageW(ctl, BM_SETCHECK,
                         st == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            InvalidateRect(ctl, NULL, FALSE);
        }
        OnCommand(LOWORD(w), ctl);
        RefreshDirty();
        return 0;
    }

    case WM_HSCROLL:
        OnHScroll((HWND)l);
        RefreshDirty();
        return 0;

    case WM_MOUSEMOVE: {
        int t = HitTab((short)LOWORD(l), (short)HIWORD(l));
        if (t != g_hotTab) {
            g_hotTab = t;
            InvalidateRect(h, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme;
        ZeroMemory(&tme, sizeof(tme));
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hotTab >= 0) {
            g_hotTab = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(l);
        int my = (short)HIWORD(l);
        int t = HitTab(mx, my);
        if (t >= 0) SwitchPage(t);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        /* 光标停在预览状态下拉上时，滚轮直接翻状态 —— 不用点开列表就能
           一路滚着比较各状态的样子。滚轮消息发给焦点窗口，所以按屏幕坐标
           反查光标下面是谁。 */
        POINT pt;
        pt.x = (short)LOWORD(l);
        pt.y = (short)HIWORD(l);
        HWND under = WindowFromPoint(pt);
        if (g_swState && under && (under == g_swState || IsChild(g_swState, under))) {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            if (delta > 0)      StepPreviewState(-1);
            else if (delta < 0) StepPreviewState(1);
            return 0;
        }
        break;                  /* 其它地方照常滚动 */
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlType != ODT_BUTTON) return FALSE;

        if (GetPropW(di->hwndItem, CHK_PROP)) {
            PaintCheck(di);
            return TRUE;
        }

        Meta* mm = FindMeta(di->hwndItem);
        if (!mm || mm->field != F_COLOR) {
            PaintPushButton(di);
            return TRUE;
        }

        /* 颜色按钮：圆角色块 + 细边框 */
        DWORD rgb = 0xFF0000;
        if (mm->block >= 0) {
            ImeState s = g_blkState[mm->block];
            if ((int)s >= 0) {
                StateStyle eff;
                CfgStateStyle(&s_cfg, s, &eff);
                rgb = eff.rgb;
            }
        }
        HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
        FillRect(di->hDC, &di->rcItem, bg);
        DeleteObject(bg);

        COLORREF fill = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        COLORREF border = RGB(0xAD, 0xAD, 0xAD);
        if (di->itemState & ODS_DISABLED) {
            /* 置灰：往白色方向混 75%，仍能看出原来的颜色 */
            fill = RGB((GetRValue(fill) + 255 * 3) / 4,
                       (GetGValue(fill) + 255 * 3) / 4,
                       (GetBValue(fill) + 255 * 3) / 4);
            border = RGB(0xD0, 0xD0, 0xD0);
        }
        PaintRoundRectAA(di->hDC, &di->rcItem, S(3), border, fill);
        return TRUE;
    }

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3));
        FillRect((HDC)w, &rc, bg);
        DeleteObject(bg);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        /* 页签区灰底 */
        HBRUSH bg = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3));
        RECT head = { 0, 0, rc.right, S(CONTENT_Y - 6) };
        FillRect(dc, &head, bg);
        RECT foot = { 0, rc.bottom - S(BOTTOM_H) + S(6), rc.right, rc.bottom };
        FillRect(dc, &foot, bg);
        DeleteObject(bg);
        PaintTabs(dc);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_CLOSE:
        RefreshDirty();          /* 以真实值判断：改回原值就不该拦 */
        if (g_dirty) {
            int r = MessageBoxW(h, L"有未保存的修改，要保存吗？",
                                L"IMEIndicator 设置", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) DoSave(0);
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        HintHide();
        if (g_widthDirty && s_cfg.settingsWidth > 0) SaveSettingsWidth(s_cfg.settingsWidth);
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = NULL; }
        if (g_whiteBrush) { DeleteObject(g_whiteBrush); g_whiteBrush = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- 入口 ---------- */
static void SetDpiAware(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
    PFN_SetProcessDpiAwarenessContext p1 =
        u ? (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (p1) p1((HANDLE)(INT_PTR)-4);
}

static UINT QueryDpi(HWND h) {
    typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) pfn = (PFN_GetDpiForWindow)GetProcAddress(u, "GetDpiForWindow");
    }
    if (pfn && h) {
        UINT d = pfn(h);
        if (d) return d;
    }
    HDC dc = h ? GetDC(h) : GetDC(NULL);
    UINT d = 96;
    if (dc) {
        int v = GetDeviceCaps(dc, LOGPIXELSY);
        if (v > 0) d = (UINT)v;
        ReleaseDC(h, dc);
    }
    return d;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;

    SetDpiAware();

    /* WIC 解码图片需要 COM（预览与「图片…」导入都会用到） */
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    CfgLoad(&s_cfg);
    g_base = s_cfg;      /* 脏标记的基线 */
    SyncLastShapes();

    RegisterPanelClass(hInst);

    {   /* 自绘下拉与它的弹出列表 */
        WNDCLASSW sc;
        ZeroMemory(&sc, sizeof(sc));
        sc.lpfnWndProc = SelProc;
        sc.hInstance = hInst;
        sc.lpszClassName = SEL_CLASS;
        sc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassW(&sc);

        WNDCLASSW sp;
        ZeroMemory(&sp, sizeof(sp));
        sp.lpfnWndProc = SelPopupProc;
        sp.hInstance = hInst;
        sp.lpszClassName = SEL_POPUP;
        sp.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassW(&sp);
    }

    static const WCHAR pcls[] = L"IMEIndicatorPreview";
    WNDCLASSW pc;
    ZeroMemory(&pc, sizeof(pc));
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = hInst;
    pc.lpszClassName = pcls;
    pc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&pc);

    {   /* 悬停说明气泡 */
        WNDCLASSW hc;
        ZeroMemory(&hc, sizeof(hc));
        hc.lpfnWndProc = HintProc;
        hc.hInstance = hInst;
        hc.lpszClassName = HINT_CLASS;
        hc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassW(&hc);
    }

    static const WCHAR cls[] = L"IMEIndicatorSettings";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    /* 窗口尺寸按客户区反推：WIN_W/WIN_H 是内容尺寸，标题栏和边框会额外吃掉
       三十来个逻辑像素。DPI 只取一次，WM_CREATE 沿用同一值。
       宽度恢复上次用过的值（不窄于设计宽度，否则右列会被裁掉）。 */
    g_startupDpi = QueryDpi(NULL);
    if (g_startupDpi == 0) g_startupDpi = 96;
    int dpi = (int)g_startupDpi;

    /* WS_CLIPCHILDREN：父窗口重画（切页签、悬停高亮、缩放）时不要再涂到子窗口
       底下去 —— 否则整窗先擦一遍底再让子控件逐个补画，看着就是整片在闪。 */
    DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;

    int logicalW = s_cfg.settingsWidth > WIN_W ? s_cfg.settingsWidth : WIN_W;
    if (logicalW > 1600) logicalW = 1600;      /* 先粗夹一刀，再按工作区夹 */
    g_loadedWidth = logicalW;
    RECT rc = { 0, 0, MulDiv(logicalW, dpi, 96), MulDiv(WIN_H, dpi, 96) };
    ClientToWindowSize(&rc, style, 0, (UINT)dpi);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int waw = wa.right - wa.left, wah = wa.bottom - wa.top;
    if (ww > waw - 20) ww = waw - 20;
    if (wh > wah - 20) wh = wah - 20;
    int wx = wa.left + (waw - ww) / 2;
    int wy = wa.top + (wah - wh) / 2;

    HWND h = CreateWindowExW(WS_EX_COMPOSITED, cls, L"IMEIndicator 设置",
                             style, wx, wy, ww, wh,
                             NULL, NULL, hInst, NULL);
    if (!h) return 1;

    ShowWindow(h, SW_SHOWNORMAL);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ImgShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}
