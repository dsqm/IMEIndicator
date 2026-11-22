/* config_test.c —— 配置层的单元测试（控制台程序）
 *
 * 只测最容易出错、又不需要真机交互的部分：
 *   1. JSON 往返：写 -> 读 -> 值不变
 *   2. 缺字段走 cn_cn（尺寸/偏移/透明度）或 defaults
 *   3. 继承：caps 状态跟随基础状态；取消跟随后用自己的值
 *   4. 继承成环不死循环
 *   5. 旧 ini 迁移映射（含"色值 0 = 不显示"）
 *   6. 黑名单匹配（大小写、.exe 后缀可选）
 *   7. 加载后未设字段按求值结果补齐（打开设置不该一上来就是"有改动"）
 *   8. 尺寸/偏移/透明度默认跟随中文输入法(cn_cn)，「自定义」后不再跟
 *   9. 旧 ini 的 AutoHideStates 位掩码展开（Caps/KbdEn 一位映射多状态）
 *
 * 用 exe 同目录的临时文件跑，不碰真实配置 —— 测试可反复执行。
 */
#include "../src/ime_indicator.h"
#include "../src/json.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <fcntl.h>
#include <io.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; wprintf(L"  [FAIL] %s (line %d)\n", msg, __LINE__); } \
} while (0)

#define CHECK_INT(a, b, msg) do { \
    int _a = (a), _b = (b); \
    if (_a == _b) { g_pass++; } \
    else { g_fail++; wprintf(L"  [FAIL] %s: got %d, want %d (line %d)\n", msg, _a, _b, __LINE__); } \
} while (0)

/* ---------- 1. JSON 往返 ---------- */
static void TestRoundTrip(void) {
    wprintf(L"[1] JSON 往返\n");
    ImeCfg a, b;
    CfgDefaults(&a);

    a.pollMs = 42;
    a.trackMs = 7;
    a.imeStrategy = 2;
    a.caretTimeoutMs = 321;
    a.hideFullscreen = 0;
    a.hideComposition = 0;
    a.wpsCom = 0;
    a.autoHideMs = 2500;
    a.settingsWidth = 1000;

    a.def.size = 33;
    a.def.offsetX = -7;
    a.def.offsetY = 9;
    a.def.alpha = 200;
    a.def.shape = SHAPE_TRIANGLE;
    /* defaults 段的 enabled/color/image 也要能往返：CfgSave 曾经只写
       size/offset/alpha/shape，读进来的这三项保存即丢 */
    a.def.enabled = 0;
    a.def.have |= SF_ENABLED;
    a.def.rgb = 0x445566;
    a.def.have |= SF_COLOR;
    wcscpy_s(a.def.image, CFG_NAME_MAX, L"dot.png");
    a.def.have |= SF_IMAGE;

    a.st[IMEST_CN_EN].rgb = 0x123456;
    a.st[IMEST_CN_EN].have |= SF_COLOR;
    a.st[IMEST_CN_EN].size = 44;
    a.st[IMEST_CN_EN].have |= SF_SIZE;
    a.st[IMEST_CN_EN].shape = SHAPE_NONE;
    a.st[IMEST_CN_EN].have |= SF_SHAPE;
    a.st[IMEST_CN_EN].enabled = 0;
    a.st[IMEST_CN_EN].have |= SF_ENABLED;
    a.st[IMEST_KR].transient = 1;
    a.st[IMEST_KR].have |= SF_TRANSIENT;
    a.st[IMEST_US_CAPS].inherit = -1;
    a.st[IMEST_US_CAPS].rgb = 0xABCDEF;
    a.st[IMEST_US_CAPS].have |= SF_COLOR;

    a.ignoreCount = 2;
    wcscpy_s(a.ignore[0], CFG_NAME_MAX, L"somegame");
    wcscpy_s(a.ignore[1], CFG_NAME_MAX, L"obs64");

    /* 直接测 JSON 层：拼 -> 解析 */
    {
        Jw w;
        JwInit(&w);
        JwRaw(&w, "{\"a\": 1, \"b\": [\"x\", \"y\"], \"c\": {\"d\": true}}");
        Jv* v = JsonParse(w.p, (int)w.len);
        JwFree(&w);
        CHECK(v != NULL, L"JSON 解析成功");
        if (v) {
            CHECK_INT(JsonInt(v, L"a", 0), 1, L"整数");
            Jv* arr = JsonGet(v, L"b");
            CHECK(arr && arr->type == JV_ARR && arr->count == 2, L"数组长度");
            if (arr && arr->count == 2)
                CHECK(wcscmp(arr->items[1]->s, L"y") == 0, L"数组元素");
            Jv* c = JsonGet(v, L"c");
            CHECK(c && JsonBool(c, L"d", 0) == 1, L"嵌套布尔");
            JsonFree(v);
        }
    }

    /* 落盘再读回 */
    CHECK(CfgSave(&a) == 1, L"CfgSave 成功");
    CfgDefaults(&b);
    /* 直接用 CfgLoad 会走真实路径，这里复用它（测试就在 exe 目录跑） */
    CfgLoad(&b);

    CHECK_INT(b.pollMs, 42, L"pollMs 往返");
    CHECK_INT(b.trackMs, 7, L"trackMs 往返");
    CHECK_INT(b.imeStrategy, 2, L"imeStrategy 往返");
    CHECK_INT(b.caretTimeoutMs, 321, L"caretTimeoutMs 往返");
    CHECK_INT(b.hideFullscreen, 0, L"hideFullscreen 往返");
    CHECK_INT(b.hideComposition, 0, L"hideComposition 往返");
    CHECK_INT(b.wpsCom, 0, L"wpsCom 往返");
    CHECK_INT(b.autoHideMs, 2500, L"autoHideMs 往返");
    CHECK_INT(b.settingsWidth, 1000, L"设置窗口宽度往返");
    CHECK_INT(b.def.size, 33, L"def.size 往返");
    CHECK_INT(b.def.offsetX, -7, L"def.offsetX 往返");
    CHECK_INT(b.def.alpha, 200, L"def.alpha 往返");
    CHECK_INT(b.def.shape, SHAPE_TRIANGLE, L"def.shape 往返");
    CHECK_INT(b.def.enabled, 0, L"def.enabled 往返");
    CHECK(b.def.rgb == 0x445566, L"def.color 往返");
    CHECK(wcscmp(b.def.image, L"dot.png") == 0, L"def.image 往返");
    CHECK(b.st[IMEST_CN_EN].rgb == 0x123456, L"状态颜色往返");
    CHECK_INT(b.st[IMEST_CN_EN].size, 44, L"状态尺寸往返");
    CHECK_INT(b.st[IMEST_CN_EN].shape, SHAPE_NONE, L"状态形状往返");
    CHECK_INT(b.st[IMEST_CN_EN].enabled, 0, L"状态开关往返");
    CHECK_INT(b.st[IMEST_KR].transient, 1, L"状态临时显示往返");
    CHECK(b.st[IMEST_US_CAPS].rgb == 0xABCDEF, L"caps 显式色往返");
    CHECK_INT(b.st[IMEST_US_CAPS].inherit, -1, L"caps 继承断开往返");
    CHECK_INT(b.ignoreCount, 2, L"黑名单数量往返");
    CHECK(wcscmp(b.ignore[0], L"somegame") == 0, L"黑名单内容往返");
}

/* ---------- 2. 缺字段走 cn_cn / defaults ---------- */
static void TestDefaults(void) {
    wprintf(L"[2] 缺字段走 cn_cn / defaults\n");
    ImeCfg c;
    CfgDefaults(&c);
    c.st[IMEST_CN_CN].size = 33;    c.st[IMEST_CN_CN].have |= SF_SIZE;
    c.st[IMEST_CN_CN].offsetX = -9; c.st[IMEST_CN_CN].have |= SF_OFFSETX;
    c.st[IMEST_CN_CN].alpha = 180;  c.st[IMEST_CN_CN].have |= SF_ALPHA;
    /* 模拟一份只有 color 的状态 */
    c.st[IMEST_JP].have = SF_COLOR;
    c.st[IMEST_JP].rgb = 0x111111;
    StateStyle s;
    CfgStateStyle(&c, IMEST_JP, &s);
    CHECK(s.rgb == 0x111111, L"显式颜色生效");
    CHECK_INT(s.size, 33, L"未设尺寸跟随中文输入法");
    CHECK_INT(s.offsetX, -9, L"未设偏移跟随中文输入法");
    CHECK_INT(s.alpha, 180, L"未设透明度跟随中文输入法");
    CHECK_INT(s.offsetY, c.st[IMEST_CN_CN].offsetY, L"未设偏移 Y 跟随中文输入法");

    /* cn_cn 是回落来源，自己不吃这一层：没设的字段仍走 defaults */
    c.st[IMEST_CN_CN].have &= ~SF_ALPHA;
    CfgStateStyle(&c, IMEST_CN_CN, &s);
    CHECK_INT(s.alpha, c.def.alpha, L"cn_cn 未设的字段走 defaults");
}

/* ---------- 3. 继承 ---------- */
static void TestInherit(void) {
    wprintf(L"[3] 继承\n");
    ImeCfg c;
    CfgDefaults(&c);

    c.st[IMEST_US].rgb = 0x00FF00;
    c.st[IMEST_US].have = SF_COLOR;
    c.st[IMEST_US].size = 30;
    c.st[IMEST_US].have |= SF_SIZE;
    c.st[IMEST_US].inherit = -1;

    c.st[IMEST_US_CAPS].inherit = IMEST_US;
    c.st[IMEST_US_CAPS].have = 0;   /* 自己不设任何字段 */

    StateStyle s;
    CfgStateStyle(&c, IMEST_US_CAPS, &s);
    CHECK(s.rgb == 0x00FF00, L"继承拿到基础状态颜色");
    CHECK_INT(s.size, 30, L"继承拿到基础状态尺寸");

    /* 自己覆盖一项，其余仍跟基础 */
    c.st[IMEST_US_CAPS].rgb = 0xFF0000;
    c.st[IMEST_US_CAPS].have = SF_COLOR;
    CfgStateStyle(&c, IMEST_US_CAPS, &s);
    CHECK(s.rgb == 0xFF0000, L"显式值压过继承");
    CHECK_INT(s.size, 30, L"未覆盖的字段仍跟基础");

    /* 断开继承 */
    c.st[IMEST_CN_CN].size = 26;
    c.st[IMEST_CN_CN].have |= SF_SIZE;
    c.st[IMEST_US_CAPS].inherit = -1;
    CfgStateStyle(&c, IMEST_US_CAPS, &s);
    CHECK(s.rgb == 0xFF0000, L"断开后保留自己的颜色");
    CHECK_INT(s.size, 26, L"断开后尺寸跟中文输入法，而不是基础状态");
}

/* ---------- 4. 继承成环 ---------- */
static void TestCycle(void) {
    wprintf(L"[4] 继承成环\n");
    ImeCfg c;
    CfgDefaults(&c);
    c.st[IMEST_US].inherit = IMEST_US_CAPS;
    c.st[IMEST_US_CAPS].inherit = IMEST_US;
    c.st[IMEST_US].rgb = 0x0000FF; c.st[IMEST_US].have = SF_COLOR;
    c.st[IMEST_US_CAPS].rgb = 0xFF00FF; c.st[IMEST_US_CAPS].have = SF_COLOR;
    StateStyle s;
    CfgStateStyle(&c, IMEST_US, &s);     /* 不应死循环 */
    CHECK(s.size >= 1 && s.size <= 256, L"成环时样式仍合法");
    CHECK(s.rgb == 0x0000FF || s.rgb == 0xFF00FF, L"成环时取到链上某个值");
}

/* ---------- 5. 黑名单 ---------- */
static void TestIgnore(void) {
    wprintf(L"[5] 黑名单\n");
    ImeCfg c;
    CfgDefaults(&c);
    c.ignoreCount = 2;
    wcscpy_s(c.ignore[0], CFG_NAME_MAX, L"somegame");
    wcscpy_s(c.ignore[1], CFG_NAME_MAX, L"Obs64");
    CHECK(CfgIsIgnored(&c, L"somegame") == 1, L"无后缀命中");
    CHECK(CfgIsIgnored(&c, L"somegame.exe") == 1, L"带后缀命中");
    CHECK(CfgIsIgnored(&c, L"SOMEGAME.EXE") == 1, L"大小写不敏感");
    CHECK(CfgIsIgnored(&c, L"obs64") == 1, L"表内带大写也命中");
    CHECK(CfgIsIgnored(&c, L"somegame2") == 0, L"前缀不误命中");
    CHECK(CfgIsIgnored(&c, L"") == 0, L"空名不命中");
}

/* ---------- 6. 默认配置自洽 ---------- */
static void TestDefaultConfig(void) {
    wprintf(L"[6] 默认配置自洽\n");
    ImeCfg c;
    CfgDefaults(&c);
    /* 每个状态都必须能求出合法样式 */
    for (int i = 0; i < IMEST_COUNT; i++) {
        StateStyle s;
        CfgStateStyle(&c, (ImeState)i, &s);
        if (s.size < 1 || s.size > 256) {
            g_fail++;
            wprintf(L"  [FAIL] 状态 %d 尺寸非法: %d\n", i, s.size);
        } else g_pass++;
        if (s.alpha < 0 || s.alpha > 255) {
            g_fail++;
            wprintf(L"  [FAIL] 状态 %d 透明度非法: %d\n", i, s.alpha);
        } else g_pass++;
        if (s.shape < SHAPE_NONE || s.shape > SHAPE_IMAGE) {
            g_fail++;
            wprintf(L"  [FAIL] 状态 %d 形状非法: %d\n", i, s.shape);
        } else g_pass++;
    }
    /* 默认只有中文态临时显示 */
    CHECK_INT(c.st[IMEST_CN_CN].transient, 1, L"中文态默认临时显示");
    CHECK_INT(c.st[IMEST_CN_EN].transient, 0, L"英文态默认常显");
    /* 默认 caps 跟随基础状态 */
    CHECK(c.st[IMEST_CN_CAPS].inherit == IMEST_CN_CN, L"中文大写默认跟随中文");
    CHECK(c.st[IMEST_US_CAPS].inherit == IMEST_US, L"美式大写默认跟随美式");
    /* 窗口宽度没拖过时是 0（老配置里没有这个字段，读出来也是 0） */
    CHECK_INT(c.settingsWidth, 0, L"设置窗口宽度默认 0");
}

/* ---------- 7. 加载后 raw 与求值结果一致 ---------- */
/* 设置程序回写的是"控件上显示的值"（求值结果）。未显式设过的字段如果 raw
 * 还留着内置初值，打开设置就会被判成有改动 —— 标题带星号、保存按钮亮着。 */
static void TestMaterialize(void) {
    wprintf(L"[7] 未设字段按 defaults 补齐\n");
    ImeCfg a, b;
    CfgDefaults(&a);
    a.def.shape = SHAPE_TRIANGLE;
    a.def.size = 33;
    /* caps 状态默认跟随基础状态；这里取消跟随、只显式设颜色，
       形状落到 defaults 段、尺寸/偏移/透明度落到 cn_cn，而不是 raw 里
       留着的内置初值 */
    a.st[IMEST_US_CAPS].inherit = -1;
    a.st[IMEST_US_CAPS].have = SF_COLOR;
    a.st[IMEST_US_CAPS].rgb = 0x112233;
    a.st[IMEST_US_CAPS].shape = SHAPE_NONE;
    a.st[IMEST_US_CAPS].size = 20;
    CHECK(CfgSave(&a) == 1, L"CfgSave 成功");
    CfgLoad(&b);
    StateStyle eff;
    CfgStateStyle(&b, IMEST_US_CAPS, &eff);
    CHECK_INT(eff.shape, SHAPE_TRIANGLE, L"求值使用 defaults 形状");
    CHECK_INT(b.st[IMEST_US_CAPS].shape, eff.shape, L"未设形状已按求值结果补齐");
    CHECK_INT(b.st[IMEST_US_CAPS].size, eff.size, L"未设尺寸已按求值结果补齐");
    CHECK(b.st[IMEST_US_CAPS].rgb == 0x112233, L"显式颜色不被覆盖");

    /* 跟随中的状态不补齐：它本来就不读自己这几个字段 */
    CfgDefaults(&a);
    a.st[IMEST_US_CAPS].inherit = IMEST_US;
    a.st[IMEST_US_CAPS].have = 0;
    a.st[IMEST_US_CAPS].size = 77;      /* 垃圾值，不该被信任 */
    CHECK(CfgSave(&a) == 1, L"CfgSave 成功（继承）");
    CfgLoad(&b);
    CHECK_INT(b.st[IMEST_US_CAPS].inherit, IMEST_US, L"继承关系保留");
    CHECK_INT(b.st[IMEST_US_CAPS].have, 0, L"跟随中的状态不被物化");
    CfgStateStyle(&b, IMEST_US_CAPS, &eff);
    CHECK(eff.size != 77, L"跟随中的状态不读自己的字段");
}

/* ---------- 8. 尺寸/偏移/透明度跟随中文输入法 ---------- */
/* 这四项没显式设过时回落到 cn_cn（而不是 defaults）：设置里改一次 cn_cn 的滑块，
 * 所有状态一起变；某一项想单独调就勾「自定义」，那就是显式值。 */
static void TestFollowCn(void) {
    wprintf(L"[8] 尺寸/偏移/透明度跟随中文输入法\n");
    ImeCfg c;
    CfgDefaults(&c);

    /* 全新配置：除 cn_cn 自己与整块继承的状态外，都没物化这四项 */
    for (int i = 0; i < IMEST_COUNT; i++) {
        if ((ImeState)i == IMEST_CN_CN || c.st[i].inherit >= 0) continue;
        CHECK(CfgFieldFollowsCn(&c, (ImeState)i, SF_SIZE) == 1, L"全新配置默认跟随");
    }
    CHECK(CfgFieldFollowsCn(&c, IMEST_CN_CN, SF_SIZE) == 0, L"cn_cn 自己不算跟随");

    /* 改 cn_cn，别的状态跟着变 */
    c.st[IMEST_CN_CN].size = 42;  c.st[IMEST_CN_CN].have |= SF_SIZE;
    c.st[IMEST_CN_CN].alpha = 99; c.st[IMEST_CN_CN].have |= SF_ALPHA;
    StateStyle us, jp;
    CfgStateStyle(&c, IMEST_US, &us);
    CfgStateStyle(&c, IMEST_JP, &jp);
    CHECK_INT(us.size, 42, L"美式跟随 cn_cn 尺寸");
    CHECK_INT(jp.alpha, 99, L"日文跟随 cn_cn 透明度");

    /* 勾「自定义」= 显式设过，不再跟 */
    c.st[IMEST_US].size = 12;
    c.st[IMEST_US].have |= SF_SIZE;
    CfgStateStyle(&c, IMEST_US, &us);
    CHECK_INT(us.size, 12, L"自定义后用自己的值");
    CHECK(CfgFieldFollowsCn(&c, IMEST_US, SF_SIZE) == 0, L"显式设过就不算跟随");
    CHECK(CfgFieldFollowsCn(&c, IMEST_US, SF_ALPHA) == 1, L"同状态其它项仍跟随");

    /* 整块继承优先于 cn_cn 回落 */
    c.st[IMEST_US_CAPS].inherit = IMEST_US;
    c.st[IMEST_US_CAPS].have = 0;
    StateStyle caps;
    CfgStateStyle(&c, IMEST_US_CAPS, &caps);
    CHECK_INT(caps.size, 12, L"整块继承优先于跟随 cn_cn");
    CHECK(CfgFieldFollowsCn(&c, IMEST_US_CAPS, SF_SIZE) == 0,
          L"整块继承时不算跟随 cn_cn");

    /* 落盘：跟随的项不写显式值，读回后还在跟随 */
    CHECK(CfgSave(&c) == 1, L"CfgSave 成功");
    ImeCfg d;
    CfgLoad(&d);
    CHECK_INT((int)(d.st[IMEST_JP].have & SF_SIZE), 0, L"跟随项不写出显式值");
    CHECK(CfgFieldFollowsCn(&d, IMEST_JP, SF_SIZE) == 1, L"读回后仍在跟随");
    StateStyle jp2;
    CfgStateStyle(&d, IMEST_JP, &jp2);
    CHECK_INT(jp2.size, 42, L"读回后仍取 cn_cn 的尺寸");
}

/* ---------- 9. 旧 ini 迁移 ---------- */
/* 旧 ini 的 Size/Shape/OffsetX/OffsetY/Alpha 是全局值；迁移后必须落到
 * defaults 与各状态的求值结果里，不能被 CfgDefaults 物化的内置值盖住。 */
static void TestMigrateOldIni(void) {
    wprintf(L"[9] 旧 ini 迁移\n");
    WCHAR json[MAX_PATH], ini[MAX_PATH], bak[MAX_PATH];
    CfgPath(json, MAX_PATH);
    CfgIniPath(ini, MAX_PATH);
    _snwprintf_s(bak, MAX_PATH, _TRUNCATE, L"%s.bak", ini);
    DeleteFileW(json);
    DeleteFileW(bak);

    const char* text =
        "[General]\n"
        "AutoHideMs = 1000\n"
        "AutoHideStates = Cn\n"
        "[Colors]\n"
        "En = #FF0000\n"
        "Cn = #00A000\n"
        "Alpha = 200\n"
        "[Overlay]\n"
        "Size = 30\n"
        "Shape = triangle\n"
        "OffsetX = 5\n"
        "OffsetY = -3\n";
    FILE* f = NULL;
    CHECK(_wfopen_s(&f, ini, L"wb") == 0 && f != NULL, L"写入旧 ini");
    if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }

    /* 旧版是物理像素，迁移按系统 DPI 折算成逻辑像素 */
    UINT dpi = CfgSystemDpi();
    int expSize = CfgOldPxToLogical(30, dpi);
    int expOffX = CfgOldPxToLogical(5, dpi);
    int expOffY = CfgOldPxToLogical(-3, dpi);

    ImeCfg c;
    CfgLoad(&c);
    CHECK_INT(c.def.size, expSize, L"defaults 尺寸迁移（按 DPI 折算）");
    CHECK_INT(c.def.shape, SHAPE_TRIANGLE, L"defaults 形状迁移");
    CHECK_INT(c.def.offsetX, expOffX, L"defaults 偏移 X 迁移（按 DPI 折算）");
    CHECK_INT(c.def.offsetY, expOffY, L"defaults 偏移 Y 迁移（按 DPI 折算）");
    CHECK_INT(c.def.alpha, 200, L"defaults 透明度迁移");

    StateStyle us, cn;
    CfgStateStyle(&c, IMEST_US, &us);
    CfgStateStyle(&c, IMEST_CN_CN, &cn);
    CHECK_INT(us.size, expSize, L"美式尺寸取迁移值");
    CHECK_INT(us.shape, SHAPE_TRIANGLE, L"美式形状取迁移值");
    CHECK_INT(us.offsetX, expOffX, L"美式偏移 X 取迁移值");
    CHECK_INT(us.offsetY, expOffY, L"美式偏移 Y 取迁移值");
    CHECK_INT(us.alpha, 200, L"美式透明度取迁移值");
    CHECK_INT(cn.size, expSize, L"中文尺寸取迁移值");
    CHECK_INT(cn.shape, SHAPE_TRIANGLE, L"中文形状取迁移值");

    /* 折算公式本身：96 DPI 不变，高 DPI 按比例缩小 */
    CHECK_INT(CfgOldPxToLogical(20, 96), 20, L"96 DPI 不缩放");
    CHECK_INT(CfgOldPxToLogical(20, 144), 13, L"150% 屏折算");
    CHECK_INT(CfgOldPxToLogical(20, 192), 10, L"200% 屏折算");
    CHECK_INT(CfgOldPxToLogical(-2, 144), -1, L"负偏移折算");

    /* 迁移后旧 ini 改名保留，JSON 已生成 */
    CHECK(GetFileAttributesW(bak) != INVALID_FILE_ATTRIBUTES, L"旧 ini 备份为 .bak");
    CHECK(GetFileAttributesW(json) != INVALID_FILE_ATTRIBUTES, L"已生成 JSON");
    DeleteFileW(bak);
    DeleteFileW(json);
}

/* ---------- 10. 旧 ini 的 AutoHideStates 位掩码展开 ---------- */
/* 旧 Caps 是盖掉一切的单一状态，旧 KbdEn 同时代表"其他布局"：迁移时一个位
 * 要展开到多个新状态，否则 cn_caps/jp_caps/kr_caps/other_caps 会漏掉。 */
static void TestMigrateAutoHideStates(void) {
    wprintf(L"[10] 旧 ini AutoHideStates 位掩码展开\n");
    WCHAR json[MAX_PATH], ini[MAX_PATH], bak[MAX_PATH];
    CfgPath(json, MAX_PATH);
    CfgIniPath(ini, MAX_PATH);
    _snwprintf_s(bak, MAX_PATH, _TRUNCATE, L"%s.bak", ini);
    DeleteFileW(json);
    DeleteFileW(bak);

    const char* text =
        "[General]\n"
        "AutoHideStates = Caps,KbdEn\n";
    FILE* f = NULL;
    CHECK(_wfopen_s(&f, ini, L"wb") == 0 && f != NULL, L"写入旧 ini（AutoHideStates）");
    if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }

    ImeCfg c;
    CfgLoad(&c);
    /* Caps 位覆盖全部五个 *_caps */
    CHECK_INT(c.st[IMEST_US_CAPS].transient, 1, L"us_caps 临时显示");
    CHECK_INT(c.st[IMEST_CN_CAPS].transient, 1, L"cn_caps 临时显示");
    CHECK_INT(c.st[IMEST_JP_CAPS].transient, 1, L"jp_caps 临时显示");
    CHECK_INT(c.st[IMEST_KR_CAPS].transient, 1, L"kr_caps 临时显示");
    CHECK_INT(c.st[IMEST_OTHER_CAPS].transient, 1, L"other_caps 临时显示");
    /* KbdEn 位同时覆盖 us 与 other */
    CHECK_INT(c.st[IMEST_US].transient, 1, L"us 临时显示");
    CHECK_INT(c.st[IMEST_OTHER].transient, 1, L"other 临时显示");
    /* 没在掩码里的状态不受影响（jp 默认常显） */
    CHECK_INT(c.st[IMEST_JP].transient, 0, L"未在掩码里的状态不受影响");

    DeleteFileW(bak);
    DeleteFileW(json);
}

int wmain(void) {
    /* 控制台按 UTF-8 输出：重定向到文件时也是 UTF-8，CI 日志里中文不乱码 */
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    wprintf(L"=== config_test ===\n");
    /* 测试会把配置写进"本 exe 所在目录"，所以必须跑在 bin\test\ 这种
       独立目录里 —— 别把测试 exe 放到主程序旁边，会覆盖真实配置。 */
    { WCHAR p[MAX_PATH]; CfgPath(p, MAX_PATH); wprintf(L"配置路径: %s\n", p); }
    TestRoundTrip();
    TestDefaults();
    TestInherit();
    TestCycle();
    TestIgnore();
    TestDefaultConfig();
    TestMaterialize();
    TestFollowCn();
    TestMigrateOldIni();
    TestMigrateAutoHideStates();

    wprintf(L"\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) { wprintf(L"=== FAILED ===\n"); return 1; }
    wprintf(L"=== OK ===\n");
    return 0;
}
