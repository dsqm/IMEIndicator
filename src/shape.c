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

/* 前置声明：三角形直接复用下方的凸多边形实现，不再自己维护一份符号+边循环。 */
static double DistConvexPolygon(double fx, double fy,
                                const double* vx, const double* vy, int n);

/* 等边三角形，尖角朝上。尺寸取「宽 = 圆的直径」：等边三角形的高只有宽的
   √3/2 ≈ 0.866（这是它本来的比例，宽>高才对——之前把半宽公式用错，
   高比宽大 30%，看着又瘦又高）。与圆同宽则视觉等大，高略矮是正常形。
   光学修正：三角形的视觉重心偏底边（底边重、尖角轻），按几何中心摆会显得
   整体偏下，所以整体向上移 5% 高度（设计圈惯用做法，Play 按钮同款问题）。
   ★ 顶点与旧实现逐字一致（宽 = 2r、高 = √3/2·宽），面积仍是 (√3/4)W²，
     测试里的面积断言不用改；只是把「符号 + 边循环」交给 DistConvexPolygon。 */
static double DistTriangle(double fx, double fy, double half) {
    const double W  = half * 2.0;            /* 全宽 = 圆直径 */
    const double Ht = W * 0.8660254038;      /* 全高 = 宽×√3/2（等边） */
    const double dy = -Ht * 0.05;            /* 光学：向上微移 5% */
    const double hw = W * 0.5;               /* 半宽 */
    const double hh = Ht * 0.5;              /* 半高 */
    const double vx[3] = { 0.0, -hw, hw };   /* 尖、左下、右下 */
    const double vy[3] = { -hh + dy, hh + dy, hh + dy };
    return DistConvexPolygon(fx, fy, vx, vy, 3);
}

/* 方形：到四条边所在直线的距离取最小（内正外负） */
static double DistSquare(double fx, double fy, double half) {
    double ax = fabs(fx), ay = fabs(fy);
    return half - (ax > ay ? ax : ay);
}

/* 菱形：顶点在上下左右，边界 |x|+|y| = half；到边界距离要除以 √2 */
static double DistDiamond(double fx, double fy, double half) {
    return (half - fabs(fx) - fabs(fy)) * 0.70710678118654752;
}

/* 圆角方形：圆角 = 边长的 25%（半边长的一半）。
   标准圆角矩形 SDF 取反，转成"内正外负"。 */
static double DistRounded(double fx, double fy, double half) {
    double cr = half * 0.5;
    double qx = fabs(fx) - (half - cr);
    double qy = fabs(fy) - (half - cr);
    double mx = qx > 0 ? qx : 0;
    double my = qy > 0 ? qy : 0;
    double len = sqrt(mx * mx + my * my);
    double mn = qx > qy ? qx : qy;
    if (mn > 0) mn = 0;
    return cr - len - mn;
}

/* 凸多边形：到各边所在直线的带符号距离取最小。符号用重心定：
   重心一定在内部，它到某条边的符号就是"内部"的符号。 */
static double DistConvexPolygon(double fx, double fy,
                                const double* vx, const double* vy, int n) {
    double gx = 0, gy = 0;
    for (int i = 0; i < n; i++) { gx += vx[i]; gy += vy[i]; }
    gx /= n; gy /= n;

    double best = 1e9;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        double ex = vx[j] - vx[i], ey = vy[j] - vy[i];
        double len = sqrt(ex * ex + ey * ey);
        if (len < 1e-9) continue;
        double d  = (ex * (fy - vy[i]) - ey * (fx - vx[i])) / len;
        double dg = (ex * (gy - vy[i]) - ey * (gx - vx[i])) / len;
        if (dg < 0) d = -d;
        if (d < best) best = d;
    }
    return best;
}

/* 六边形，尖角朝上：外接圆半径 = half */
static double DistHexagon(double fx, double fy, double half) {
    static const double ux[6] = { 0.0, 0.8660254037844386, 0.8660254037844386,
                                  0.0, -0.8660254037844386, -0.8660254037844386 };
    static const double uy[6] = { -1.0, -0.5, 0.5, 1.0, 0.5, -0.5 };
    double vx[6], vy[6];
    for (int i = 0; i < 6; i++) { vx[i] = ux[i] * half; vy[i] = uy[i] * half; }
    return DistConvexPolygon(fx, fy, vx, vy, 6);
}

/* 圆环：环宽 = 半径的 30% */
static double DistRing(double fx, double fy, double half) {
    double inner = half * 0.7;
    double len = sqrt(fx * fx + fy * fy);
    double mid = (half + inner) * 0.5;
    double thick = (half - inner) * 0.5;
    return thick - fabs(len - mid);
}

/* 任意半轴菱形：|x|/a + |y|/b <= 1。到边界距离 = 分子 / √(1/a²+1/b²) */
static double DistDiamondAB(double fx, double fy, double a, double b) {
    return (1.0 - fabs(fx) / a - fabs(fy) / b) * (a * b) / sqrt(a * a + b * b);
}

/* 四角星：横、竖两个细菱形取并集。
   本文件约定"内正外负"，并集取 max（标准 SDF 的负内约定才是 min）。 */
static double DistSparkle(double fx, double fy, double half) {
    double thin = half * 0.35;
    double dh = DistDiamondAB(fx, fy, half, thin);
    double dv = DistDiamondAB(fx, fy, thin, half);
    return dh > dv ? dh : dv;
}

/* 半圆：平边朝上，圆心在 (0, -half/2)，半径 half，取下半部分。
   圆与半平面求交 = min。 */
static double DistSemicircle(double fx, double fy, double half) {
    double cy = fy + half * 0.5;
    double dc = half - sqrt(fx * fx + cy * cy);
    double dp = cy;   /* y >= -half/2 */
    return dc < dp ? dc : dp;
}

/* 梯形：平顶，上宽 = half，下宽 = 2half，高 = 2half */
static double DistTrapezoid(double fx, double fy, double half) {
    const double vx[4] = { -half * 0.5, half * 0.5, half, -half };
    const double vy[4] = { -half, -half, half, half };
    return DistConvexPolygon(fx, fy, vx, vy, 4);
}

typedef double (*DistFn)(double, double, double);

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

    DistFn dist = DistCircle;
    switch (shape) {
    case SHAPE_TRIANGLE: dist = DistTriangle; break;
    case SHAPE_SQUARE:   dist = DistSquare;   break;
    case SHAPE_DIAMOND:  dist = DistDiamond;  break;
    case SHAPE_ROUNDED:  dist = DistRounded;  break;
    case SHAPE_HEXAGON:  dist = DistHexagon;  break;
    case SHAPE_RING:     dist = DistRing;     break;
    case SHAPE_SPARKLE:    dist = DistSparkle;    break;
    case SHAPE_SEMICIRCLE: dist = DistSemicircle; break;
    case SHAPE_TRAPEZOID:  dist = DistTrapezoid;  break;
    default:             dist = DistCircle;   break;
    }

    BYTE* p = bgra;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            double fx = x - cxp, fy = y - cyp;
            double d = dist(fx, fy, r);
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
