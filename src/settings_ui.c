#include "settings_internal.h"

int DpiScale(int v) {
    if (g_dpi <= 0) g_dpi = 96;
    return MulDiv(v, g_dpi, 96);
}

/* 客户区尺寸 -> 整窗尺寸（含标题栏/边框）。按传入 DPI 算：AdjustWindowRectEx
   用的是系统 DPI，150%/175% 屏上会把最小尺寸算小。 */
void ClientToWindowSize(RECT* rc, DWORD style, DWORD exstyle, UINT dpi) {
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

HFONT g_fontBold = NULL;

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
    /* 失败就返回 NULL：系统字体不归我们所有，混进 g_font 会在 WM_DESTROY 被误删 */
    return CreateFontIndirectW(&lf);
}

HFONT BaseFont(void) {
    if (!g_font) g_font = MakeFont(0);
    /* 建字体失败时借用系统字体，只用于绘制，不写回 g_font */
    return g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

HFONT BoldFont(void) {
    if (!g_fontBold) g_fontBold = MakeFont(1);
    return g_fontBold ? g_fontBold : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
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
void PaintRoundRectAA(HDC dc, const RECT* rc, int radius,
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

int BufBegin(BufDC* b, HDC ref, int w, int h) {
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

void BufEnd(BufDC* b, HDC dc) {
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

/* ---------- 悬停说明气泡 ----------
   自绘控件用不上普通工具提示的那套：comctl32 的 TTF_SUBCLASS 挂在 ownerdraw
   按钮上不会弹出。这里自己开一个小弹出窗：鼠标停住 400ms 后贴在控件下方，
   离开或按下就收。文字用静态字面量，窗口只存指针。 */
static HWND g_hint = NULL;

void HintHide(void) {
    if (g_hint) {
        DestroyWindow(g_hint);
        g_hint = NULL;
    }
}

LRESULT CALLBACK HintProc(HWND h, UINT m, WPARAM w, LPARAM l) {
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
            InflateRect(&tr, -DpiScale(10), -DpiScale(6));
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
    int maxW = DpiScale(300);
    RECT calc = { 0, 0, maxW - DpiScale(16), 0 };
    HDC dc = GetDC(NULL);
    HGDIOBJ of = SelectObject(dc, BaseFont());
    DrawTextW(dc, text, -1, &calc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);
    SelectObject(dc, of);
    ReleaseDC(NULL, dc);

    int w = calc.right + DpiScale(10) * 2;
    int ht = calc.bottom + DpiScale(6) * 2;
    int x = wr.left;
    int y = wr.bottom + DpiScale(4);

    /* 贴边时收回屏幕内；下方不够高就翻到控件上方 */
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(mon, &mi)) {
        if (y + ht > mi.rcWork.bottom) y = wr.top - DpiScale(4) - ht;
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

void MakePushButton(HWND h) {
    SetWindowSubclass(h, BtnSub, 1, 0);
}

/* 先铺父窗口底色，圆角外的四个角才像"透明"的 */
void PaintPushButton(const DRAWITEMSTRUCT* di) {
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
                       : hover ? RGB(0x1A, 0x86, 0xD9) : ACCENT_RGB;
        border = fill;
        text = RGB(0xFF, 0xFF, 0xFF);
    } else {
        fill = pressed ? RGB(0xCC, 0xE4, 0xF7)
                       : hover ? RGB(0xE5, 0xF1, 0xFB) : RGB(0xFC, 0xFC, 0xFC);
        border = pressed ? RGB(0x00, 0x54, 0x99)
                         : hover ? ACCENT_RGB : RGB(0xAD, 0xAD, 0xAD);
        text = RGB(0x00, 0x00, 0x00);
    }

    int rad = DpiScale(4);
    PaintRoundRectAA(di->hDC, &rc, rad, border, fill);

    WCHAR txt[96];
    GetWindowTextW(h, txt, 96);
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (!f) f = BaseFont();
    HGDIOBJ of = SelectObject(di->hDC, f);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, text);
    RECT tr = rc;
    if (pressed && !primary) OffsetRect(&tr, 0, DpiScale(1));
    DrawTextW(di->hDC, txt, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(di->hDC, of);
}

/* 自绘复选框：16px 圆角方块 + 白色对勾，悬停描边变蓝。
   BS_OWNERDRAW 不会自动切换勾选状态，点击时由 WM_COMMAND 里手动翻转。 */
void PaintCheck(const DRAWITEMSTRUCT* di) {
    HWND h = di->hwndItem;
    RECT rc = di->rcItem;
    BOOL checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    BOOL hover = GetPropW(h, BTN_HOVER_PROP) != NULL;
    BOOL disabled = (di->itemState & ODS_DISABLED) != 0;

    HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(di->hDC, &rc, bg);
    DeleteObject(bg);

    int box = DpiScale(16);
    int by = rc.top + (rc.bottom - rc.top - box) / 2;
    RECT br = { rc.left, by, rc.left + box, by + box };

    COLORREF fill, border;
    if (disabled) {
        fill = RGB(0xF6, 0xF6, 0xF6);
        border = RGB(0xBD, 0xBD, 0xBD);
    } else if (checked) {
        fill = ACCENT_RGB;
        border = fill;
    } else {
        fill = RGB(0xFF, 0xFF, 0xFF);
        border = hover ? ACCENT_RGB : RGB(0xAD, 0xAD, 0xAD);
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
            double hw = DpiScale(2) / 2.0;         /* 96 DPI 下笔画宽 2px */
            DWORD white = RgbToPix(RGB(0xFF, 0xFF, 0xFF));
            UIPixCapsule(lay.px, lw, lh, lx + DpiScale(4), ly + DpiScale(8),
                         lx + DpiScale(7), ly + DpiScale(11), hw, white, 255);
            UIPixCapsule(lay.px, lw, lh, lx + DpiScale(7), ly + DpiScale(11),
                         lx + DpiScale(12), ly + DpiScale(5), hw, white, 255);
        }
        UILayerBlend(&lay, di->hDC, 0, 0);
        UILayerEnd(&lay);
    } else {
        HBRUSH b = CreateSolidBrush(fill);
        HPEN p = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ ob = SelectObject(di->hDC, b);
        HGDIOBJ op = SelectObject(di->hDC, p);
        RoundRect(di->hDC, br.left, br.top, br.right, br.bottom, DpiScale(6), DpiScale(6));
        SelectObject(di->hDC, ob);
        SelectObject(di->hDC, op);
        DeleteObject(b);
        DeleteObject(p);

        if (checked) {
            HPEN cp = CreatePen(PS_SOLID, DpiScale(2), RGB(0xFF, 0xFF, 0xFF));
            HGDIOBJ oc = SelectObject(di->hDC, cp);
            POINT pts[3] = {
                { br.left + DpiScale(4),  br.top + DpiScale(8) },
                { br.left + DpiScale(7),  br.top + DpiScale(11) },
                { br.left + DpiScale(12), br.top + DpiScale(5) },
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
    tr.left += box + DpiScale(6);
    DrawTextW(di->hDC, txt, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(di->hDC, of);
}

/* ---------- 控件元数据（建一次，用于按 hwnd 反查字段 / 统一缩放） ---------- */

Meta g_meta[200];
int  g_metaCount = 0;

void RegMeta(HWND h, int block, int field, HWND numEdit, int x, int y, int w, int hh) {
    if (!h || g_metaCount >= (int)(sizeof(g_meta) / sizeof(g_meta[0]))) return;
    Meta* m = &g_meta[g_metaCount++];
    m->hwnd = h; m->block = block; m->field = field; m->numEdit = numEdit;
    m->x = x; m->y = y; m->w = w; m->h = hh;
}

Meta* FindMeta(HWND h) {
    for (int i = 0; i < g_metaCount; i++) if (g_meta[i].hwnd == h) return &g_meta[i];
    return NULL;
}

/* 数值框与滑块双向同步：程序改文本时置位，避免 EN_CHANGE 反过来又写一遍 */
int g_syncEdit = 0;

int SliderPos(HWND s) { return (int)SendMessageW(s, TBM_GETPOS, 0, 0); }

/* 设位置并立刻重画。redraw 一律传 FALSE：让 trackbar 自己失效一次会先擦一遍
   系统灰底再等我们铺白，拖动时那一下就闪；这里只标记脏区，我们自己画。 */
void SetSliderPos(HWND sl, int v) {
    if (!sl) return;
    SendMessageW(sl, TBM_SETPOS, FALSE, (LPARAM)v);
    InvalidateRect(sl, NULL, FALSE);
}

void SetNumText(HWND e, int v) {
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

void ScaleControls(void) {
    for (int i = 0; i < g_metaCount; i++) {
        Meta* m = &g_meta[i];
        if (!m->hwnd || !IsWindow(m->hwnd)) continue;
        SetWindowPos(m->hwnd, NULL, DpiScale(m->x), DpiScale(m->y), DpiScale(m->w), DpiScale(m->h),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* ---------- 面板 ---------- */
/* 图片文件名标签：文件缺失/解码失败时标红（WM_CTLCOLORSTATIC 里用）。
   放在这里是因为面板过程要读，而 Block 结构在后面才定义。 */
HWND g_imgLabel[3];
int  g_imgBad[3];

/* 「颜色」标签：形状=图片时图片不染色，颜色项整行置灰（STATIC 的
   EnableWindow 不改字色，所以要在 WM_CTLCOLORSTATIC 里自己换） */
HWND g_colorLabel[3];
int  g_colorDim[3];

/* 四个数值项的标签：跟随 cn_cn 时置灰（STATIC 的 EnableWindow 不改字色，
   所以要在 WM_CTLCOLORSTATIC 里自己换） */
HWND g_customLabel[3][4];
int  g_customDim[3][4];

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

void RegisterPanelClass(HINSTANCE hi) {
    static const WCHAR cls[] = L"IMEIndicatorPanel";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = hi;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
}

HWND MakePanel(void) {
    return CreateWindowExW(0, L"IMEIndicatorPanel", NULL,
                           WS_CHILD | WS_CLIPCHILDREN, 0, 0, 100, 100,
                           g_hwnd, NULL, NULL, NULL);
}

/* ---------- 控件工厂 ---------- */
HWND g_parent;   /* 当前创建控件用的父面板 */

HWND CLabel(const WCHAR* t, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             x, y, w, h, g_parent, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(c, -2, 0, NULL, x, y, w, h);
    return c;
}

HWND CCheck(const WCHAR* t, int x, int y, int w, int h,
                   int block, int field, int checked) {
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             x, y, w, h, g_parent, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    /* 先把属性与子类装上，BM_SETCHECK 才拦得到（拦截逻辑见 BtnSub） */
    SetPropW(c, CHK_PROP, (HANDLE)1);
    MakePushButton(c);          /* 复用悬停跟踪 */
    SendMessageW(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    RegMeta(c, block, field, NULL, x, y, w, h);
    return c;
}

HWND CButton(const WCHAR* t, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             x, y, w, h, g_parent, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    MakePushButton(c);
    RegMeta(c, -2, 0, NULL, x, y, w, h);
    return c;
}

HWND CEdit(int x, int y, int w, int h) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             x, y, w, h, g_parent, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(c, -2, 0, NULL, x, y, w, h);
    return c;
}

/* 数值框：直接输入，和滑块双向同步（滑块手抖时靠它精确调）。
   不用 ES_NUMBER：偏移量允许负数，而 ES_NUMBER 连负号一起挡掉；
   非法字符由失焦时的回填纠正。 */
HWND CEditNum(int x, int y, int w, int h, int block, int field) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                             WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL,
                             x, y, w, h, g_parent, NULL, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(c, block, field, NULL, x, y, w, h);
    return c;
}

/* ---------- 自绘下拉（替代系统 COMBOBOX） ----------
   系统下拉的 3D 边框与方形箭头改不掉，和圆角按钮/下划线页签放一起很割裂。
   这里自绘：闭合态是圆角框 + 当前项 + 右侧箭头，点开弹出一份圆角列表
   （悬停高亮、当前项蓝色文字）。
   对外仍认 CB_ADDSTRING / CB_SETCURSEL / CB_GETCURSEL，调用处不用改。 */

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
                    : hover ? ACCENT_RGB : RGB(0xAD, 0xAD, 0xAD);
    RECT brc = { rc.left, rc.top, rc.right, rc.bottom };
    PaintRoundRectAA(dc, &brc, DpiScale(3), border, RGB(0xFF, 0xFF, 0xFF));

    int ax = rc.right - DpiScale(16), ay = (rc.top + rc.bottom) / 2;
    COLORREF ac = enabled ? RGB(0x54, 0x54, 0x54) : RGB(0xB0, 0xB0, 0xB0);
    POINT a1 = { ax - DpiScale(4), ay - DpiScale(2) };
    POINT a2 = { ax, ay + DpiScale(2) };
    POINT a3 = { ax + DpiScale(4), ay - DpiScale(2) };
    PaintLineAA(dc, a1, a2, DpiScale(2) / 2.0, ac);
    PaintLineAA(dc, a2, a3, DpiScale(2) / 2.0, ac);

    if (d && d->count > 0) {
        HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        if (!f) f = BaseFont();
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? RGB(0x00, 0x00, 0x00) : RGB(0xA0, 0xA0, 0xA0));
        RECT tr = rc;
        tr.left += DpiScale(10);
        tr.right -= DpiScale(26);
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
    RoundRect(dc, 0, 0, rc.right - 1, rc.bottom - 1, DpiScale(8), DpiScale(8));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);

    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (!f) f = BaseFont();
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    int ih = DpiScale(SEL_ITEM_H);
    for (int i = 0; d && i < d->count; i++) {
        RECT r = { DpiScale(4), DpiScale(4) + i * ih, rc.right - DpiScale(4), DpiScale(4) + (i + 1) * ih };
        if (i == d->hot) {
            HBRUSH hb = CreateSolidBrush(RGB(0xE5, 0xF1, 0xFB));
            FillRect(dc, &r, hb);
            DeleteObject(hb);
        }
        SetTextColor(dc, i == d->sel ? RGB(0x00, 0x5A, 0x9E) : RGB(0x00, 0x00, 0x00));
        RECT tr = r;
        tr.left += DpiScale(8);
        DrawTextW(dc, d->items[i], -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, of);
}

LRESULT CALLBACK SelPopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
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
        int ih = DpiScale(SEL_ITEM_H);
        int y = (short)HIWORD(l) - DpiScale(4);
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
        int ih = DpiScale(SEL_ITEM_H);
        int y = (short)HIWORD(l) - DpiScale(4);
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
    int ih = DpiScale(SEL_ITEM_H);
    int w = wr.right - wr.left;
    int h = d->count * ih + DpiScale(8);

    g_selPopup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, SEL_POPUP, NULL,
                                 WS_POPUP, wr.left, wr.bottom, w, h,
                                 owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_selPopup) return;
    g_selOwner = owner;
    d->hot = -1;
    SetWindowLongPtrW(g_selPopup, GWLP_USERDATA, (LONG_PTR)d);
    SendMessageW(g_selPopup, WM_SETFONT, (WPARAM)BaseFont(), TRUE);

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, DpiScale(8), DpiScale(8));
    SetWindowRgn(g_selPopup, rgn, TRUE);

    ShowWindow(g_selPopup, SW_SHOWNOACTIVATE);
    SetCapture(g_selPopup);
    InvalidateRect(g_selPopup, NULL, FALSE);
}

LRESULT CALLBACK SelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
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
    g->thumbR = DpiScale(14) / 2.0;
    g->trackH = DpiScale(4) > 0 ? (double)DpiScale(4) : 4.0;
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
    COLORREF kAccent = on ? ACCENT_RGB : RGB(0xBC, 0xBC, 0xBC);

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
    RECT track = { (int)g.xL, icy - DpiScale(2), (int)g.xR, icy + DpiScale(2) };
    HBRUSH gb = CreateSolidBrush(kTrack);
    HPEN gp = CreatePen(PS_SOLID, 1, kTrack);
    HGDIOBJ ob = SelectObject(dc, gb);
    HGDIOBJ op = SelectObject(dc, gp);
    RoundRect(dc, track.left, track.top, track.right, track.bottom, DpiScale(4), DpiScale(4));
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
        RoundRect(dc, track.left, track.top, icx, track.bottom, DpiScale(4), DpiScale(4));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(bb);
        DeleteObject(bp);
    }

    int d = DpiScale(14);
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
        g_dragGrab = (fabs(x - cx) <= g.thumbR + DpiScale(2)) ? x - cx : 0.0;
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

HWND CCombo(int x, int y, int w, int h, int block, int field) {
    (void)h;   /* 系统下拉的"展开高度"对自绘控件没有意义 */
    SelData* d = (SelData*)calloc(1, sizeof(SelData));
    HWND c = CreateWindowExW(0, SEL_CLASS, NULL, WS_CHILD | WS_VISIBLE,
                             x, y, w, 22, g_parent, NULL, NULL, NULL);
    /* 建窗失败就没有句柄可挂：先释放数据，别让 SetWindowLongPtrW(NULL) 白跑 */
    if (!c) { free(d); return NULL; }
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

HWND CColorBtn(int x, int y, int w, int h, int block) {
    HWND c = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                             x, y, w, h, g_parent, NULL, NULL, NULL);
    RegMeta(c, block, F_COLOR, NULL, x, y, w, h);
    return c;
}

HWND CSlider(int x, int y, int w, int h, int block, int field,
                    int lo, int hi, int val, HWND numEdit) {
    HWND c = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
                             WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                             x, y, w, h, g_parent, NULL, NULL, NULL);
    /* ★ 上下限分开设，不用 TBM_SETRANGE + MAKELPARAM：上限超过 32767 时
       MAKELPARAM 拼出来的 32 位值在符号扩展后会被控件当成负数，范围被截成
       lo..lo，滑块就永远卡在最小值。 */
    SendMessageW(c, TBM_SETRANGEMIN, FALSE, (LPARAM)lo);
    SendMessageW(c, TBM_SETRANGEMAX, FALSE, (LPARAM)hi);
    SendMessageW(c, TBM_SETPOS, FALSE, (LPARAM)val);
    SetWindowSubclass(c, SliderSub, 1, 0);
    RegMeta(c, block, field, numEdit, x, y, w, h);
    return c;
}
