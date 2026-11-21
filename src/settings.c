#include "ime_indicator.h"
#include <stdio.h>
#include <wchar.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

/* ================= IMEIndicator 设置程序 =================
   独立 exe，纯 Win32。分页表单（原型变体 C）：自定义页签 + 预览条 + 表单。

   为什么是独立 exe：
   - 主程序常以管理员运行，设置程序需要能以普通权限启动并写配置；
   - 设置 UI 崩了不该带崩指示器；不跑设置时也不占主程序体积。

   保存流程：写 IMEIndicator.json -> 调 IMEIndicator.exe --restart。
   重启失败（典型：设置程序未提权、主程序是管理员）时提示手动重启 ——
   这是有意接受的降级，不做提权。

   ★ 布局全部按 **96 DPI 的逻辑坐标**写死，建完表单后由 ScaleControls()
     统一乘 DPI 系数。这样 100%/150%/175% 下比例一致，标签不会被裁。

   每个控件创建时登记"它对应哪个状态的哪个字段"，提交时按字段派发，
   不靠控件下标算术（那玩意儿一改布局就会错位）。 */

#define IDC_BASE        4000
#define ID_SAVE         100
#define ID_SAVE_RESTART 101
#define ID_CLOSE        102
#define ID_ADD_IGNORE   103
#define ID_DEL_IGNORE   104

/* 字段 id */
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

/* 逻辑坐标（96 DPI 基准）。
   WIN_H 要保证 175% 缩放下整窗仍能塞进 1080p 屏：540*1.75 ≈ 945 + 标题栏 < 1040。 */
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
static HBRUSH  g_bgBrush = NULL;
static HBRUSH  g_whiteBrush = NULL;
static int     g_dirty = 0;
static int     g_dpi = 96;
static UINT    g_startupDpi = 96;   /* WinMain 里定下来，窗口尺寸与控件缩放共用同一值 */

/* 取窗口所在显示器的 DPI。优先 GetDpiForWindow（Win10 1607+），
   退回屏幕 DC 的 LOGPIXELSY。★ 必须和窗口尺寸用**同一个值**：
   两处各取各的（之前一处用屏幕 DC、一处用窗口 DC）会导致控件被放大得比
   窗口多，内容溢出/被裁。 */
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

/* 逻辑像素 -> 物理像素 */
static int S(int v) {
    if (g_dpi <= 0) g_dpi = 96;
    return MulDiv(v, g_dpi, 96);
}

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

/* 给人看的状态名（JSON 键名仍用 CfgStateName） */
static const WCHAR* StateLabel(ImeState s) {
    switch (s) {
    case IMEST_US:         return L"普通";
    case IMEST_US_CAPS:    return L"大写";
    case IMEST_CN_CN:      return L"中文";
    case IMEST_CN_EN:      return L"英文";
    case IMEST_CN_CAPS:    return L"大写";
    case IMEST_JP:         return L"普通";
    case IMEST_JP_CAPS:    return L"大写";
    case IMEST_KR:         return L"普通";
    case IMEST_KR_CAPS:    return L"大写";
    case IMEST_OTHER:      return L"普通";
    case IMEST_OTHER_CAPS: return L"大写";
    default:               return L"?";
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

/* ---------- 控件登记 ---------- */
typedef struct {
    HWND hwnd;
    int  state;   /* ImeState；-1 = 全局/其它 */
    int  field;   /* F_* */
    HWND label;   /* 滑块的数值标签（可空） */
    int  x, y, w, h;   /* 逻辑坐标 */
} Ctl;

static Ctl  g_ctls[256];
static int  g_ctlCount = 0;
static HWND g_ignoreList = NULL;
static HWND g_ignoreEdit = NULL;

static void ResetCtls(void) {
    for (int i = 0; i < g_ctlCount; i++)
        if (g_ctls[i].hwnd && IsWindow(g_ctls[i].hwnd))
            DestroyWindow(g_ctls[i].hwnd);
    g_ctlCount = 0;
    g_ignoreList = NULL;
    g_ignoreEdit = NULL;
}

static void Reg(HWND h, int state, int field, HWND label, int x, int y, int w, int hh) {
    if (!h) return;
    if (g_ctlCount >= (int)(sizeof(g_ctls) / sizeof(g_ctls[0]))) return;
    Ctl* c = &g_ctls[g_ctlCount++];
    c->hwnd = h;
    c->state = state;
    c->field = field;
    c->label = label;
    c->x = x; c->y = y; c->w = w; c->h = hh;
}

static Ctl* FindCtl(HWND h) {
    for (int i = 0; i < g_ctlCount; i++)
        if (g_ctls[i].hwnd == h) return &g_ctls[i];
    return NULL;
}

/* 按 DPI 系数把逻辑坐标套到所有控件上 */
static void ScaleControls(void) {
    for (int i = 0; i < g_ctlCount; i++) {
        Ctl* c = &g_ctls[i];
        if (!c->hwnd || !IsWindow(c->hwnd)) continue;
        SetWindowPos(c->hwnd, NULL, S(c->x), S(c->y), S(c->w), S(c->h),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* ---------- 字体 ---------- */
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

/* ---------- 控件工厂（全部登记逻辑坐标） ---------- */
static HWND CLabel(const WCHAR* t, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             x, y, w, h, g_hwnd, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    Reg(c, -1, 0, NULL, x, y, w, h);
    return c;
}

static HWND CCheck(const WCHAR* t, int x, int y, int w, int h,
                   int state, int field, int checked) {
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                             x, y, w, h, g_hwnd,
                             (HMENU)(INT_PTR)(IDC_BASE + g_ctlCount), NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    Reg(c, state, field, NULL, x, y, w, h);
    return c;
}

static HWND CButton(const WCHAR* t, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    Reg(c, -2, 0, NULL, x, y, w, h);   /* -2 = 页内按钮，随页销毁重建 */
    return c;
}

/* 常驻按钮（保存/关闭）：**不登记** —— 登记了就会被 ResetCtls 随页销毁，
   而它们只在 WM_SIZE 里定位，不需要参与 DPI 缩放登记。 */
static HWND CButtonFixed(const WCHAR* t, int id) {
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    return c;
}

static HWND CEdit(int x, int y, int w, int h, const WCHAR* text) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             x, y, w, h, g_hwnd,
                             (HMENU)(INT_PTR)(IDC_BASE + g_ctlCount), NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    Reg(c, -1, 0, NULL, x, y, w, h);
    return c;
}

static HWND CCombo(int x, int y, int w, int h, int state, int field) {
    HWND c = CreateWindowExW(0, L"COMBOBOX", NULL,
                             WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                             x, y, w, h, g_hwnd,
                             (HMENU)(INT_PTR)(IDC_BASE + g_ctlCount), NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    Reg(c, state, field, NULL, x, y, w, h);
    return c;
}

static HWND CColorBtn(int x, int y, int w, int h, int state) {
    HWND c = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             x, y, w, h, g_hwnd,
                             (HMENU)(INT_PTR)(IDC_BASE + g_ctlCount), NULL, NULL);
    Reg(c, state, F_COLOR, NULL, x, y, w, h);
    return c;
}

static HWND CSlider(int x, int y, int w, int h, int state, int field,
                    int lo, int hi, int val, HWND label) {
    HWND c = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
                             WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                             x, y, w, h, g_hwnd,
                             (HMENU)(INT_PTR)(IDC_BASE + g_ctlCount), NULL, NULL);
    SendMessageW(c, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(c, TBM_SETPOS, TRUE, val);
    Reg(c, state, field, label, x, y, w, h);
    return c;
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
            COLORREF col = RGB((st.rgb >> 16) & 0xFF, (st.rgb >> 8) & 0xFF, st.rgb & 0xFF);
            HBRUSH b = CreateSolidBrush(col);
            HGDIOBJ oldB = SelectObject(dc, b);
            HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
            if (st.shape == SHAPE_TRIANGLE) {
                POINT pts[3];
                pts[0].x = x + sz / 2; pts[0].y = y;
                pts[1].x = x + sz;     pts[1].y = y + sz;
                pts[2].x = x;          pts[2].y = y + sz;
                Polygon(dc, pts, 3);
            } else {
                Ellipse(dc, x, y, x + sz, y + sz);
            }
            SelectObject(dc, oldP);
            SelectObject(dc, oldB);
            DeleteObject(b);
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

/* ---------- 表单 ---------- */

/* 一个状态块，逻辑高约 122。返回下一个 y。 */
static int BuildStateBlock(ImeState s, int y) {
    StateStyle* raw = &s_cfg.st[s];
    StateStyle eff;
    CfgStateStyle(&s_cfg, s, &eff);

    int isCaps = IsCapsState(s);
    int inherited = isCaps && raw->inherit >= 0;

    const int x0 = 14;
    const int lx = x0 + 6;        /* 左列 x */
    const int rx = x0 + 380;      /* 右列 x */

    /* 标题行 */
    WCHAR title[160];
    _snwprintf_s(title, 160, _TRUNCATE, L"%s（%s）", StateLabel(s), CfgStateName(s));
    CLabel(title, lx, y + 2, 150, 20);
    CCheck(L"启用", lx + 156, y, 60, 22, (int)s, F_ENABLED, raw->enabled);
    if (isCaps) {
        CCheck(L"跟随基础状态", lx + 220, y, 120, 22, (int)s, F_INHERIT, inherited);
    }
    y += 24;

    /* 颜色 | 形状 */
    CLabel(L"颜色", lx, y + 4, 54, 20);
    CColorBtn(lx + 58, y, 60, 22, (int)s);

    CLabel(L"形状", rx, y + 4, 54, 20);
    {
        HWND c = CCombo(rx + 58, y, 130, 180, (int)s, F_SHAPE);
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"不显示");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"圆");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"三角形");
        int sel = (eff.shape == SHAPE_NONE) ? 0 : (eff.shape == SHAPE_TRIANGLE) ? 2 : 1;
        SendMessageW(c, CB_SETCURSEL, sel, 0);
    }
    y += 26;

    /* 尺寸 | 偏移 X  /  偏移 Y | 透明度 */
    struct { const WCHAR* nm; int field; int lo, hi, val; } sl[4] = {
        { L"尺寸",   F_SIZE,   4,  96, eff.size },
        { L"偏移 X", F_OFFX, -40,  40, eff.offsetX },
        { L"偏移 Y", F_OFFY, -40,  40, eff.offsetY },
        { L"透明度", F_ALPHA,  0, 255, eff.alpha },
    };
    for (int i = 0; i < 4; i++) {
        int cx = (i % 2 == 0) ? lx : rx;
        int cy = y + (i / 2) * 23;
        CLabel(sl[i].nm, cx, cy + 3, 54, 20);
        WCHAR vb[24];
        _snwprintf_s(vb, 24, _TRUNCATE, L"%d", sl[i].val);
        HWND lab = CLabel(vb, cx + 244, cy + 3, 48, 20);
        CSlider(cx + 56, cy, 184, 22, (int)s, sl[i].field, sl[i].lo, sl[i].hi, sl[i].val, lab);
    }
    y += 46;

    CCheck(L"状态变化后只显示一会儿", lx, y, 220, 22, (int)s, F_TRANSIENT, eff.transient);
    y += 22;

    /* 跟随状态下整块置灰（启用与跟随本身除外） */
    if (inherited) {
        for (int i = 0; i < g_ctlCount; i++) {
            Ctl* c = &g_ctls[i];
            if (c->state != (int)s) continue;
            if (c->field == F_INHERIT || c->field == F_ENABLED) continue;
            EnableWindow(c->hwnd, FALSE);
            if (c->label) EnableWindow(c->label, FALSE);
        }
    }

    HWND sep = CreateWindowExW(0, L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, x0, y + 2, WIN_W - 28, 2,
        g_hwnd, NULL, NULL, NULL);
    Reg(sep, -1, 0, NULL, x0, y + 2, WIN_W - 28, 2);
    y += 8;
    return y;
}

static void BuildLayoutPage(int page) {
    int y = CONTENT_Y;
    for (int i = 0; i < 3; i++) {
        ImeState s = kPages[page].states[i];
        if ((int)s < 0) break;
        y = BuildStateBlock(s, y);
    }
}

static void BuildGlobalPage(void) {
    int y = CONTENT_Y, x0 = 14;
    CLabel(L"检测", x0, y, 200, 22);
    y += 24;

    struct { const WCHAR* nm; int field; int lo, hi, val; } sl[4] = {
        { L"状态检测间隔", F_POLL,      5, 60000, s_cfg.pollMs },
        { L"光标追踪间隔", F_TRACK,     5, 200,   s_cfg.trackMs },
        { L"光标查询超时", F_TIMEOUT,  20, 5000,  s_cfg.caretTimeoutMs },
        { L"临时显示时长", F_AUTOHIDE,  0, 60000, s_cfg.autoHideMs },
    };
    for (int i = 0; i < 4; i++) {
        CLabel(sl[i].nm, x0 + 6, y + 3, 110, 20);
        WCHAR vb[32];
        _snwprintf_s(vb, 32, _TRUNCATE, L"%d ms", sl[i].val);
        HWND lab = CLabel(vb, x0 + 350, y + 3, 80, 20);
        CSlider(x0 + 120, y, 220, 22, -1, sl[i].field, sl[i].lo, sl[i].hi, sl[i].val, lab);
        y += 26;
    }

    y += 8;
    CLabel(L"中英判定策略", x0 + 6, y + 3, 110, 20);
    {
        HWND c = CCombo(x0 + 120, y, 220, 180, -1, F_STRATEGY);
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"自动学习（推荐）");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"只看 IME open 状态");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"只看转换模式");
        SendMessageW(c, CB_SETCURSEL, s_cfg.imeStrategy, 0);
    }
    y += 34;

    CLabel(L"隐藏", x0, y, 200, 22);
    y += 24;
    CCheck(L"前景窗口全屏时隐藏", x0 + 6, y, 300, 22, -1, F_FULLSCREEN, s_cfg.hideFullscreen);
    y += 24;
    CCheck(L"输入法组合窗出现时隐藏", x0 + 6, y, 300, 22, -1, F_COMPOSITION, s_cfg.hideComposition);
    y += 24;
    CCheck(L"启用 WPS 光标通道", x0 + 6, y, 300, 22, -1, F_WPSCOM, s_cfg.wpsCom);
    y += 34;

    CLabel(L"尺寸与偏移按 96 DPI 的逻辑像素计算，高 DPI 屏上会自动放大。",
           x0, y, 640, 40);
}

static void BuildIgnorePage(void) {
    int y = CONTENT_Y, x0 = 14;
    CLabel(L"命中名单的程序彻底隐身：不做状态检测、不做光标查询、不显示提示。",
           x0, y, 740, 22);
    y += 22;
    CLabel(L"每行一个进程名，.exe 可写可不写。", x0, y, 740, 22);
    y += 26;

    g_ignoreList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        x0, y, 420, 258, g_hwnd,
        (HMENU)(INT_PTR)(IDC_BASE + g_ctlCount), NULL, NULL);
    SendMessageW(g_ignoreList, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    Reg(g_ignoreList, -1, 0, NULL, x0, y, 420, 258);
    for (int i = 0; i < s_cfg.ignoreCount; i++)
        SendMessageW(g_ignoreList, LB_ADDSTRING, 0, (LPARAM)s_cfg.ignore[i]);
    y += 268;

    g_ignoreEdit = CEdit(x0, y, 300, 24, L"");
    CButton(L"添加", x0 + 310, y, 70, 24, ID_ADD_IGNORE);
    CButton(L"删除选中", x0 + 386, y, 90, 24, ID_DEL_IGNORE);
}

static void BuildForm(void) {
    ResetCtls();
    if (g_page < 5) BuildLayoutPage(g_page);
    else if (g_page == PAGE_GLOBAL) BuildGlobalPage();
    else BuildIgnorePage();
    ScaleControls();
    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* ---------- 提交 ---------- */

static void CommitForm(void) {
    if (g_page < 5) {
        for (int i = 0; i < 3; i++) {
            ImeState s = kPages[g_page].states[i];
            if ((int)s < 0) break;
            StateStyle* raw = &s_cfg.st[s];
            for (int k = 0; k < g_ctlCount; k++) {
                Ctl* c = &g_ctls[k];
                if (c->state != (int)s) continue;
                switch (c->field) {
                case F_ENABLED:
                    raw->enabled = (SendMessageW(c->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    raw->have |= SF_ENABLED;
                    break;
                case F_INHERIT: {
                    int on = (SendMessageW(c->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    raw->inherit = on ? CapsBase(s) : -1;
                    break;
                }
                case F_SHAPE: {
                    int sel = (int)SendMessageW(c->hwnd, CB_GETCURSEL, 0, 0);
                    raw->shape = (sel == 0) ? SHAPE_NONE :
                                 (sel == 2) ? SHAPE_TRIANGLE : SHAPE_CIRCLE;
                    raw->have |= SF_SHAPE;
                    break;
                }
                case F_SIZE:  raw->size    = (int)SendMessageW(c->hwnd, TBM_GETPOS, 0, 0); raw->have |= SF_SIZE; break;
                case F_OFFX:  raw->offsetX = (int)SendMessageW(c->hwnd, TBM_GETPOS, 0, 0); raw->have |= SF_OFFSETX; break;
                case F_OFFY:  raw->offsetY = (int)SendMessageW(c->hwnd, TBM_GETPOS, 0, 0); raw->have |= SF_OFFSETY; break;
                case F_ALPHA: raw->alpha   = (int)SendMessageW(c->hwnd, TBM_GETPOS, 0, 0); raw->have |= SF_ALPHA; break;
                case F_TRANSIENT:
                    raw->transient = (SendMessageW(c->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    raw->have |= SF_TRANSIENT;
                    break;
                default: break;
                }
            }
        }
    } else if (g_page == PAGE_GLOBAL) {
        for (int k = 0; k < g_ctlCount; k++) {
            Ctl* c = &g_ctls[k];
            if (c->state != -1) continue;
            switch (c->field) {
            case F_POLL:     s_cfg.pollMs = (int)SendMessageW(c->hwnd, TBM_GETPOS, 0, 0); break;
            case F_TRACK:    s_cfg.trackMs = (int)SendMessageW(c->hwnd, TBM_GETPOS, 0, 0); break;
            case F_TIMEOUT:  s_cfg.caretTimeoutMs = (int)SendMessageW(c->hwnd, TBM_GETPOS, 0, 0); break;
            case F_AUTOHIDE: s_cfg.autoHideMs = (int)SendMessageW(c->hwnd, TBM_GETPOS, 0, 0); break;
            case F_STRATEGY: s_cfg.imeStrategy = (int)SendMessageW(c->hwnd, CB_GETCURSEL, 0, 0); break;
            case F_FULLSCREEN:  s_cfg.hideFullscreen = (SendMessageW(c->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
            case F_COMPOSITION: s_cfg.hideComposition = (SendMessageW(c->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
            case F_WPSCOM:      s_cfg.wpsCom = (SendMessageW(c->hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED); break;
            default: break;
            }
        }
    }
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
        MessageBoxW(g_hwnd, L"已保存，但找不到 IMEIndicator.exe。\n\n"
                            L"请手动启动主程序。",
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

static void SwitchPage(int page) {
    if (page < 0 || page >= PAGE_COUNT || page == g_page) return;
    CommitForm();
    g_page = page;
    if (page < 5) g_selState = (int)kPages[page].states[0];
    BuildForm();
    InvalidatePreview();
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

/* ---------- 消息处理 ---------- */

static void OnCommand(int id, int code, HWND ctl) {
    if (id == ID_SAVE)         { DoSave(0); return; }
    if (id == ID_SAVE_RESTART) { DoSave(1); return; }
    if (id == ID_CLOSE)        { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }

    if (id == ID_ADD_IGNORE && g_ignoreEdit) {
        WCHAR buf[CFG_NAME_MAX];
        GetWindowTextW(g_ignoreEdit, buf, CFG_NAME_MAX);
        WCHAR* p = buf;
        while (*p == L' ') p++;
        size_t len = wcslen(p);
        while (len > 0 && p[len-1] == L' ') p[--len] = 0;
        if (!p[0] || s_cfg.ignoreCount >= CFG_MAX_IGNORE) return;
        lstrcpynW(s_cfg.ignore[s_cfg.ignoreCount], p, CFG_NAME_MAX);
        s_cfg.ignoreCount++;
        SendMessageW(g_ignoreList, LB_ADDSTRING, 0, (LPARAM)p);
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

    Ctl* c = FindCtl(ctl);
    if (!c) return;

    /* 颜色按钮 */
    if (c->field == F_COLOR && c->state >= 0) {
        StateStyle* raw = &s_cfg.st[c->state];
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

    /* 跟随开关 */
    if (c->field == F_INHERIT && c->state >= 0) {
        int on = (SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED);
        StateStyle* raw = &s_cfg.st[c->state];
        if (on) {
            raw->inherit = CapsBase((ImeState)c->state);
        } else {
            /* 取消跟随时把基础状态当前的值拷进来，视觉上不跳变 */
            StateStyle eff;
            CfgStateStyle(&s_cfg, (ImeState)c->state, &eff);
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
        BuildForm();
        InvalidatePreview();
        return;
    }

    if (c->field == F_SHAPE && c->state >= 0) {
        int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        s_cfg.st[c->state].shape = (sel == 0) ? SHAPE_NONE :
                                   (sel == 2) ? SHAPE_TRIANGLE : SHAPE_CIRCLE;
        s_cfg.st[c->state].have |= SF_SHAPE;
        g_dirty = 1;
        InvalidatePreview();
        return;
    }
    if (c->field == F_ENABLED && c->state >= 0) {
        s_cfg.st[c->state].enabled = (SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED);
        s_cfg.st[c->state].have |= SF_ENABLED;
        g_dirty = 1;
        InvalidatePreview();
        return;
    }
    if (c->field == F_TRANSIENT && c->state >= 0) {
        s_cfg.st[c->state].transient = (SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED);
        s_cfg.st[c->state].have |= SF_TRANSIENT;
        g_dirty = 1;
        return;
    }
    (void)code;
}

static void OnHScroll(HWND slider) {
    Ctl* c = FindCtl(slider);
    if (!c) return;
    int v = (int)SendMessageW(slider, TBM_GETPOS, 0, 0);

    if (c->label) {
        WCHAR vb[32];
        int isMs = (c->field == F_POLL || c->field == F_TRACK ||
                    c->field == F_TIMEOUT || c->field == F_AUTOHIDE);
        _snwprintf_s(vb, 32, _TRUNCATE, isMs ? L"%d ms" : L"%d", v);
        SetWindowTextW(c->label, vb);
    }

    if (c->state >= 0) {
        StateStyle* raw = &s_cfg.st[c->state];
        switch (c->field) {
        case F_SIZE:  raw->size = v;    raw->have |= SF_SIZE;     break;
        case F_OFFX:  raw->offsetX = v; raw->have |= SF_OFFSETX;  break;
        case F_OFFY:  raw->offsetY = v; raw->have |= SF_OFFSETY;  break;
        case F_ALPHA: raw->alpha = v;   raw->have |= SF_ALPHA;    break;
        default: break;
        }
        InvalidatePreview();
    }
    g_dirty = 1;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        g_dpi = (int)g_startupDpi;   /* 与窗口尺寸同源，绝不在窗口 DC 上另取 */
        if (g_dpi <= 0) g_dpi = 96;
        g_bgBrush = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3));
        g_whiteBrush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
        g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"IMEIndicatorPreview", NULL,
            WS_CHILD | WS_VISIBLE, 0, 0, 100, 100, h, NULL, NULL, NULL);
        CButtonFixed(L"保存", ID_SAVE);
        CButtonFixed(L"保存并重启", ID_SAVE_RESTART);
        CButtonFixed(L"关闭", ID_CLOSE);
        g_page = 0;
        g_selState = (int)kPages[0].states[0];
        BuildForm();
        return 0;

    case WM_SIZE: {
        RECT rc;
        GetClientRect(h, &rc);
        MoveWindow(g_preview, S(PREVIEW_X), S(PREVIEW_Y),
                   rc.right - S(PREVIEW_X * 2), S(PREVIEW_H), TRUE);
        int by = rc.bottom - S(BOTTOM_H);
        HWND b1 = GetDlgItem(h, ID_SAVE);
        HWND b2 = GetDlgItem(h, ID_SAVE_RESTART);
        HWND b3 = GetDlgItem(h, ID_CLOSE);
        /* 用 HWND_TOP 放到 z 序最前：表单控件是后建的，否则会盖住按钮 */
        if (b1) SetWindowPos(b1, HWND_TOP, S(14), by, S(80), S(26), SWP_NOACTIVATE);
        if (b2) SetWindowPos(b2, HWND_TOP, S(14 + 88), by, S(110), S(26), SWP_NOACTIVATE);
        if (b3) SetWindowPos(b3, HWND_TOP, rc.right - S(14 + 80), by, S(80), S(26), SWP_NOACTIVATE);
        return 0;
    }

    case WM_COMMAND:
        OnCommand(LOWORD(w), HIWORD(w), (HWND)l);
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
            Ctl* c = FindCtl(di->hwndItem);
            DWORD rgb = 0xFF0000;
            if (c && c->state >= 0) {
                /* 必须画**解析后**的颜色：继承中的大写态本来就不该显示它自己
                   raw 里那份过时的值，否则和预览色对不上。 */
                StateStyle eff;
                CfgStateStyle(&s_cfg, (ImeState)c->state, &eff);
                rgb = eff.rgb;
            }
            HBRUSH b = CreateSolidBrush(RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
            FillRect(di->hDC, &di->rcItem, b);
            DeleteObject(b);
            FrameRect(di->hDC, &di->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH));
            return TRUE;
        }
        return FALSE;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* 表单区是白底、页签区是灰底；标签一律给白底，否则白底上会浮出
           一块块灰色补丁。页签是自己画的，不走这里。 */
        HDC dc = (HDC)w;
        SetBkColor(dc, RGB(0xFF, 0xFF, 0xFF));
        return (LRESULT)g_whiteBrush;
    }

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, g_bgBrush);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        /* 页签区灰底 */
        RECT head = { 0, 0, rc.right, S(CONTENT_Y - 6) };
        FillRect(dc, &head, g_bgBrush);
        /* 表单区白底 */
        RECT body = { 0, S(CONTENT_Y - 6), rc.right, rc.bottom - S(BOTTOM_H) + S(6) };
        FillRect(dc, &body, g_whiteBrush);
        /* 底部按钮区灰底 */
        RECT foot = { 0, rc.bottom - S(BOTTOM_H) + S(6), rc.right, rc.bottom };
        FillRect(dc, &foot, g_bgBrush);
        PaintTabs(dc);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_CLOSE:
        if (g_dirty) {
            int r = MessageBoxW(h, L"有未保存的修改，要保存吗？",
                                L"IMEIndicator 设置",
                                MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) DoSave(0);
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        ResetCtls();
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        if (g_bgBrush) { DeleteObject(g_bgBrush); g_bgBrush = NULL; }
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

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;

    SetDpiAware();

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    CfgLoad(&s_cfg);

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

    /* 窗口尺寸按 DPI 放大：WIN_W/WIN_H 是逻辑尺寸。
       ★ DPI 取一次存进 g_startupDpi，WM_CREATE 里直接沿用 —— 两处各取各的
         会让控件缩放和窗口尺寸对不上。
       居中并夹到工作区内 —— 175% + 1080p 时整窗接近满高，用 CW_USEDEFAULT
       会被级联到屏幕外，底部按钮点不到。 */
    g_startupDpi = QueryDpi(NULL);
    if (g_startupDpi == 0) g_startupDpi = 96;
    int dpi = (int)g_startupDpi;

    /* ★ 按**客户区**反推窗口尺寸：WIN_W/WIN_H 描述的是内容区，标题栏和边框
       会额外吃掉三十来个逻辑像素 —— 直接把它当整窗尺寸用，最底下的状态块
       就会被底部按钮压住。 */
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

    HWND h = CreateWindowExW(0, cls, L"IMEIndicator 设置",
                             style,
                             wx, wy, ww, wh,
                             NULL, NULL, hInst, NULL);
    if (!h) return 1;

    ShowWindow(h, SW_SHOWNORMAL);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
