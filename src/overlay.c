#include "ime_status.h"

/* ================= 悬浮圆点窗口 =================
   WS_EX_LAYERED + WS_EX_TRANSPARENT + WS_EX_TOPMOST + WS_EX_NOACTIVATE：
   不抢焦点、不拦鼠标、始终最顶。用 32 位 DIB Section + UpdateLayeredWindow
   （ULW_ALPHA）画一个抗锯齿实心圆，alpha 为预乘，无 GDI+ 依赖。 */

static HWND    g_hwnd = NULL;
static int     g_dotSize = 8, g_offX = 2, g_offY = 4;
static BYTE    g_cr = 0xFF, g_cg = 0x8C, g_cb = 0x00; /* 当前颜色分量 */
static BYTE    g_alpha = 190;

static LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) return 0;
    return DefWindowProcW(h, m, w, l);
}

void OverlayInit(ImeCfg* c) {
    g_dotSize = c->size; g_offX = c->offsetX; g_offY = c->offsetY;
    g_alpha = (BYTE)c->dotAlpha;

    HINSTANCE hi = GetModuleHandleW(NULL);
    static const WCHAR cls[] = L"IMEStatusOverlay";
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
        cls, L"IMEStatus", WS_POPUP,
        -100, -100, sz, sz, NULL, NULL, hi, NULL);
    if (g_hwnd) {
        /* 初始为全透明占位，避免闪出白块 */
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 0, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_hwnd, NULL, NULL, NULL, NULL, NULL, 0, &bf, ULW_ALPHA);
    }
}

void OverlayShutdown(void) {
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
}

void OverlaySetVisible(int visible) {
    if (g_hwnd) ShowWindow(g_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void OverlaySetColor(DWORD rgb, DWORD alpha) {
    g_cr = (BYTE)(rgb >> 16);
    g_cg = (BYTE)(rgb >> 8);
    g_cb = (BYTE)(rgb);
    g_alpha = (BYTE)(alpha & 0xFF);
}

/* 画 antialias 圆并放置窗口。
   sx, baselineY：光标左上角屏幕坐标与光标底缘；窗口落在
   (sx+offsetX, baselineY+offsetY)，圆居中于窗口。 */
static void Redraw(int sx, int sy) {
    int sz = g_dotSize;
    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = sz;
    bmi.bmiHeader.biHeight = -sz;            /* 自上而下 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP hbm = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (hbm && bits) {
        HGDIOBJ old = SelectObject(mem, hbm);

        /* 预乘：UpdateLayeredWindow + AC_SRC_ALPHA 需要预乘 alpha */
        int pr = (g_cr * g_alpha + 127) / 255;
        int pg = (g_cg * g_alpha + 127) / 255;
        int pb = (g_cb * g_alpha + 127) / 255;

        double cx = (sz - 1) / 2.0, cy = (sz - 1) / 2.0;
        double r = (sz / 2.0) - 0.5;         /* 圆心到边的内切半径，留 1px 抗锯齿 */
        const int SS = 4;                     /* 4x4 子采样抗锯齿 */
        BYTE* p = (BYTE*)bits;
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                /* 计算该像素的覆盖率（子采样点圆心距） */
                int inside = 0;
                for (int syy = 0; syy < SS; syy++)
                    for (int sxx = 0; sxx < SS; sxx++) {
                        double fx = x + (sxx + 0.5) / SS - cx;
                        double fy = y + (syy + 0.5) / SS - cy;
                        if (fx * fx + fy * fy <= r * r) inside++;
                    }
                int a = (g_alpha * inside + (SS * SS - 1)) / (SS * SS);
                p[0] = (BYTE)((pb * a + 127) / 255);   /* B 预乘 */
                p[1] = (BYTE)((pg * a + 127) / 255);
                p[2] = (BYTE)((pr * a + 127) / 255);
                p[3] = (BYTE)a;
                p += 4;
            }
        }

        POINT dst = { sx + g_offX, sy + g_offY };
        POINT src = { 0, 0 };
        SIZE   szw = { sz, sz };
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(g_hwnd, scr, &dst, &szw, mem, &src, 0, &bf, ULW_ALPHA);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        SelectObject(mem, old);
        DeleteObject(hbm);
    }
    DeleteDC(mem);
    ReleaseDC(NULL, scr);
}

void OverlayMove(int screenX, int baselineY) {
    if (!g_hwnd) return;
    Redraw(screenX, baselineY);
}