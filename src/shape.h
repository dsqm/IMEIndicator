/* shape.h —— 形状光栅化（悬浮提示与设置程序预览共用）
 *
 * 共用一份的意义：设置界面里的预览必须和真实圆点**逐像素一致**，
 * 各画各的必然出现"预览有锯齿、实际没有"这类差异。
 */
#ifndef IME_SHAPE_H
#define IME_SHAPE_H

#include <windows.h>

/* 把形状画进 size×size 的 32 位自上而下 DIB（BGRA 预乘 alpha）。
 * shape 取 SHAPE_CIRCLE / SHAPE_TRIANGLE（SHAPE_NONE / SHAPE_IMAGE 由调用方处理）。
 * rgb 是 0x00RRGGBB，alpha 是 0..255 的整体不透明度。 */
void ShapeRasterize(BYTE* bgra, int size, DWORD rgb, int alpha, int shape);

#endif
