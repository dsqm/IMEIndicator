#include "ime_indicator.h"
#include "shape.h"
#include "image.h"
#include <stdio.h>
#include <wchar.h>
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

static HFONT BaseFont(void) {
    if (!g_font) {
        NONCLIENTMETRICSW ncm;
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return g_font;
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
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             x, y, w, h, P, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    RegMeta(c, block, field, NULL, x, y, w, h);
    return c;
}

static HWND CButton(const WCHAR* t, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             x, y, w, h, P, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
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

static HWND CCombo(int x, int y, int w, int h, int block, int field) {
    HWND c = CreateWindowExW(0, L"COMBOBOX", NULL,
                             WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                             x, y, w, h, P, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(c, block, field, NULL, x, y, w, h);
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
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                rx + 174, y, 76, 22, P,
                                (HMENU)(INT_PTR)(ID_PICK_IMAGE + i), NULL, NULL);
    SendMessageW(b->imgBtn, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
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
        *pvl[k] = CLabel(L"0", cx + 244, cy + 3, 48, 20);
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
    _snwprintf_s(vb, 64, _TRUNCATE, L"%d", eff.size);    SetWindowTextW(b->vSize, vb);
    _snwprintf_s(vb, 64, _TRUNCATE, L"%d", eff.offsetX); SetWindowTextW(b->vOffX, vb);
    _snwprintf_s(vb, 64, _TRUNCATE, L"%d", eff.offsetY); SetWindowTextW(b->vOffY, vb);
    _snwprintf_s(vb, 64, _TRUNCATE, L"%d", eff.alpha);   SetWindowTextW(b->vAlpha, vb);

    if (s_cfg.autoHideMs > 0)
        _snwprintf_s(vb, 64, _TRUNCATE, L"状态变化后只显示 %d ms", s_cfg.autoHideMs);
    else
        _snwprintf_s(vb, 64, _TRUNCATE, L"状态变化后常显（全局已关闭）");
    SetWindowTextW(b->transient, vb);
    SendMessageW(b->transient, BM_SETCHECK, eff.transient ? BST_CHECKED : BST_UNCHECKED, 0);

    ShowBlock(i);

    /* 图片行：只在形状=图片时露文件名；文件缺失/解码失败标红，提示会回退圆点 */
    g_imgBad[i] = 0;
    if (eff.shape == SHAPE_IMAGE) {
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
    CLabel(L"检测", x0, y, 200, 22);
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
        WCHAR vb[32];
        _snwprintf_s(vb, 32, _TRUNCATE, L"%d ms", vals[i]);
        *sl[i].vl = CLabel(vb, x0 + 350, y + 3, 80, 20);
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

    CLabel(L"隐藏", x0, y, 200, 22);
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
    WCHAR vb[32];
    _snwprintf_s(vb, 32, _TRUNCATE, L"%d ms", s_cfg.pollMs);     SetWindowTextW(g_vPoll, vb);
    _snwprintf_s(vb, 32, _TRUNCATE, L"%d ms", s_cfg.trackMs);    SetWindowTextW(g_vTrack, vb);
    _snwprintf_s(vb, 32, _TRUNCATE, L"%d ms", s_cfg.caretTimeoutMs); SetWindowTextW(g_vTimeout, vb);
    _snwprintf_s(vb, 32, _TRUNCATE, L"%d ms", s_cfg.autoHideMs); SetWindowTextW(g_vAutoHide, vb);
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

static void PaintTabs(HDC dc) {
    int x = S(TAB_X);
    HFONT old = (HFONT)SelectObject(dc, BaseFont());
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < PAGE_COUNT; i++) {
        int w = TabWidth(dc, i);
        RECT r = { x, S(TAB_Y), x + w, S(TAB_Y + TAB_H) };
        if (i == g_page) {
            SetTextColor(dc, RGB(0x00, 0x67, 0xC0));
            LOGFONTW lf;
            GetObjectW(BaseFont(), sizeof(lf), &lf);
            lf.lfWeight = FW_BOLD;
            HFONT bold = CreateFontIndirectW(&lf);
            HFONT prev = (HFONT)SelectObject(dc, bold);
            DrawTextW(dc, kPages[i].name, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, prev);
            DeleteObject(bold);
            RECT ul = { x + S(8), S(TAB_Y + TAB_H - 3), x + w - S(8), S(TAB_Y + TAB_H - 1) };
            HBRUSH b = CreateSolidBrush(RGB(0x00, 0x67, 0xC0));
            FillRect(dc, &ul, b);
            DeleteObject(b);
        } else {
            SetTextColor(dc, RGB(0x4A, 0x4A, 0x4A));
            DrawTextW(dc, kPages[i].name, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
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

static void OnHScroll(HWND slider) {
    Meta* m = FindMeta(slider);
    if (!m) return;
    int v = SliderPos(slider);

    if (m->label) {
        WCHAR vb[32];
        int isMs = (m->field == F_POLL || m->field == F_TRACK ||
                    m->field == F_TIMEOUT || m->field == F_AUTOHIDE);
        _snwprintf_s(vb, 32, _TRUNCATE, isMs ? L"%d ms" : L"%d", v);
        SetWindowTextW(m->label, vb);
    }

    if (m->block >= 0) {
        ImeState s = g_blkState[m->block];
        if ((int)s >= 0) {
            StateStyle* raw = &s_cfg.st[s];
            switch (m->field) {
            case F_SIZE:  raw->size = v;    raw->have |= SF_SIZE;    break;
            case F_OFFX:  raw->offsetX = v; raw->have |= SF_OFFSETX; break;
            case F_OFFY:  raw->offsetY = v; raw->have |= SF_OFFSETY; break;
            case F_ALPHA: raw->alpha = v;   raw->have |= SF_ALPHA;   break;
            default: break;
            }
            InvalidatePreview();
        }
    } else if (m->block == -1) {
        switch (m->field) {
        case F_POLL:     s_cfg.pollMs = v; break;
        case F_TRACK:    s_cfg.trackMs = v; break;
        case F_TIMEOUT:  s_cfg.caretTimeoutMs = v; break;
        case F_AUTOHIDE:
            s_cfg.autoHideMs = v;
            if (g_page < 5) { for (int i = 0; i < 3; i++) FillBlock(i, g_blkState[i]); }
            break;
        default: break;
        }
    }
    g_dirty = 1;
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
            b = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_SAVE, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            b = CreateWindowExW(0, L"BUTTON", L"保存并重启", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_SAVE_RESTART, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            b = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_CLOSE, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
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

        int by = rc.bottom - S(BOTTOM_H);
        HWND b1 = GetDlgItem(h, ID_SAVE);
        HWND b2 = GetDlgItem(h, ID_SAVE_RESTART);
        HWND b3 = GetDlgItem(h, ID_CLOSE);
        if (b1) SetWindowPos(b1, HWND_TOP, S(14), by, S(80), S(26), SWP_NOACTIVATE);
        if (b2) SetWindowPos(b2, HWND_TOP, S(14 + 88), by, S(110), S(26), SWP_NOACTIVATE);
        if (b3) SetWindowPos(b3, HWND_TOP, rc.right - S(14 + 80), by, S(80), S(26), SWP_NOACTIVATE);
        return 0;
    }

    case WM_COMMAND:
        OnCommand(LOWORD(w), (HWND)l);
        return 0;

    case WM_HSCROLL:
        OnHScroll((HWND)l);
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
        if (di->CtlType == ODT_BUTTON) {
            Meta* mm = FindMeta(di->hwndItem);
            DWORD rgb = 0xFF0000;
            if (mm && mm->block >= 0) {
                ImeState s = g_blkState[mm->block];
                if ((int)s >= 0) {
                    StateStyle eff;
                    CfgStateStyle(&s_cfg, s, &eff);
                    rgb = eff.rgb;
                }
            }
            HBRUSH b = CreateSolidBrush(RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
            FillRect(di->hDC, &di->rcItem, b);
            DeleteObject(b);
            FrameRect(di->hDC, &di->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH));
            return TRUE;
        }
        return FALSE;
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
