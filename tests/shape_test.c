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

static void TestPremultiply(void) {
    printf("[3] 预乘与透明度\n");
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
        return 0;
    }
    printf("=== shape_test ===\n");
    TestCircle();
    TestTriangle();
    TestPremultiply();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) { printf("=== FAILED ===\n"); return 1; }
    printf("=== OK ===\n");
    return 0;
}
