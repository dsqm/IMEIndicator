#include "shape.h"
#include "ime_indicator.h"
#include <math.h>

/* 形状光栅化：按"像素中心到图形边界的距离"算覆盖率，而不是多倍采样。
   65px 以内 4x4 超采样只有 1/16 的 alpha 台阶，边缘能看出阶梯；解析覆盖率
   是连续的，而且每像素只算一次距离，比 16 次采样更快。

   alpha = clamp(0.5 + 到边界的带符号距离, 0, 1)
   —— 像素中心正好压在边界上时得到 0.5，符合面积覆盖的直觉。 */

/* 圆形：fx,fy 相对图形中心，half 是半径。返回带符号距离（内正外负）。 */
static double DistCircle(double fx, double fy, double half) {
    return half - sqrt(fx * fx + fy * fy);
}

/* 等边三角形，尖角朝上。尺寸取「宽 = 圆的直径」：等边三角形的高只有宽的
   √3/2 ≈ 0.866（这是它本来的比例，宽>高才对——之前把半宽公式用错，
   高比宽大 30%，看着又瘦又高）。与圆同宽则视觉等大，高略矮是正常形。
   光学修正：三角形的视觉重心偏底边（底边重、尖角轻），按几何中心摆会显得
   整体偏下，所以整体向上移 5% 高度（设计圈惯用做法，Play 按钮同款问题）。
   凸多边形到边界的带符号距离 = 到各条边所在直线的带符号距离的最小值。 */
static double DistTriangle(double fx, double fy, double half) {
    const double W  = half * 2.0;            /* 全宽 = 圆直径 */
    const double Ht = W * 0.8660254038;      /* 全高 = 宽×√3/2（等边） */
    const double dy = -Ht * 0.05;            /* 光学：向上微移 5% */
    const double hw = W * 0.5;               /* 半宽 */
    const double hh = Ht * 0.5;              /* 半高 */
    const double ax = 0.0,    ay = -hh + dy; /* 尖 */
    const double bx = -hw,    by = hh + dy;  /* 左下 */
    const double cx = hw,     cy = hh + dy;  /* 右下 */

    /* 三条有向边：a->b, b->c, c->a。y 轴向下，顶点按顺时针给。 */
    const double ex[3] = { bx - ax, cx - bx, ax - cx };
    const double ey[3] = { by - ay, cy - by, ay - cy };
    const double px[3] = { ax, bx, cx };
    const double py[3] = { ay, by, cy };

    /* 叉积的符号取决于绕序：先用重心定出"内部是正还是负"，
       ★ 必须**逐条**乘上这个符号再取最小值。先取 min 再翻转是错的 ——
       那样只要还有一条边的距离为负就永远判成"在内部"，填出来是个色块。 */
    double gx = (ax + bx + cx) / 3.0, gy = (ay + by + cy) / 3.0;
    double sign = 1.0;
    {
        double len = sqrt(ex[0] * ex[0] + ey[0] * ey[0]);
        if (len > 1e-9) {
            double d0 = (ex[0] * (gy - py[0]) - ey[0] * (gx - px[0])) / len;
            if (d0 < 0) sign = -1.0;
        }
    }

    double best = 1e9;
    for (int i = 0; i < 3; i++) {
        double len = sqrt(ex[i] * ex[i] + ey[i] * ey[i]);
        if (len < 1e-9) continue;
        /* 叉积 / 边长 = 到该边所在直线的带符号距离 */
        double d = (ex[i] * (fy - py[i]) - ey[i] * (fx - px[i])) / len;
        d *= sign;                     /* 转成"内正外负" */
        if (d < best) best = d;
    }
    return best;
}

void ShapeRasterize(BYTE* bgra, int size, DWORD rgb, int alpha, int shape) {
    if (!bgra || size < 1) return;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;

    int cr = (int)((rgb >> 16) & 0xFF);
    int cg = (int)((rgb >> 8) & 0xFF);
    int cb = (int)(rgb & 0xFF);
    /* 预乘：UpdateLayeredWindow / AlphaBlend 的 AC_SRC_ALPHA 都要求预乘 */
    int pr = (cr * alpha + 127) / 255;
    int pg = (cg * alpha + 127) / 255;
    int pb = (cb * alpha + 127) / 255;

    double cxp = (size - 1) / 2.0, cyp = (size - 1) / 2.0;
    double r = (size / 2.0) - 0.5;           /* 半径，留 1px 给边缘过渡 */
    int tri = (shape == SHAPE_TRIANGLE);

    BYTE* p = bgra;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double fx = x - cxp, fy = y - cyp;
            double d = tri ? DistTriangle(fx, fy, r) : DistCircle(fx, fy, r);
            double cov = d + 0.5;
            if (cov < 0) cov = 0;
            if (cov > 1) cov = 1;
            int a = (int)(alpha * cov + 0.5);
            p[0] = (BYTE)((pb * a + 127) / 255);   /* B */
            p[1] = (BYTE)((pg * a + 127) / 255);   /* G */
            p[2] = (BYTE)((pr * a + 127) / 255);   /* R */
            p[3] = (BYTE)a;
            p += 4;
        }
    }
}
