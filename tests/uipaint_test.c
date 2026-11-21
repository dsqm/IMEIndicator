/* uipaint_test.c —— 设置界面抗锯齿画笔自检
 * 覆盖率面积、圆角透明、预乘不变式，以及图层 AlphaBlend 到目标 DC。 */
#include "../src/uipaint.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

#define W 32
#define H 32
static BYTE g_buf[W * H * 4];

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) g_pass++; \
    else { g_fail++; printf("  [FAIL] %s (line %d)\n", msg, __LINE__); } \
} while (0)

static int AlphaAt(int x, int y) { return g_buf[(y * W + x) * 4 + 3]; }

/* alpha 总和 / 255 = 已覆盖的像素面积（覆盖率是按面积积分的） */
static double CoveredArea(void) {
    double s = 0;
    for (int i = 0; i < W * H; i++) s += g_buf[i * 4 + 3];
    return s / 255.0;
}

static int PartialCount(void) {
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        int a = g_buf[i * 4 + 3];
        if (a > 0 && a < 255) n++;
    }
    return n;
}

static void TestCircle(void) {
    printf("[1] 圆\n");
    memset(g_buf, 0, sizeof(g_buf));
    UIPixCircle(g_buf, W, H, 16.0, 16.0, 12.0, 0x0078D7, 255);

    double want = 3.14159265 * 12.0 * 12.0;
    double got = CoveredArea();
    printf("  面积 %.1f (理论 %.1f)，过渡像素 %d\n", got, want, PartialCount());
    CHECK(fabs(got - want) / want < 0.03, "圆面积接近 πr²");
    CHECK(AlphaAt(16, 16) == 255, "圆心不透明");
    CHECK(AlphaAt(0, 0) == 0, "角落在圆外");
    CHECK(AlphaAt(16, 3) == 0, "圆顶外侧透明");
    CHECK(PartialCount() > 20, "圆边缘有抗锯齿过渡");
}

static void TestRoundRect(void) {
    printf("[2] 圆角矩形\n");
    memset(g_buf, 0, sizeof(g_buf));
    UIPixRoundRect(g_buf, W, H, 2.0, 2.0, 30.0, 20.0, 6.0, 0xFF8000, 255);

    CHECK(AlphaAt(16, 10) == 255, "矩形内部不透明");
    CHECK(AlphaAt(1, 10) == 0, "直边外侧透明");
    CHECK(AlphaAt(2, 10) == 255, "直边内侧贴边铺满");
    CHECK(AlphaAt(2, 2) == 0, "圆角外的角是透明的");
    CHECK(AlphaAt(4, 4) == 255, "圆角内的实体不透明");
    CHECK(AlphaAt(3, 3) > 0 && AlphaAt(3, 3) < 255, "圆角边缘有过渡");
    CHECK(AlphaAt(16, 24) == 0, "矩形下方透明");
}

static void TestCapsule(void) {
    printf("[3] 圆头粗线\n");
    memset(g_buf, 0, sizeof(g_buf));
    UIPixCapsule(g_buf, W, H, 8.0, 16.0, 24.0, 16.0, 2.0, 0xFFFFFF, 255);

    CHECK(AlphaAt(16, 16) == 255, "线中不透明");
    CHECK(AlphaAt(16, 13) == 0, "线外透明");
    CHECK(AlphaAt(8, 16) == 255, "端点圆头覆盖");
    CHECK(AlphaAt(5, 16) == 0, "圆头外透明");
    CHECK(AlphaAt(24, 16) == 255, "右端点圆头覆盖");
}

static void TestPremultiply(void) {
    printf("[4] 预乘与整体 alpha\n");
    memset(g_buf, 0, sizeof(g_buf));
    UIPixCircle(g_buf, W, H, 16.0, 16.0, 12.0, 0xFF8000, 128);

    int bad = 0;
    for (int i = 0; i < W * H; i++) {
        int b = g_buf[i * 4], g = g_buf[i * 4 + 1], r = g_buf[i * 4 + 2], a = g_buf[i * 4 + 3];
        if (b > a || g > a || r > a) bad++;
    }
    CHECK(bad == 0, "所有像素都是预乘 alpha");
    int a = AlphaAt(16, 16);
    printf("  半透明圆心 alpha=%d\n", a);
    CHECK(a >= 120 && a <= 136, "整体 alpha 生效");
}

/* 图层贴图：AlphaBlend 要求 AC_SRC_ALPHA + 预乘，这里用白底目标回读验证 */
static void TestLayer(void) {
    printf("[5] 图层贴图\n");
    UILayer lay;
    CHECK(UILayerBegin(&lay, NULL, 8, 8) == 1, "图层创建成功");
    if (!lay.px) return;                       /* 建不出来后面的断言没有意义 */

    int clean = 1;
    for (int i = 0; i < 8 * 8 * 4; i++)
        if (lay.px[i]) clean = 0;
    CHECK(clean, "图层初始化为全透明");
    UIPixCircle(lay.px, 8, 8, 4.0, 4.0, 3.0, 0x0000FF, 255);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 8;
    bmi.bmiHeader.biHeight = -8;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(NULL);
    void* bits = NULL;
    HBITMAP dst = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    CHECK(dst != NULL, "白底目标 DIB 创建成功");
    if (dc && dst && bits) {
        HGDIOBJ old = SelectObject(dc, dst);
        memset(bits, 0xFF, 8 * 8 * 4);         /* 白底 */
        UILayerBlend(&lay, dc, 0, 0);
        BYTE* d = (BYTE*)bits;
        int cen = (4 * 8 + 4) * 4;
        printf("  圆心 BGR=(%d,%d,%d) alpha=%d\n", d[cen], d[cen + 1], d[cen + 2], d[cen + 3]);
        CHECK(d[cen] == 255 && d[cen + 1] == 0 && d[cen + 2] == 0, "圆心按蓝色贴上");
        CHECK(d[0] == 255 && d[1] == 255 && d[2] == 255, "圆外保持白底");
        SelectObject(dc, old);
    }
    if (dst) DeleteObject(dst);
    if (dc) DeleteDC(dc);

    UILayerEnd(&lay);
    CHECK(lay.px == NULL && lay.dc == NULL, "图层释放后清零");
}

int main(void) {
    printf("=== uipaint_test ===\n");
    TestCircle();
    TestRoundRect();
    TestCapsule();
    TestPremultiply();
    TestLayer();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) { printf("=== FAILED ===\n"); return 1; }
    printf("=== OK ===\n");
    return 0;
}
