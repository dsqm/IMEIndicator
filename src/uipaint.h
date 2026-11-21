/* uipaint.h —— 设置界面自绘控件的抗锯齿图形
 *
 * GDI 的 Ellipse / RoundRect / Polyline 都是硬边光栅化：14px 的滑块圆点、
 * 16px 的复选框圆角在 175% DPI 下台阶非常明显（预览用的悬浮窗图形走的是
 * shape.c 的覆盖率光栅化，所以那边反而更干净）。这里把同样一套覆盖率算法
 * 做成通用画笔，供按钮 / 复选框 / 滑块 / 下拉共用。
 *
 * 坐标约定：连续的设备像素，像素 (x,y) 覆盖 [x,x+1)×[y,y+1)，中心在
 * (x+0.5, y+0.5)。所以矩形给 [0,0,24,24] 正好覆盖 24×24 个像素，边缘整齐。 */
#ifndef IME_UIPAINT_H
#define IME_UIPAINT_H

#include <windows.h>

/* 直接往一块 w×h 的 32 位**预乘 BGRA** 图层（自上而下）上画。
 * rgb 是 0x00RRGGBB，alpha 是 0..255 的整体不透明度。 */
void UIPixRoundRect(BYTE* bgra, int w, int h,
                    double x0, double y0, double x1, double y1,
                    double radius, DWORD rgb, int alpha);
void UIPixCircle(BYTE* bgra, int w, int h,
                 double cx, double cy, double radius, DWORD rgb, int alpha);
/* 带圆头的粗线（复选框的对勾就是两段） */
void UIPixCapsule(BYTE* bgra, int w, int h,
                  double x0, double y0, double x1, double y1,
                  double halfWidth, DWORD rgb, int alpha);

/* 一块临时图层：图形都画进它，最后整块 AlphaBlend 到目标 DC 上。
 * 一次绘制里多个图形共用一块，比每个图形各建一次 DIB 省得多。 */
typedef struct {
    HDC     dc;
    HBITMAP bmp;
    HGDIOBJ old;
    BYTE*   px;
    int     w, h;
} UILayer;

/* 成功返回 1，失败返回 0（调用方应回退到 GDI 硬边绘制，功能优先）。
 * ref 只用来建兼容 DC，可以传 NULL。 */
int  UILayerBegin(UILayer* l, HDC ref, int w, int h);
void UILayerBlend(UILayer* l, HDC dst, int x, int y);
void UILayerEnd(UILayer* l);

#endif
