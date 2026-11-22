/* settings_dirty_test.c —— 「有未保存改动」判定的单元测试（控制台程序）
 *
 * 只测 SettingsCfgSame：保存按钮亮不亮全靠它，而它不碰窗口，可以单测。
 *   1. 完全相同的配置 -> 无改动
 *   2. 「自定义」位不同、值相同 -> 有改动（取消勾选后保存按钮要亮）
 *   3. 值不同 -> 有改动
 *   4. 打开设置时 CommitForm 补的 SF_ENABLED 位不算改动
 *   5. 继承关系、黑名单、全局项变化 -> 有改动
 */
#include "../src/settings_internal.h"
#include <stdio.h>
#include <wchar.h>
#include <fcntl.h>
#include <io.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; wprintf(L"  [FAIL] %s (line %d)\n", msg, __LINE__); } \
} while (0)

/* ---------- 1. 完全相同 ---------- */
static void TestSame(void) {
    wprintf(L"[1] 完全相同的配置\n");
    ImeCfg a, b;
    CfgDefaults(&a);
    b = a;
    CHECK(SettingsCfgSame(&a, &b), L"默认配置与自身副本等价");
}

/* ---------- 2. 「自定义」位 ---------- */
/* 取消勾选「自定义」只清 have 位、不改值：存出来的 JSON 会少一个字段，
 * 必须算改动，否则保存按钮不亮。 */
static void TestCustomBits(void) {
    wprintf(L"[2] 「自定义」位不同、值相同\n");
    const unsigned bits[4] = { SF_SIZE, SF_OFFSETX, SF_OFFSETY, SF_ALPHA };
    const WCHAR* names[4] = { L"尺寸", L"偏移X", L"偏移Y", L"透明度" };

    for (int k = 0; k < 4; k++) {
        ImeCfg a, b;
        CfgDefaults(&a);
        a.st[IMEST_CN_CN].have |= bits[k];
        b = a;
        b.st[IMEST_CN_CN].have &= ~bits[k];   /* 取消勾选：值不变，只清位 */
        CHECK(!SettingsCfgSame(&a, &b), names[k]);
        CHECK(!SettingsCfgSame(&b, &a), L"反向比较同样算改动");
    }

    /* 勾选再取消（回到基线）不算改动。用 cn_en：它默认不物化尺寸位，
       勾选才会加上，取消后回到基线。 */
    ImeCfg a, b;
    CfgDefaults(&a);
    b = a;
    b.st[IMEST_CN_EN].have |= SF_SIZE;
    b.st[IMEST_CN_EN].have &= ~SF_SIZE;
    CHECK(SettingsCfgSame(&a, &b), L"勾选再取消回到基线不算改动");
}

/* ---------- 3. 值不同 ---------- */
static void TestValueDiff(void) {
    wprintf(L"[3] 值不同\n");
    ImeCfg a, b;
    CfgDefaults(&a);
    b = a;
    b.st[IMEST_CN_CN].size = a.st[IMEST_CN_CN].size + 1;
    CHECK(!SettingsCfgSame(&a, &b), L"尺寸值不同算改动");

    b = a;
    b.pollMs = a.pollMs + 1;
    CHECK(!SettingsCfgSame(&a, &b), L"全局项不同算改动");

    b = a;
    b.ignoreCount = 1;
    lstrcpynW(b.ignore[0], L"game.exe", CFG_NAME_MAX);
    CHECK(!SettingsCfgSame(&a, &b), L"黑名单不同算改动");
}

/* ---------- 4. CommitForm 补的位不算改动 ---------- */
/* 打开设置时 CommitForm 会无条件给当前页的状态补 SF_ENABLED；这个位不参与
 * 比较，否则一打开设置就显示「有未保存改动」。 */
static void TestCommitFormBits(void) {
    wprintf(L"[4] CommitForm 补的位不算改动\n");
    ImeCfg a, b;
    CfgDefaults(&a);
    b = a;
    b.st[IMEST_CN_CAPS].have |= SF_ENABLED;   /* 继承中的 caps 状态基线 have=0 */
    CHECK(SettingsCfgSame(&a, &b), L"补 SF_ENABLED 不算改动");
}

/* ---------- 5. 继承关系 ---------- */
static void TestInherit(void) {
    wprintf(L"[5] 继承关系\n");
    ImeCfg a, b;
    CfgDefaults(&a);
    b = a;
    b.st[IMEST_CN_CAPS].inherit = -1;
    CHECK(!SettingsCfgSame(&a, &b), L"取消跟随基础状态算改动");
}

int wmain(void) {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);

    TestSame();
    TestCustomBits();
    TestValueDiff();
    TestCommitFormBits();
    TestInherit();

    wprintf(L"\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) { wprintf(L"=== FAILED ===\n"); return 1; }
    wprintf(L"=== OK ===\n");
    return 0;
}
