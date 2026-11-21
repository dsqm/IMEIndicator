#include "uipaint.h"
#include <math.h>
#include <string.h>

#pragma comment(lib, "msimg32.lib")

/* 覆盖率光栅化：alpha = clamp(0.5 + 到边界的带符号距离, 0, 1)。
 * 像素中心正好压在边界上时得到 0.5，与面积覆盖的直觉一致；65000 像素围成的
 * 小圆也只多算一次 sqrt，比分倍采样更省。 */

static double Clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* 覆盖率 a 的纯色叠到已有的预乘像素上（src-over，两边都是预乘 BGRA） */
static void BlendPx(BYTE* p, DWORD rgb, int a) {
    if (a <= 0) return;
    int r = (int)((rgb >> 16) & 0xFF);
    int g = (int)((rgb >> 8) & 0xFF);
    int b = (int)(rgb & 0xFF);
    if (a >= 255) {
        p[0] = (BYTE)b;
        p[1] = (BYTE)g;
        p[2] = (BYTE)r;
        p[3] = 255;
        return;
    }
    int ia = 255 - a;
    p[0] = (BYTE)((b * a + p[0] * ia + 127) / 255);
    p[1] = (BYTE)((g * a + p[1] * ia + 127) / 255);
    p[2] = (BYTE)((r * a + p[2] * ia + 127) / 255);
    p[3] = (BYTE)((a * 255 + p[3] * ia + 127) / 255);
}

/* 把画布上的一个矩形范围裁到图层内 */
static void ClipBox(int* x0, int* y0, int* x1, int* y1, int w, int h) {
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > w) *x1 = w;
    if (*y1 > h) *y1 = h;
}

/* 圆角矩形的带符号距离场（内负外正）。等价于"把四个角换成半径 radius 的
 * 圆弧"—— 圆角外的那块是真正透明的，不会被写成直角。 */
static double RoundRectSDF(double px, double py,
                           double x0, double y0, double x1, double y1,
                           double radius) {
    double hw = (x1 - x0) * 0.5;
    double hh = (y1 - y0) * 0.5;
    double cx = x0 + hw;
    double cy = y0 + hh;
    if (hw < 0 || hh < 0) return 1.0;
    if (radius > hw) radius = hw;
    if (radius > hh) radius = hh;
    if (radius < 0) radius = 0;
    double qx = fabs(px - cx) - (hw - radius);
    double qy = fabs(py - cy) - (hh - radius);
    double ox = qx > 0.0 ? qx : 0.0;
    double oy = qy > 0.0 ? qy : 0.0;
    double mx = qx > qy ? qx : qy;
    if (mx > 0.0) mx = 0.0;
    return sqrt(ox * ox + oy * oy) + mx - radius;
}

/* 点到线段的距离 */
static double DistSeg(double px, double py,
                      double x0, double y0, double x1, double y1) {
    double vx = x1 - x0, vy = y1 - y0;
    double len2 = vx * vx + vy * vy;
    double t = 0.0;
    if (len2 > 1e-12) {
        t = ((px - x0) * vx + (py - y0) * vy) / len2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
    }
    double dx = px - (x0 + vx * t);
    double dy = py - (y0 + vy * t);
    return sqrt(dx * dx + dy * dy);
}

void UIPixRoundRect(BYTE* bgra, int w, int h,
                    double x0, double y0, double x1, double y1,
                    double radius, DWORD rgb, int alpha) {
    if (!bgra || w <= 0 || h <= 0 || alpha <= 0 || x1 <= x0 || y1 <= y0) return;
    int bx0 = (int)floor(x0 - 1.0), by0 = (int)floor(y0 - 1.0);
    int bx1 = (int)ceil(x1 + 1.0),  by1 = (int)ceil(y1 + 1.0);
    ClipBox(&bx0, &by0, &bx1, &by1, w, h);
    for (int y = by0; y < by1; y++) {
        BYTE* row = bgra + (size_t)y * w * 4;
        for (int x = bx0; x < bx1; x++) {
            double d = -RoundRectSDF(x + 0.5, y + 0.5, x0, y0, x1, y1, radius);
            int a = (int)(Clamp01(0.5 + d) * alpha + 0.5);
            if (a > 0) BlendPx(row + (size_t)x * 4, rgb, a);
        }
    }
}

void UIPixCircle(BYTE* bgra, int w, int h,
                 double cx, double cy, double radius, DWORD rgb, int alpha) {
    if (!bgra || w <= 0 || h <= 0 || alpha <= 0 || radius <= 0.0) return;
    int bx0 = (int)floor(cx - radius - 1.0), by0 = (int)floor(cy - radius - 1.0);
    int bx1 = (int)ceil(cx + radius + 1.0),  by1 = (int)ceil(cy + radius + 1.0);
    ClipBox(&bx0, &by0, &bx1, &by1, w, h);
    for (int y = by0; y < by1; y++) {
        BYTE* row = bgra + (size_t)y * w * 4;
        for (int x = bx0; x < bx1; x++) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            double d = radius - sqrt(dx * dx + dy * dy);
            int a = (int)(Clamp01(0.5 + d) * alpha + 0.5);
            if (a > 0) BlendPx(row + (size_t)x * 4, rgb, a);
        }
    }
}

void UIPixCapsule(BYTE* bgra, int w, int h,
                  double x0, double y0, double x1, double y1,
                  double halfWidth, DWORD rgb, int alpha) {
    if (!bgra || w <= 0 || h <= 0 || alpha <= 0 || halfWidth <= 0.0) return;
    double l = x0 < x1 ? x0 : x1, r = x0 < x1 ? x1 : x0;
    double t = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
    int bx0 = (int)floor(l - halfWidth - 1.0), by0 = (int)floor(t - halfWidth - 1.0);
    int bx1 = (int)ceil(r + halfWidth + 1.0),  by1 = (int)ceil(b + halfWidth + 1.0);
    ClipBox(&bx0, &by0, &bx1, &by1, w, h);
    for (int y = by0; y < by1; y++) {
        BYTE* row = bgra + (size_t)y * w * 4;
        for (int x = bx0; x < bx1; x++) {
            double d = halfWidth - DistSeg(x + 0.5, y + 0.5, x0, y0, x1, y1);
            int a = (int)(Clamp01(0.5 + d) * alpha + 0.5);
            if (a > 0) BlendPx(row + (size_t)x * 4, rgb, a);
        }
    }
}

int UILayerBegin(UILayer* l, HDC ref, int w, int h) {
    if (!l) return 0;
    ZeroMemory(l, sizeof(*l));
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;          /* 负高 = 自上而下，和调用方坐标一致 */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    l->dc = CreateCompatibleDC(ref);
    if (!l->dc) return 0;
    void* bits = NULL;
    l->bmp = CreateDIBSection(l->dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!l->bmp || !bits) {
        UILayerEnd(l);
        return 0;
    }
    l->old = SelectObject(l->dc, l->bmp);
    l->px = (BYTE*)bits;
    l->w = w;
    l->h = h;
    /* DIB 内容是未初始化的，不透明残留会当成"已画好"贴上去 */
    ZeroMemory(l->px, (size_t)w * h * 4);
    return 1;
}

void UILayerBlend(UILayer* l, HDC dst, int x, int y) {
    if (!l || !l->dc || !l->bmp || !dst) return;
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    AlphaBlend(dst, x, y, l->w, l->h, l->dc, 0, 0, l->w, l->h, bf);
}

void UILayerEnd(UILayer* l) {
    if (!l) return;
    if (l->dc) {
        if (l->old) SelectObject(l->dc, l->old);
        if (l->bmp) DeleteObject(l->bmp);
        DeleteDC(l->dc);
    }
    ZeroMemory(l, sizeof(*l));
}
