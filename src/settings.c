#include "ime_indicator.h"
#include "shape.h"
#include "image.h"
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
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
#define ID_PICK_IMAGE   110   /* 110..112：三个状态块各一个「图片…」按钮 */

#define F_ENABLED   1
#define F_INHERIT   2
#define F_COLOR     3
#define F_SHAPE     4
#define F_SIZE      5
#define F_OFFX      6
#define F_OFFY      7
#define F_ALPHA     8
#define F_TRANSIENT 9
#define F_POLL      20
#define F_TRACK     21
#define F_STRATEGY  22
#define F_TIMEOUT   23
#define F_FULLSCREEN 24
#define F_COMPOSITION 25
#define F_WPSCOM    26
#define F_AUTOHIDE  27

#define PAGE_COUNT  7
#define PAGE_GLOBAL 5
#define PAGE_IGNORE 6

/* 逻辑坐标（96 DPI 基准） */
#define WIN_W      820
#define WIN_H      556
#define TAB_X      14
#define TAB_Y      8
#define TAB_H      28
#define PREVIEW_X  14
#define PREVIEW_Y  42
#define PREVIEW_H  66
#define CONTENT_Y  116
#define BOTTOM_H   44

static ImeCfg  s_cfg;
static HWND    g_hwnd = NULL;
static HWND    g_preview = NULL;
static int     g_page = 0;
static int     g_selState = 0;
static HFONT   g_font = NULL;
static HBRUSH  g_whiteBrush = NULL;
static int     g_dirty = 0;
static int     g_dpi = 96;
static UINT    g_startupDpi = 96;

/* ---------- 页定义 ---------- */
typedef struct {
    const WCHAR* name;
    ImeState     states[3];
} PageDef;

static const PageDef kPages[PAGE_COUNT] = {
    { L"美式键盘",   { IMEST_US, IMEST_US_CAPS, (ImeState)-1 } },
    { L"中文输入法", { IMEST_CN_CN, IMEST_CN_EN, IMEST_CN_CAPS } },
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

/* ---------- 自绘按钮 ----------
   参考 CapsEnhance 设置程序的观感：圆角、主按钮蓝底白字、次按钮浅底灰边，
   悬停淡蓝、按下加深。系统默认按钮是 Win95 的浮凸方块，和现在的页签/预览
   放在一起很割裂。 */
#define BTN_HOVER_PROP L"IMEIndicatorBtnHover"
#define CHK_PROP       L"IMEIndicatorCheck"      /* 是自绘复选框 */
#define CHK_STATE_PROP L"IMEIndicatorCheckState" /* 复选框的勾选状态 */

static LRESULT CALLBACK BtnSub(HWND h, UINT m, WPARAM w, LPARAM l,
                               UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    /* BS_OWNERDRAW 的按钮不保存勾选状态，BM_SETCHECK/BM_GETCHECK 由这里接管 */
    if (GetPropW(h, CHK_PROP)) {
        if (m == BM_SETCHECK) {
            SetPropW(h, CHK_STATE_PROP, (HANDLE)(INT_PTR)(w == BST_CHECKED ? 1 : 0));
            InvalidateRect(h, NULL, TRUE);
            return 0;
        }
        if (m == BM_GETCHECK)
            return GetPropW(h, CHK_STATE_PROP) ? BST_CHECKED : BST_UNCHECKED;
    }
    switch (m) {
    case WM_MOUSEMOVE:
        if (!GetPropW(h, BTN_HOVER_PROP)) {
            SetPropW(h, BTN_HOVER_PROP, (HANDLE)1);
            TRACKMOUSEEVENT t;
            ZeroMemory(&t, sizeof(t));
            t.cbSize = sizeof(t);
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            InvalidateRect(h, NULL, TRUE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, BTN_HOVER_PROP);
        InvalidateRect(h, NULL, TRUE);
        break;
    case WM_NCDESTROY:
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
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ open = SelectObject(di->hDC, pen);
    HGDIOBJ obr = SelectObject(di->hDC, br);
    RoundRect(di->hDC, rc.left, rc.top, rc.right - 1, rc.bottom - 1,
              rad * 2, rad * 2);
    SelectObject(di->hDC, open);
    SelectObject(di->hDC, obr);
    DeleteObject(pen);
    DeleteObject(br);

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

    if (di->itemState & ODS_FOCUS) {
        RECT frc = rc;
        InflateRect(&frc, -S(3), -S(3));
        DrawFocusRect(di->hDC, &frc);
    }
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

    if (di->itemState & ODS_FOCUS) {
        RECT frc = rc;
        InflateRect(&frc, -S(2), -S(2));
        DrawFocusRect(di->hDC, &frc);
    }
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

static void SetNumText(HWND e, int v) {
    if (!e) return;
    WCHAR vb[32];
    _snwprintf_s(vb, 32, _TRUNCATE, L"%d", v);
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

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    /* 控件的命令/滚动/自绘通知发到**父窗口**（现在父窗口是面板），
       统一转给主窗口处理，逻辑只写一份。 */
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_DRAWITEM:
    case WM_NOTIFY:
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
#define SEL_MAX_ITEMS 8
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
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, S(6), S(6));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);

    int ax = rc.right - S(16), ay = (rc.top + rc.bottom) / 2;
    HPEN ap = CreatePen(PS_SOLID, S(2),
                        enabled ? RGB(0x54, 0x54, 0x54) : RGB(0xB0, 0xB0, 0xB0));
    HGDIOBJ oa = SelectObject(dc, ap);
    MoveToEx(dc, ax - S(4), ay - S(2), NULL);
    LineTo(dc, ax, ay + S(2));
    LineTo(dc, ax + S(4), ay - S(2));
    SelectObject(dc, oa);
    DeleteObject(ap);

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

static LRESULT CALLBACK SelPopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    SelData* d = SelOf(h);
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

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
                InvalidateRect(owner, NULL, TRUE);
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
    InvalidateRect(g_selPopup, NULL, TRUE);
}

static LRESULT CALLBACK SelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    SelData* d = SelOf(h);
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        SelPaint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case CB_ADDSTRING:
        if (d && d->count < SEL_MAX_ITEMS) {
            lstrcpynW(d->items[d->count], (const WCHAR*)l, SEL_MAX_TEXT);
            int idx = d->count++;
            InvalidateRect(h, NULL, TRUE);
            return idx;
        }
        return CB_ERR;
    case CB_SETCURSEL:
        if (d && (int)w >= 0 && (int)w < d->count) {
            d->sel = (int)w;
            InvalidateRect(h, NULL, TRUE);
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
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    case WM_MOUSELEAVE:
        RemovePropW(h, BTN_HOVER_PROP);
        InvalidateRect(h, NULL, TRUE);
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
    SendMessageW(c, TBM_SETPOS, TRUE, val);
    RegMeta(c, block, field, label, x, y, w, h);
    return c;
}

/* ---------- 布局面板：3 个固定块 ---------- */
typedef struct {
    HWND title, enable, inherit, color, shape;
    HWND imgBtn, imgLabel;
    HWND lbl[6];      /* 颜色/形状/尺寸/偏移X/偏移Y/透明度 的静态标签 */
    HWND slSize, vSize, slOffX, vOffX, slOffY, vOffY, slAlpha, vAlpha;
    HWND transient;
} Block;

static Block g_blk[3];
static ImeState g_blkState[3] = { (ImeState)-1, (ImeState)-1, (ImeState)-1 };
static HWND  g_layoutPanel = NULL;
static HWND  g_globalPanel = NULL;
static HWND  g_ignorePanel = NULL;

static void BuildStateBlock(int i, int y) {
    Block* b = &g_blk[i];
    const int x0 = 14;
    const int lx = x0 + 6;
    const int rx = x0 + 380;

    b->title = CLabel(L"", lx, y + 2, 150, 20);
    SendMessageW(b->title, WM_SETFONT, (WPARAM)BoldFont(), TRUE);
    b->enable = CCheck(L"启用", lx + 156, y, 60, 22, i, F_ENABLED, 1);
    b->inherit = CCheck(L"跟随基础状态", lx + 220, y, 120, 22, i, F_INHERIT, 0);
    y += 24;

    b->lbl[0] = CLabel(L"颜色", lx, y + 4, 54, 20);
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
    b->imgLabel = CLabel(L"", rx + 256, y + 4, 150, 20);
    g_imgLabel[i] = b->imgLabel;
    y += 26;

    struct { const WCHAR* nm; int field; int lo, hi; } sl[4] = {
        { L"尺寸",   F_SIZE,   4,  96 },
        { L"偏移 X", F_OFFX, -40,  40 },
        { L"偏移 Y", F_OFFY, -40,  40 },
        { L"透明度", F_ALPHA,  0, 255 },
    };
    HWND* psl[4] = { &b->slSize, &b->slOffX, &b->slOffY, &b->slAlpha };
    HWND* pvl[4] = { &b->vSize,  &b->vOffX,  &b->vOffY,  &b->vAlpha };
    for (int k = 0; k < 4; k++) {
        int cx = (k % 2 == 0) ? lx : rx;
        int cy = y + (k / 2) * 23;
        b->lbl[2 + k] = CLabel(sl[k].nm, cx, cy + 3, 54, 20);
        *pvl[k] = CEditNum(cx + 244, cy, 48, 22, i, sl[k].field);
        *psl[k] = CSlider(cx + 56, cy, 184, 22, i, sl[k].field,
                          sl[k].lo, sl[k].hi, sl[k].lo, *pvl[k]);
    }
    y += 46;

    b->transient = CCheck(L"", lx, y, 330, 22, i, F_TRANSIENT, 0);
    y += 22;

    HWND sep = CreateWindowExW(0, L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, x0, y + 2, WIN_W - 28, 2,
        P, NULL, NULL, NULL);
    RegMeta(sep, -2, 0, NULL, x0, y + 2, WIN_W - 28, 2);
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

    SendMessageW(b->slSize, TBM_SETPOS, TRUE, eff.size);
    SendMessageW(b->slOffX, TBM_SETPOS, TRUE, eff.offsetX);
    SendMessageW(b->slOffY, TBM_SETPOS, TRUE, eff.offsetY);
    SendMessageW(b->slAlpha, TBM_SETPOS, TRUE, eff.alpha);
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
        { L"状态检测间隔", F_POLL,      5, 60000, &g_slPoll,    &g_vPoll },
        { L"光标追踪间隔", F_TRACK,     5, 200,   &g_slTrack,   &g_vTrack },
        { L"光标查询超时", F_TIMEOUT,  20, 5000,  &g_slTimeout, &g_vTimeout },
        { L"临时显示时长", F_AUTOHIDE,  0, 60000, &g_slAutoHide,&g_vAutoHide },
    };
    int vals[4] = { s_cfg.pollMs, s_cfg.trackMs, s_cfg.caretTimeoutMs, s_cfg.autoHideMs };
    for (int i = 0; i < 4; i++) {
        CLabel(sl[i].nm, x0 + 6, y + 3, 110, 20);
        *sl[i].vl = CEditNum(x0 + 346, y, 64, 22, -1, sl[i].field);
        CLabel(L"ms", x0 + 414, y + 3, 30, 20);
        *sl[i].sl = CSlider(x0 + 120, y, 220, 22, -1, sl[i].field,
                            sl[i].lo, sl[i].hi, vals[i], *sl[i].vl);
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
    SendMessageW(g_slPoll, TBM_SETPOS, TRUE, s_cfg.pollMs);
    SendMessageW(g_slTrack, TBM_SETPOS, TRUE, s_cfg.trackMs);
    SendMessageW(g_slTimeout, TBM_SETPOS, TRUE, s_cfg.caretTimeoutMs);
    SendMessageW(g_slAutoHide, TBM_SETPOS, TRUE, s_cfg.autoHideMs);
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
static LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(0xFA, 0xFA, 0xFA));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

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
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void InvalidatePreview(void) {
    if (g_preview) InvalidateRect(g_preview, NULL, TRUE);
}

/* ---------- 提交 ---------- */
static int SliderPos(HWND s) { return (int)SendMessageW(s, TBM_GETPOS, 0, 0); }
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
            if (IsCapsState(s)) {
                raw->inherit = Checked(b->inherit) ? CapsBase(s) : -1;
            }
            int sel = (int)SendMessageW(b->shape, CB_GETCURSEL, 0, 0);
            raw->shape = (sel == 0) ? SHAPE_NONE :
                         (sel == 2) ? SHAPE_TRIANGLE :
                         (sel == 3) ? SHAPE_IMAGE : SHAPE_CIRCLE;
            raw->have |= SF_SHAPE;
            raw->size = SliderPos(b->slSize);     raw->have |= SF_SIZE;
            raw->offsetX = SliderPos(b->slOffX);  raw->have |= SF_OFFSETX;
            raw->offsetY = SliderPos(b->slOffY);  raw->have |= SF_OFFSETY;
            raw->alpha = SliderPos(b->slAlpha);   raw->have |= SF_ALPHA;
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

/* ---------- 保存 / 重启 ---------- */
static void DoSave(int restart) {
    CommitForm();
    if (!CfgSave(&s_cfg)) {
        MessageBoxW(g_hwnd, L"写入 IMEIndicator.json 失败。\n\n"
                            L"请确认程序所在目录可写（放在 Program Files 下时需要管理员权限）。",
                    L"保存失败", MB_ICONERROR | MB_OK);
        return;
    }
    g_dirty = 0;

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
    if (page < 5) g_selState = (int)kPages[page].states[0];
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
            HBRUSH hb = CreateSolidBrush(RGB(0xE5, 0xF1, 0xFB));
            HGDIOBJ ob = SelectObject(dc, hb);
            HGDIOBJ opn = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1,
                      S(8), S(8));
            SelectObject(dc, opn);
            SelectObject(dc, ob);
            DeleteObject(hb);
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
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"图片 (*.png;*.ico;*.bmp;*.jpg)\0*.png;*.ico;*.bmp;*.jpg\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    WCHAR name[CFG_NAME_MAX];
    if (!ImgImport(file, name, CFG_NAME_MAX)) {
        MessageBoxW(g_hwnd, L"无法把图片复制到 shapes 目录。\n\n"
                            L"请确认程序所在目录可写。",
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

/* ---------- 命令处理 ---------- */
static void OnCommand(int id, HWND ctl) {
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
        s_cfg.st[s].shape = (sel == 0) ? SHAPE_NONE :
                            (sel == 2) ? SHAPE_TRIANGLE :
                            (sel == 3) ? SHAPE_IMAGE : SHAPE_CIRCLE;
        s_cfg.st[s].have |= SF_SHAPE;
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
    SendMessageW(sl, TBM_SETPOS, TRUE, v);
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

/* ---------- 滑块自绘 ----------
   默认 trackbar 是 Win95 的蚀刻槽 + 小方块滑块，和圆角按钮/下划线页签放在
   一起很割裂。这里接管 NM_CUSTOMDRAW：4px 圆角轨道、已选段蓝色填充、
   14px 圆形滑块。命中判定仍走控件默认过程（画的和点的是同一套几何）。 */
static LRESULT OnSliderCustomDraw(NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage == CDDS_PREPAINT) {
        HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
        FillRect(cd->hdc, &cd->rc, bg);
        DeleteObject(bg);
        return CDRF_NOTIFYITEMDRAW;
    }
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;

    HWND h = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    RECT rc = cd->rc;

    if (cd->dwItemSpec == TBCD_CHANNEL) {
        int lo = (int)SendMessageW(h, TBM_GETRANGEMIN, 0, 0);
        int hi = (int)SendMessageW(h, TBM_GETRANGEMAX, 0, 0);
        int pos = (int)SendMessageW(h, TBM_GETPOS, 0, 0);

        int cy = (rc.top + rc.bottom) / 2;
        int th = S(4);
        RECT track = { rc.left, cy - th / 2, rc.right, cy + th / 2 };
        int thumbW = S(14);
        int travel = (track.right - track.left) - thumbW;
        if (travel < 1) travel = 1;
        double frac = (hi > lo) ? (double)(pos - lo) / (double)(hi - lo) : 0.0;
        int cx = track.left + thumbW / 2 + (int)(travel * frac);

        HBRUSH gb = CreateSolidBrush(RGB(0xE0, 0xE0, 0xE0));
        HPEN gp = CreatePen(PS_SOLID, 1, RGB(0xE0, 0xE0, 0xE0));
        HGDIOBJ ob = SelectObject(dc, gb);
        HGDIOBJ op = SelectObject(dc, gp);
        RoundRect(dc, track.left, track.top, track.right, track.bottom, th, th);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(gb);
        DeleteObject(gp);

        if (cx > track.left) {
            HBRUSH bb = CreateSolidBrush(RGB(0x00, 0x78, 0xD7));
            HPEN bp = CreatePen(PS_SOLID, 1, RGB(0x00, 0x78, 0xD7));
            ob = SelectObject(dc, bb);
            op = SelectObject(dc, bp);
            RoundRect(dc, track.left, track.top, cx, track.bottom, th, th);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(bb);
            DeleteObject(bp);
        }
        return CDRF_SKIPDEFAULT;
    }

    if (cd->dwItemSpec == TBCD_THUMB) {
        HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        int d = S(14);
        int cx = (rc.left + rc.right) / 2;
        int cy = (rc.top + rc.bottom) / 2;
        HBRUSH b = CreateSolidBrush(RGB(0x00, 0x78, 0xD7));
        HPEN p = CreatePen(PS_SOLID, 1, RGB(0x00, 0x78, 0xD7));
        HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, p);
        Ellipse(dc, cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(p);
        return CDRF_SKIPDEFAULT;
    }

    return CDRF_DODEFAULT;
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        g_dpi = (int)g_startupDpi;
        if (g_dpi <= 0) g_dpi = 96;
        g_whiteBrush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));

        /* 预览 */
        g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"IMEIndicatorPreview", NULL,
            WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, h, NULL, NULL, NULL);

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
            b = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_CLOSE, NULL, NULL);
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
        ShowPage(0);
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
        HWND b1 = GetDlgItem(h, ID_SAVE);
        HWND b2 = GetDlgItem(h, ID_SAVE_RESTART);
        HWND b3 = GetDlgItem(h, ID_CLOSE);
        if (b1) SetWindowPos(b1, HWND_TOP, S(14), by, S(84), bh, SWP_NOACTIVATE);
        if (b2) SetWindowPos(b2, HWND_TOP, S(14 + 92), by, S(116), bh, SWP_NOACTIVATE);
        if (b3) SetWindowPos(b3, HWND_TOP, rc.right - S(14 + 84), by, S(84), bh, SWP_NOACTIVATE);
        return 0;
    }

    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l;
        if (nh->code == NM_CUSTOMDRAW)
            return OnSliderCustomDraw((NMCUSTOMDRAW*)l);
        break;
    }

    case WM_COMMAND: {
        int code = HIWORD(w);
        HWND ctl = (HWND)l;
        if (code == EN_CHANGE)    { OnNumEditChanged(ctl); return 0; }
        if (code == EN_KILLFOCUS) { OnNumEditDone(ctl); return 0; }
        if (code == BN_CLICKED && GetPropW(ctl, CHK_PROP)) {
            /* 自绘复选框不会自己翻转状态，这里补上 */
            int st = (int)SendMessageW(ctl, BM_GETCHECK, 0, 0);
            SendMessageW(ctl, BM_SETCHECK,
                         st == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            InvalidateRect(ctl, NULL, TRUE);
        }
        OnCommand(LOWORD(w), ctl);
        return 0;
    }

    case WM_HSCROLL:
        OnHScroll((HWND)l);
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

        HBRUSH b = CreateSolidBrush(RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xAD, 0xAD, 0xAD));
        HGDIOBJ ob = SelectObject(di->hDC, b);
        HGDIOBJ op = SelectObject(di->hDC, pen);
        RoundRect(di->hDC, di->rcItem.left, di->rcItem.top,
                  di->rcItem.right - 1, di->rcItem.bottom - 1, S(6), S(6));
        SelectObject(di->hDC, ob);
        SelectObject(di->hDC, op);
        DeleteObject(b);
        DeleteObject(pen);
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
        if (g_dirty) {
            int r = MessageBoxW(h, L"有未保存的修改，要保存吗？",
                                L"IMEIndicator 设置", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) DoSave(0);
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
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
       三十来个逻辑像素。DPI 只取一次，WM_CREATE 沿用同一值。 */
    g_startupDpi = QueryDpi(NULL);
    if (g_startupDpi == 0) g_startupDpi = 96;
    int dpi = (int)g_startupDpi;

    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
    RECT rc = { 0, 0, MulDiv(WIN_W, dpi, 96), MulDiv(WIN_H, dpi, 96) };
    {
        typedef BOOL (WINAPI* PFN_AdjForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
        static PFN_AdjForDpi pfn = NULL;
        static int tried = 0;
        if (!tried) {
            tried = 1;
            HMODULE u = GetModuleHandleW(L"user32.dll");
            if (u) pfn = (PFN_AdjForDpi)GetProcAddress(u, "AdjustWindowRectExForDpi");
        }
        BOOL ok = FALSE;
        if (pfn) ok = pfn(&rc, style, FALSE, 0, (UINT)dpi);
        if (!ok) AdjustWindowRectEx(&rc, style, FALSE, 0);
    }
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
