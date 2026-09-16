#include "ime_indicator.h"

/* ================= 悬浮圆点窗口 =================
   WS_EX_LAYERED + WS_EX_TRANSPARENT + WS_EX_TOPMOST + WS_EX_NOACTIVATE：
   不抢焦点、不拦鼠标、始终最顶。用 32 位 DIB Section + UpdateLayeredWindow
   （ULW_ALPHA）画一个抗锯齿实心圆，alpha 为预乘，无 GDI+ 依赖。

   追踪间隔只有 15ms，所以：
   - DIB/内存 DC 按尺寸缓存，不再每帧 Create/Delete；
   - 位置与配色都没变时直接跳过重绘（只在真正变化时才 UpdateLayeredWindow）。 */

static HWND    g_hwnd = NULL;
static int     g_dotSize = 9, g_offX = 2, g_offY = 4;
static int     g_shape = SHAPE_CIRCLE;  /* SHAPE_CIRCLE / SHAPE_TRIANGLE */
static BYTE    g_cr = 0xFF, g_cg = 0x00, g_cb = 0x00; /* 当前颜色分量（默认英文红） */
static BYTE    g_alpha = 255;

static HDC     g_mem = NULL;
static HBITMAP g_bmp = NULL;
static HGDIOBJ g_oldBmp = NULL;
static void*   g_bits = NULL;
static int     g_bmpSize = 0;
static int     g_bmpDirty = 1;   /* 配色/尺寸变了 -> 重算像素 */
static int     g_needPaint = 1;  /* 必须再 UpdateLayeredWindow 一次（如重新显示） */
static int     g_lastX = 0, g_lastY = 0;
static int     g_havePos = 0;

static LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) return 0;
    return DefWindowProcW(h, m, w, l);
}

void OverlayInit(ImeCfg* c) {
    g_dotSize = c->size; g_offX = c->offsetX; g_offY = c->offsetY;
    g_shape = (c->shape == SHAPE_TRIANGLE) ? SHAPE_TRIANGLE : SHAPE_CIRCLE;
    g_alpha = (BYTE)c->dotAlpha;

    HINSTANCE hi = GetModuleHandleW(NULL);
    static const WCHAR cls[] = L"IMEIndicatorOverlay";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hi;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    int sz = g_dotSize;
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        cls, L"IMEIndicator", WS_POPUP,
        -100, -100, sz, sz, NULL, NULL, hi, NULL);
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
    g_bmpSize = 0;
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

void OverlaySetColor(DWORD rgb, DWORD alpha) {
    BYTE nr = (BYTE)(rgb >> 16), ng = (BYTE)(rgb >> 8), nb = (BYTE)rgb;
    BYTE na = (BYTE)(alpha & 0xFF);
    if (nr != g_cr || ng != g_cg || nb != g_cb || na != g_alpha) g_bmpDirty = 1;
    g_cr = nr; g_cg = ng; g_cb = nb;
    g_alpha = na;
}

/* 按当前尺寸准备好 DIB Section（尺寸变了才重建） */
static int EnsureSurface(void) {
    if (g_mem && g_bmp && g_bits && g_bmpSize == g_dotSize) return 1;
    FreeSurface();
    int sz = g_dotSize;
    if (sz < 1) sz = 1;
    HDC scr = GetDC(NULL);
    if (!scr) return 0;
    g_mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = sz;
    bmi.bmiHeader.biHeight = -sz;            /* 自上而下 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    if (g_mem)
        g_bmp = CreateDIBSection(g_mem, &bmi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    ReleaseDC(NULL, scr);
    if (!g_mem || !g_bmp || !g_bits) { FreeSurface(); return 0; }
    g_oldBmp = SelectObject(g_mem, g_bmp);
    g_bmpSize = sz;
    g_bmpDirty = 1;
    return 1;
}

/* 4x4 子采样抗锯齿画形状，写进缓存的 DIB。
   形状只影响"这个采样点算不算在图形内"，外围（DIB 缓存、预乘、
   位置未变跳过重绘）与形状无关 —— 加形状不用动那些。 */

/* 圆形：fx,fy 是相对图形中心的偏移，half 是内切半径 */
static int InsideCircle(double fx, double fy, double half) {
    return (fx * fx + fy * fy) <= half * half;
}

/* 等边三角形，尖角朝上。尺寸取"让三角形高度等于圆的直径"（= 2*half）：
   直接拿圆的 half 当外接半高会让三角形显得比圆小一圈（面积只有圆的 ~27%），
   按高度撑满后是 ~49%，两者摆一起才像同一套东西。
   平移量把包围盒居中 —— 等边三角形的重心不在外接圆心，不平移会整体偏上半个身位。
   判定用"点与三条有向边同侧"（叉积同号），比逐个三角形求交简单。 */
static int InsideTriangle(double fx, double fy, double half) {
    /* 半高 H 使三角形高 = 2*half → H = 2*half/1.5 = half*4/3 */
    const double H  = half * 1.3333333333;
    const double w  = H * 0.5773502692;   /* 半宽 = 半高 * √3/3 */
    /* 顶点（以画布中心的未平移坐标系）：上 (0,-H)、左下 (-w, H*0.5)、右下 (w, H*0.5)
       包围盒 y ∈ [-H, H*0.5]，中心在 -H*0.25 → 下移 H*0.25 使其在画布里居中 */
    const double dy = H * 0.25;
    const double ax = 0.0, ay = -H + dy;
    const double bx = -w,  by = H * 0.5 + dy;
    const double cx2 = w,  cy2 = H * 0.5 + dy;
    double d1 = (bx - ax) * (fy - ay) - (by - ay) * (fx - ax);
    double d2 = (cx2 - bx) * (fy - by) - (cy2 - by) * (fx - bx);
    double d3 = (ax - cx2) * (fy - cy2) - (ay - cy2) * (fx - cx2);
    int neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    int pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);                  /* 三个叉积同号（含 0）= 在内部 */
}

static void PaintDot(void) {
    int sz = g_bmpSize;
    /* 预乘：UpdateLayeredWindow + AC_SRC_ALPHA 需要预乘 alpha */
    int pr = (g_cr * g_alpha + 127) / 255;
    int pg = (g_cg * g_alpha + 127) / 255;
    int pb = (g_cb * g_alpha + 127) / 255;

    double cx = (sz - 1) / 2.0, cy = (sz - 1) / 2.0;
    double r = (sz / 2.0) - 0.5;         /* 图形中心到边的内切半径，留 1px 抗锯齿 */
    const int SS = 4;                     /* 4x4 子采样抗锯齿 */
    BYTE* p = (BYTE*)g_bits;
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            int inside = 0;
            for (int syy = 0; syy < SS; syy++)
                for (int sxx = 0; sxx < SS; sxx++) {
                    double fx = x + (sxx + 0.5) / SS - cx;
                    double fy = y + (syy + 0.5) / SS - cy;
                    if (g_shape == SHAPE_TRIANGLE) {
                        if (InsideTriangle(fx, fy, r)) inside++;
                    } else if (InsideCircle(fx, fy, r)) inside++;
                }
            int a = (g_alpha * inside + (SS * SS - 1)) / (SS * SS);
            p[0] = (BYTE)((pb * a + 127) / 255);   /* B 预乘 */
            p[1] = (BYTE)((pg * a + 127) / 255);
            p[2] = (BYTE)((pr * a + 127) / 255);
            p[3] = (BYTE)a;
            p += 4;
        }
    }
    g_bmpDirty = 0;
}

/* sx, baselineY：光标左上角屏幕坐标与光标底缘；窗口落在
   (sx+offsetX, baselineY+offsetY)，圆居中于窗口。 */
void OverlayMove(int screenX, int baselineY) {
    if (!g_hwnd) return;
    /* 位置、配色都没变 -> 不重绘（追踪循环每 15ms 调一次，绝大多数是空转） */
    if (!g_bmpDirty && !g_needPaint && g_havePos &&
        screenX == g_lastX && baselineY == g_lastY) return;
    if (!EnsureSurface()) return;
    if (g_bmpDirty) PaintDot();

    HDC scr = GetDC(NULL);
    if (!scr) return;
    POINT dst = { screenX + g_offX, baselineY + g_offY };
    POINT src = { 0, 0 };
    SIZE  szw = { g_bmpSize, g_bmpSize };
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, scr, &dst, &szw, g_mem, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ReleaseDC(NULL, scr);

    g_lastX = screenX; g_lastY = baselineY; g_havePos = 1;
    g_needPaint = 0;
}
