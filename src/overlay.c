#include "ime_indicator.h"
#include "shape.h"
#include "image.h"

/* ================= 悬浮提示窗口 =================
   WS_EX_LAYERED + WS_EX_TRANSPARENT + WS_EX_TOPMOST + WS_EX_NOACTIVATE：
   不抢焦点、不拦鼠标、始终最顶。用 32 位 DIB Section + UpdateLayeredWindow
   （ULW_ALPHA）画形状，alpha 为预乘，无 GDI+ 依赖。

   追踪间隔只有 15ms，所以：
   - DIB/内存 DC 按尺寸缓存，不再每帧 Create/Delete；
   - 样式与位置都没变时直接跳过重绘（只在真正变化时才 UpdateLayeredWindow）。

   ★ 尺寸/偏移是**逻辑像素**（96 DPI 基准），按**光标所在显示器**的 DPI
     换算成物理像素。旧版直接用物理像素，150% 屏上 20px 只有 13 逻辑像素，
     看起来偏小。
   ★ DPI 在 OverlayMove 里按光标坐标算（不是鼠标坐标）：多显示器混合 DPI
     时，圆点要跟它实际落在的那块屏走。 */

static HWND    g_hwnd = NULL;

/* 当前样式（逻辑值）与由它换算出的物理值 */
static StateStyle g_style;
static int     g_haveStyle = 0;

static int     g_pxW = 20, g_pxH = 20;
static int     g_pxTarget = 20;   /* 图片形状的「长边」目标像素（g_pxW 是短边时不能拿它当目标） */
static int     g_pxOffX = -2;
static int     g_pxOffY = 4;
static int     g_shape = SHAPE_CIRCLE;
static BYTE    g_cr = 0xFF, g_cg = 0x00, g_cb = 0x00;
static BYTE    g_alpha = 255;
static UINT    g_lastDpi = 0;

static HDC     g_mem = NULL;
static HBITMAP g_bmp = NULL;
static HGDIOBJ g_oldBmp = NULL;
static void*   g_bits = NULL;
static int     g_bmpW = 0, g_bmpH = 0;
static int     g_bmpDirty = 1;   /* 配色/尺寸/形状变了 -> 重算像素 */
static int     g_needPaint = 1;  /* 必须再 UpdateLayeredWindow 一次（如重新显示） */
static int     g_lastX = 0, g_lastY = 0;
static int     g_havePos = 0;

/* OverlayMove 本帧查到的图片视图。PaintDot 直接复用，避免每帧查两次表。
   样式/尺寸变化时由 OverlayApply / OverlayMove 重新赋值或失效。 */
static ImgView g_imgView;
static int     g_haveImgView = 0;

static LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) return 0;
    return DefWindowProcW(h, m, w, l);
}

void OverlayInit(void) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    static const WCHAR cls[] = L"IMEIndicatorOverlay";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hi;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        cls, L"IMEIndicator", WS_POPUP,
        -100, -100, g_pxW, g_pxH, NULL, NULL, hi, NULL);
    if (g_hwnd) {
        /* 初始为全透明占位，避免闪出白块 */
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 0, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_hwnd, NULL, NULL, NULL, NULL, NULL, 0, &bf, ULW_ALPHA);
    }
}

/* 释放位图与内存 DC */
static void FreeSurface(void) {
    if (g_mem && g_oldBmp) SelectObject(g_mem, g_oldBmp);
    g_oldBmp = NULL;
    if (g_mem) { DeleteDC(g_mem); g_mem = NULL; }
    if (g_bmp) { DeleteObject(g_bmp); g_bmp = NULL; }
    g_bits = NULL;
    g_bmpW = g_bmpH = 0;
    g_bmpDirty = 1;
}

void OverlayShutdown(void) {
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
    FreeSurface();
}

void OverlaySetVisible(int visible) {
    if (!g_hwnd) return;
    ShowWindow(g_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    g_needPaint = 1;             /* 重新显示后要再贴一次内容 */
}

/* 逻辑像素 -> 物理像素。dpi=0 时按 96 处理。四舍五入，
   避免 150% 下 20 被截断成 29。
   ★ 负数要按「远离零」四舍五入：C 的整数除法向零截断，
     (-2*144+48)/96 = -2，而正确值是 -3（偏移量可以是负的）。 */
static int ScaleByDpi(int logical, UINT dpi) {
    if (dpi == 0) dpi = 96;
    if (logical >= 0)
        return (int)(((long long)logical * (long long)dpi + 48) / 96);
    return -(int)(((long long)(-logical) * (long long)dpi + 48) / 96);
}

static UINT DpiAtPoint(int x, int y) {
    POINT pt = { x, y };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!mon) return 96;
    typedef HRESULT (WINAPI* PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
    static PFN_GetDpiForMonitor pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE sh = LoadLibraryW(L"shcore.dll");
        if (sh) pfn = (PFN_GetDpiForMonitor)GetProcAddress(sh, "GetDpiForMonitor");
    }
    if (pfn) {
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(pfn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx) return dx;
    }
    return 96;
}

void OverlayApply(const StateStyle* st) {
    if (!st) return;
    if (g_haveStyle && memcmp(&g_style, st, sizeof(StateStyle)) == 0) {
        /* 样式没变：不重绘（追踪循环每 15ms 调一次） */
        return;
    }
    g_style = *st;
    g_haveStyle = 1;
    g_bmpDirty = 1;
    g_haveImgView = 0;   /* 样式变了（可能换了图/尺寸），缓存的图片视图作废 */
}

/* 按当前尺寸准备好 DIB Section（尺寸变了才重建）。
   图片形状不是正方形，所以宽高分开记。 */
static int EnsureSurface(int w, int h) {
    if (g_mem && g_bmp && g_bits && g_bmpW == w && g_bmpH == h) return 1;
    FreeSurface();
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    HDC scr = GetDC(NULL);
    if (!scr) return 0;
    g_mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;             /* 自上而下 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    if (g_mem)
        g_bmp = CreateDIBSection(g_mem, &bmi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    ReleaseDC(NULL, scr);
    if (!g_mem || !g_bmp || !g_bits) { FreeSurface(); return 0; }
    g_oldBmp = SelectObject(g_mem, g_bmp);
    g_bmpW = w;
    g_bmpH = h;
    g_bmpDirty = 1;
    return 1;
}

/* 画当前形状进缓存的 DIB。内置形状由 shape.c 提供，图片由 image.c 提供
   （与设置程序预览共用同一份实现，保证"预览看到的 = 实际看到的"）。 */
static void PaintDot(void) {
    if (g_shape == SHAPE_IMAGE) {
        /* 直接用 OverlayMove 本帧查到的视图，不再重复 ImgGet（每帧只查一次表）。 */
        if (g_haveImgView && g_imgView.w == g_bmpW && g_imgView.h == g_bmpH) {
            ImgBlit((BYTE*)g_bits, &g_imgView, g_alpha);
            g_bmpDirty = 0;
            return;
        }
        /* 视图失效（缓存被挤掉/换了图）：整块 DIB 清零后返回。
           ★ 不能拿 g_bmpW 当 size 调 ShapeRasterize：它按 size*size*4 写，
           图片非正方形时 DIB 只有 g_bmpW×g_bmpH，会越界写堆。
           下一帧 OverlayMove 会发现 ImgGet 失败，把形状回退成圆点并重建
           正方形 surface，指示器不会整个消失。 */
        ZeroMemory(g_bits, (size_t)g_bmpW * g_bmpH * 4);
        g_bmpDirty = 0;
        return;
    }
    ShapeRasterize((BYTE*)g_bits, g_bmpW, g_style.rgb, g_alpha, g_shape);
    g_bmpDirty = 0;
}

/* sx, baselineY：光标左上角屏幕坐标与光标底缘；窗口落在
   (sx+offsetX, baselineY+offsetY)，图形居中于窗口。 */
void OverlayMove(int screenX, int baselineY) {
    if (!g_hwnd || !g_haveStyle) return;

    /* 按光标所在显示器换算物理像素（多显示器混合 DPI） */
    UINT dpi = DpiAtPoint(screenX, baselineY + 1);
    int pxSize = ScaleByDpi(g_style.size, dpi);
    if (pxSize < 1) pxSize = 1;
    if (pxSize > 512) pxSize = 512;
    int pxOffX = ScaleByDpi(g_style.offsetX, dpi);
    int pxOffY = ScaleByDpi(g_style.offsetY, dpi);

    int shape = g_style.shape;
    int pxW = pxSize, pxH = pxSize;
    g_haveImgView = 0;
    if (shape == SHAPE_IMAGE) {
        ImgView v;
        if (ImgGet(g_style.image, pxSize, &v, NULL, 0)) {
            pxW = v.w;
            pxH = v.h;
            g_imgView = v;          /* 存给 PaintDot 复用，省掉每帧第二次 ImgGet */
            g_haveImgView = 1;
        } else {
            shape = SHAPE_CIRCLE;   /* 图片缺失/解码失败：回退圆点 */
        }
    }

    BYTE nr = (BYTE)(g_style.rgb >> 16);
    BYTE ng = (BYTE)(g_style.rgb >> 8);
    BYTE nb = (BYTE)(g_style.rgb);
    BYTE na = (BYTE)(g_style.alpha & 0xFF);

    if (pxW != g_pxW || pxH != g_pxH || pxSize != g_pxTarget || shape != g_shape ||
        nr != g_cr || ng != g_cg || nb != g_cb || na != g_alpha ||
        dpi != g_lastDpi) {
        g_bmpDirty = 1;
    }
    g_pxW = pxW;
    g_pxH = pxH;
    g_pxTarget = pxSize;
    g_pxOffX = pxOffX;
    g_pxOffY = pxOffY;
    g_shape = shape;
    g_cr = nr; g_cg = ng; g_cb = nb; g_alpha = na;
    g_lastDpi = dpi;

    /* 位置、样式都没变 -> 不重绘（追踪循环每 15ms 调一次，绝大多数是空转） */
    if (!g_bmpDirty && !g_needPaint && g_havePos &&
        screenX == g_lastX && baselineY == g_lastY) return;
    if (!EnsureSurface(pxW, pxH)) return;
    if (g_bmpDirty) PaintDot();

    HDC scr = GetDC(NULL);
    if (!scr) return;
    POINT dst = { screenX + g_pxOffX, baselineY + g_pxOffY };
    POINT src = { 0, 0 };
    SIZE  szw = { g_bmpW, g_bmpH };
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    /* 贴图失败（句柄失效/桌面切换等）时不能更新位置与重绘标志，
       否则这一帧永远不再重试，圆点就卡在旧位置或干脆不显示。
       把 g_bmpDirty 置回 1：若 g_needPaint 本来就是 0 且位置没变，
       下一帧会被「什么都没变」的快速路径挡掉，必须靠它强制重试。 */
    if (!UpdateLayeredWindow(g_hwnd, scr, &dst, &szw, g_mem, &src, 0, &bf, ULW_ALPHA)) {
        g_bmpDirty = 1;
        ReleaseDC(NULL, scr);
        return;
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ReleaseDC(NULL, scr);

    g_lastX = screenX; g_lastY = baselineY; g_havePos = 1;
    g_needPaint = 0;
}
