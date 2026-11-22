/* shape_test.c —— 形状光栅化自检
 * 默认跑断言（面积、边界、预乘）；`--dump` 打印 ASCII 图供肉眼检查。 */
#include "../src/shape.h"
#include "../src/ime_indicator.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

#define N 64
static BYTE g_buf[N * N * 4];

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  [FAIL] %s (line %d)\n", msg, __LINE__); } \
} while (0)

static int AlphaAt(int x, int y) { return g_buf[(y * N + x) * 4 + 3]; }

/* 覆盖面积 = alpha 总和 / 255 */
static double CoveredArea(void) {
    double s = 0;
    for (int i = 0; i < N * N; i++) s += g_buf[i * 4 + 3];
    return s / 255.0;
}

static void TestCircle(void) {
    printf("[1] 圆\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_CIRCLE);

    double r = N / 2.0 - 0.5;
    double want = 3.14159265 * r * r;
    double got = CoveredArea();
    int ok = fabs(got - want) / want < 0.04;
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(ok, "圆面积接近 πr²");

    CHECK(AlphaAt(N/2, N/2) == 255, "圆心不透明");
    CHECK(AlphaAt(0, 0) == 0, "左上角在圆外");
    CHECK(AlphaAt(N-1, N-1) == 0, "右下角在圆外");

    /* 边缘必须有中间值（抗锯齿）：统计 0<alpha<255 的像素数 */
    int partial = 0;
    for (int i = 0; i < N * N; i++) {
        int a = g_buf[i * 4 + 3];
        if (a > 0 && a < 255) partial++;
    }
    printf("  边缘过渡像素 %d\n", partial);
    CHECK(partial > 50, "圆边缘有抗锯齿过渡");
}

static void TestTriangle(void) {
    printf("[2] 三角形\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_TRIANGLE);

    double W = N - 1.0;
    double want = 0.4330127 * W * W;   /* √3/4 × 边长² */
    double got = CoveredArea();
    int ok = fabs(got - want) / want < 0.06;
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(ok, "三角形面积接近 (√3/4)W²");

    CHECK(AlphaAt(N/2, N/2) == 255, "重心不透明");
    CHECK(AlphaAt(0, 0) == 0, "左上角在三角形外");
    CHECK(AlphaAt(N-1, 0) == 0, "右上角在三角形外");
    /* 等边三角形的高只有宽的 0.866，底部两角并不是画布角 ——
       取底边附近、明显在内部的两点。 */
    CHECK(AlphaAt(16, 50) > 128, "左下部在三角形内");
    CHECK(AlphaAt(N-17, 50) > 128, "右下部在三角形内");
    CHECK(AlphaAt(0, N-2) == 0, "左下画布角在三角形外");

    int partial = 0;
    for (int i = 0; i < N * N; i++) {
        int a = g_buf[i * 4 + 3];
        if (a > 0 && a < 255) partial++;
    }
    printf("  边缘过渡像素 %d\n", partial);
    CHECK(partial > 60, "三角形边缘有抗锯齿过渡");
}

static void TestSquare(void) {
    printf("[3] 方形\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_SQUARE);

    double r = N / 2.0 - 0.5;
    double want = 4.0 * r * r;
    double got = CoveredArea();
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(fabs(got - want) / want < 0.05, "方形面积接近 (2r)²");

    CHECK(AlphaAt(N/2, N/2) == 255, "方形中心不透明");
    /* 方形正好填满外接方形，四个角落在像素中心上：半覆盖，不是全透明 */
    CHECK(AlphaAt(0, 0) > 0 && AlphaAt(0, 0) < 255, "方形左上角是边界过渡");
    CHECK(AlphaAt(N-1, N-1) > 0 && AlphaAt(N-1, N-1) < 255, "方形右下角是边界过渡");
    CHECK(AlphaAt(N/2, 2) > 200, "方形上边内部");
    CHECK(AlphaAt(2, N/2) > 200, "方形左边内部");
}

static void TestDiamond(void) {
    printf("[4] 菱形\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_DIAMOND);

    double r = N / 2.0 - 0.5;
    double want = 2.0 * r * r;
    double got = CoveredArea();
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(fabs(got - want) / want < 0.05, "菱形面积接近 2r²");

    CHECK(AlphaAt(N/2, N/2) == 255, "菱形中心不透明");
    CHECK(AlphaAt(0, 0) == 0, "菱形左上角在外");
    CHECK(AlphaAt(N/2, 2) > 200, "菱形上顶点附近");
    CHECK(AlphaAt(2, N/2) > 200, "菱形左顶点附近");
    CHECK(AlphaAt(N/2, N-3) > 200, "菱形下顶点附近");
}

static void TestRounded(void) {
    printf("[5] 圆角方形\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_ROUNDED);

    double r = N / 2.0 - 0.5;
    double cr = r * 0.5;
    double want = 4.0 * r * r - (4.0 - 3.14159265) * cr * cr;
    double got = CoveredArea();
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(fabs(got - want) / want < 0.05, "圆角方形面积接近理论值");

    CHECK(AlphaAt(N/2, N/2) == 255, "圆角方形中心不透明");
    CHECK(AlphaAt(0, 0) == 0, "圆角方形左上角在外");
    CHECK(AlphaAt(N/2, 2) > 200, "圆角方形上边内部");
    CHECK(AlphaAt(2, N/2) > 200, "圆角方形左边内部");
}

static void TestHexagon(void) {
    printf("[6] 六边形\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_HEXAGON);

    double r = N / 2.0 - 0.5;
    double want = 2.598076211 * r * r;   /* (3√3/2)r² */
    double got = CoveredArea();
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(fabs(got - want) / want < 0.05, "六边形面积接近 (3√3/2)r²");

    CHECK(AlphaAt(N/2, N/2) == 255, "六边形中心不透明");
    CHECK(AlphaAt(0, 0) == 0, "六边形左上角在外");
    CHECK(AlphaAt(N/2, 2) > 200, "六边形上顶点附近");
    CHECK(AlphaAt(6, N/2) > 200, "六边形左边内部");
}

static void TestRing(void) {
    printf("[7] 圆环\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_RING);

    double r = N / 2.0 - 0.5;
    double inner = r * 0.7;
    double want = 3.14159265 * (r * r - inner * inner);
    double got = CoveredArea();
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(fabs(got - want) / want < 0.05, "圆环面积接近 π(r²-inner²)");

    CHECK(AlphaAt(N/2, N/2) == 0, "圆环中心是空的");
    CHECK(AlphaAt(N/2, 2) > 200, "圆环上边有料");
    CHECK(AlphaAt(2, N/2) > 200, "圆环左边有料");
    CHECK(AlphaAt(0, 0) == 0, "圆环左上角在外");
}

static void TestSparkle(void) {
    printf("[8] 四角星\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_SPARKLE);

    double r = N / 2.0 - 0.5;
    /* 两个半轴 r × 0.35r 的菱形并集：4r²·t/(1+t)，t=0.35 */
    double want = 4.0 * r * r * 0.35 / 1.35;
    double got = CoveredArea();
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(fabs(got - want) / want < 0.05, "四角星面积接近理论值");

    CHECK(AlphaAt(N/2, N/2) == 255, "四角星中心不透明");
    CHECK(AlphaAt(0, 0) == 0, "四角星左上角在外");
    CHECK(AlphaAt(N/2, 6) > 200, "四角星上尖角附近");
    CHECK(AlphaAt(6, N/2) > 200, "四角星左尖角附近");
}

static void TestSemicircle(void) {
    printf("[9] 半圆\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_SEMICIRCLE);

    double r = N / 2.0 - 0.5;
    double want = 3.14159265 * r * r * 0.5;
    double got = CoveredArea();
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(fabs(got - want) / want < 0.05, "半圆面积接近 πr²/2");

    CHECK(AlphaAt(N/2, N/2) == 255, "半圆中心不透明");
    CHECK(AlphaAt(0, 0) == 0, "半圆左上角在外");
    CHECK(AlphaAt(N/2, N/2 + 14) > 200, "半圆下部内部");
    CHECK(AlphaAt(N/2, N/2 - 18) == 0, "半圆平边上方为空");
}

static void TestTrapezoid(void) {
    printf("[10] 梯形\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0x00A000, 255, SHAPE_TRAPEZOID);

    double r = N / 2.0 - 0.5;
    double want = 3.0 * r * r;   /* (上宽 r + 下宽 2r)/2 × 高 2r */
    double got = CoveredArea();
    printf("  面积 %.1f (理论 %.1f)\n", got, want);
    CHECK(fabs(got - want) / want < 0.05, "梯形面积接近 3r²");

    CHECK(AlphaAt(N/2, N/2) == 255, "梯形中心不透明");
    CHECK(AlphaAt(0, 0) == 0, "梯形左上角在外");
    CHECK(AlphaAt(N/2, 2) > 200, "梯形上边内部");
    CHECK(AlphaAt(N/2, N-3) > 200, "梯形下边内部");
}

static void TestPremultiply(void) {
    printf("[11] 预乘与透明度\n");
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, N, 0xFF8000, 128, SHAPE_CIRCLE);

    /* 预乘：每个通道都不能超过 alpha */
    int bad = 0;
    for (int i = 0; i < N * N; i++) {
        int b = g_buf[i*4], g = g_buf[i*4+1], r = g_buf[i*4+2], a = g_buf[i*4+3];
        if (b > a || g > a || r > a) bad++;
    }
    CHECK(bad == 0, "所有像素都是预乘 alpha");

    /* 半透明圆：圆心 alpha 应为 128 左右 */
    int a = AlphaAt(N/2, N/2);
    printf("  半透明圆心 alpha=%d\n", a);
    CHECK(a >= 120 && a <= 136, "整体 alpha 生效");

    /* 尺寸变小仍然能画。注意要按 8px 的步长读，不能用 AlphaAt（那是 64 步长） */
    memset(g_buf, 0, sizeof(g_buf));
    ShapeRasterize(g_buf, 8, 0xFF0000, 255, SHAPE_CIRCLE);
    CHECK(g_buf[(4 * 8 + 4) * 4 + 3] > 200, "小尺寸（8px）也能正确填充");
}

static void Dump(int shape, const char* title, int n) {
    static BYTE buf[N * N * 4];
    ShapeRasterize(buf, n, 0x00A000, 255, shape);
    printf("=== %s (%dx%d) ===\n", title, n, n);
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            int a = buf[(y * n + x) * 4 + 3];
            char c = a == 0 ? '.' : a < 64 ? ':' : a < 128 ? '+' : a < 192 ? '*' : a < 255 ? '#' : '@';
            putchar(c);
        }
        putchar('\n');
    }
    putchar('\n');
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        Dump(SHAPE_CIRCLE, "circle", 32);
        Dump(SHAPE_TRIANGLE, "triangle", 32);
        Dump(SHAPE_SQUARE, "square", 32);
        Dump(SHAPE_DIAMOND, "diamond", 32);
        Dump(SHAPE_ROUNDED, "rounded_square", 32);
        Dump(SHAPE_HEXAGON, "hexagon", 32);
        Dump(SHAPE_RING, "ring", 32);
        Dump(SHAPE_SPARKLE, "sparkle", 32);
        Dump(SHAPE_SEMICIRCLE, "semicircle", 32);
        Dump(SHAPE_TRAPEZOID, "trapezoid", 32);
        return 0;
    }
    printf("=== shape_test ===\n");
    TestCircle();
    TestTriangle();
    TestSquare();
    TestDiamond();
    TestRounded();
    TestHexagon();
    TestRing();
    TestSparkle();
    TestSemicircle();
    TestTrapezoid();
    TestPremultiply();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) { printf("=== FAILED ===\n"); return 1; }
    printf("=== OK ===\n");
    return 0;
}
