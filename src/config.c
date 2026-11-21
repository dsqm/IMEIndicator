#include "ime_indicator.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ================= 配置模块（IMEIndicator.json，与 exe 同目录） =================
   格式：UTF-8 JSON，带 version 字段。**不需要手动编辑** —— 设置程序负责读写，
   这里只保证"读不懂的字段忽略、缺的字段走默认"，让新旧版本能互相容忍。

   结构：
     version   1
     general   { pollIntervalMs, trackIntervalMs, imeStrategy, caretTimeoutMs,
                 hideWhenFullscreen, hideWhenComposition, wpsCom, autoHideMs }
     defaults  { size, offsetX, offsetY, alpha, shape }
     states    { us: {...}, us_caps: {...}, ... }   每状态字段全部可选
     ignore    [ "somegame.exe", ... ]

   继承：状态里写 "inherit": "us" 表示"没显式设过的字段都跟 us 走"。
   求值顺序：状态显式值 > inherit 目标 > defaults。
   用 SF_* 位掩码记录"显式设过"，否则无法区分"用户就是要 20"和"默认是 20"。

   旧版 IMEIndicator.ini 在首次运行时自动迁移，迁移后改名为 .ini.bak。 */

#define CFG_VERSION 1

/* ---------- 默认值 ---------- */

/* 默认配色沿用旧版语义：
   - 美式键盘 = 旧 KbdEn（紫）
   - 中文输入法中文 = 旧 Cn（绿，且默认临时显示）
   - 中文输入法英文 = 旧 En（红）
   - 日/韩 = 旧 Jp/Kr（黑）
   - 所有大写态 = 旧 Caps（蓝）
   - 其他布局 = 旧 KbdEn（紫），因为旧版非中英日韩布局最终落到英文色 */
static const struct { ImeState st; DWORD rgb; int transient; } kDefColors[] = {
    { IMEST_US,         0x8B00FF, 0 },
    { IMEST_US_CAPS,    0x0080FF, 0 },
    { IMEST_CN_CN,      0x00A000, 1 },
    { IMEST_CN_EN,      0xFF0000, 0 },
    { IMEST_CN_CAPS,    0x0080FF, 0 },
    { IMEST_JP,         0x000000, 0 },
    { IMEST_JP_CAPS,    0x0080FF, 0 },
    { IMEST_KR,         0x000000, 0 },
    { IMEST_KR_CAPS,    0x0080FF, 0 },
    { IMEST_OTHER,      0x8B00FF, 0 },
    { IMEST_OTHER_CAPS, 0x0080FF, 0 },
};

/* 状态键名（JSON 里用）。顺序必须与 ImeState 一致。 */
static const WCHAR* kStateNames[IMEST_COUNT] = {
    L"us", L"us_caps",
    L"cn_cn", L"cn_en", L"cn_caps",
    L"jp", L"jp_caps",
    L"kr", L"kr_caps",
    L"other", L"other_caps",
};

/* 默认继承关系：大写态默认跟随同布局的普通态。
   旧版 Caps 是"盖掉一切"的独立状态，迁移时会把旧 Caps 色显式写进各 *_caps，
   所以这里只影响全新安装。 */
static const int kDefInherit[IMEST_COUNT] = {
    -1, IMEST_US,
    -1, -1, IMEST_CN_CN,
    -1, IMEST_JP,
    -1, IMEST_KR,
    -1, IMEST_OTHER,
};

const WCHAR* CfgStateName(ImeState s) {
    if (s < 0 || s >= IMEST_COUNT) return L"?";
    return kStateNames[s];
}

int CfgStateFromName(const WCHAR* name) {
    if (!name) return -1;
    for (int i = 0; i < IMEST_COUNT; i++)
        if (_wcsicmp(name, kStateNames[i]) == 0) return i;
    return -1;
}

const WCHAR* CfgShapeName(int shape) {
    switch (shape) {
    case SHAPE_NONE:     return L"none";
    case SHAPE_TRIANGLE: return L"triangle";
    case SHAPE_IMAGE:    return L"image";
    default:             return L"circle";
    }
}

int CfgShapeFromName(const WCHAR* name, int def) {
    if (!name) return def;
    if (_wcsicmp(name, L"none") == 0)     return SHAPE_NONE;
    if (_wcsicmp(name, L"circle") == 0)   return SHAPE_CIRCLE;
    if (_wcsicmp(name, L"triangle") == 0) return SHAPE_TRIANGLE;
    if (_wcsicmp(name, L"image") == 0)    return SHAPE_IMAGE;
    return def;
}

int CfgImageNameValid(const WCHAR* name) {
    if (!name || !name[0]) return 0;
    if (wcslen(name) >= CFG_NAME_MAX) return 0;
    if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) return 0;
    for (const WCHAR* p = name; *p; p++) {
        switch (*p) {
        case L'\\': case L'/': case L':': case L'*': case L'?':
        case L'"':  case L'<': case L'>': case L'|':
            return 0;
        }
    }
    return 1;
}

/* ---------- 路径 ---------- */

void CfgDir(WCHAR* out, size_t cap) {
    WCHAR exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (exe[i] == L'\\') slash = &exe[i];
    if (slash) *slash = 0;
    _snwprintf_s(out, cap, _TRUNCATE, L"%s", exe);
}

void CfgPath(WCHAR* out, size_t cap) {
    WCHAR dir[MAX_PATH];
    CfgDir(dir, MAX_PATH);
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\IMEIndicator.json", dir);
}

static void IniPath(WCHAR* out, size_t cap) {
    WCHAR dir[MAX_PATH];
    CfgDir(dir, MAX_PATH);
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\IMEIndicator.ini", dir);
}

/* ---------- 默认配置 ---------- */

void CfgDefaults(ImeCfg* c) {
    ZeroMemory(c, sizeof(*c));
    c->pollMs = 100;
    c->trackMs = 15;
    c->imeStrategy = 0;
    c->caretTimeoutMs = 150;
    c->hideFullscreen = 1;
    c->hideComposition = 1;
    c->wpsCom = 1;
    c->autoHideMs = 1000;

    /* defaults：未覆盖字段的兜底 */
    c->def.have = SF_SIZE | SF_OFFSETX | SF_OFFSETY | SF_ALPHA | SF_SHAPE;
    c->def.enabled = 1;
    c->def.rgb = 0xFF0000;
    c->def.shape = SHAPE_CIRCLE;
    c->def.size = 20;
    c->def.offsetX = -2;
    c->def.offsetY = 4;
    c->def.alpha = 255;
    c->def.transient = 0;
    c->def.inherit = -1;

    for (int i = 0; i < IMEST_COUNT; i++) {
        StateStyle* s = &c->st[i];
        ZeroMemory(s, sizeof(*s));
        s->inherit = kDefInherit[i];
        s->enabled = 1;
        s->alpha = 255;
        s->size = 20;
        s->offsetX = -2;
        s->offsetY = 4;
        s->shape = SHAPE_CIRCLE;
        for (size_t k = 0; k < sizeof(kDefColors) / sizeof(kDefColors[0]); k++) {
            if (kDefColors[k].st == (ImeState)i) {
                s->rgb = kDefColors[k].rgb;
                s->transient = kDefColors[k].transient;
                break;
            }
        }
        /* 不继承的状态把默认值**物化**进 have：这样首次保存写出的是一份
           完整快照，将来改内置默认色不会静默影响老用户。
           继承中的状态保持 have=0，让基础状态说了算。 */
        if (s->inherit < 0)
            s->have = SF_ENABLED | SF_COLOR | SF_SHAPE | SF_SIZE |
                      SF_OFFSETX | SF_OFFSETY | SF_ALPHA | SF_TRANSIENT;
        else
            s->have = 0;
    }
    c->ignoreCount = 0;
}

/* ---------- 继承求值 ---------- */

/* 把 src 里"有"的字段覆盖到 dst 上（dst 已有的不覆盖 —— 调用方按优先级从低到高叠） */
static void OverlayStyle(StateStyle* dst, const StateStyle* src) {
    unsigned h = src->have;
    if (h & SF_ENABLED)   dst->enabled   = src->enabled;
    if (h & SF_COLOR)     dst->rgb       = src->rgb;
    if (h & SF_SHAPE)     dst->shape     = src->shape;
    if (h & SF_IMAGE)     lstrcpynW(dst->image, src->image, CFG_NAME_MAX);
    if (h & SF_SIZE)      dst->size      = src->size;
    if (h & SF_OFFSETX)   dst->offsetX   = src->offsetX;
    if (h & SF_OFFSETY)   dst->offsetY   = src->offsetY;
    if (h & SF_ALPHA)     dst->alpha     = src->alpha;
    if (h & SF_TRANSIENT) dst->transient = src->transient;
}

void CfgStateStyle(const ImeCfg* c, ImeState s, StateStyle* out) {
    if (!c || !out) return;
    if (s < 0 || s >= IMEST_COUNT) s = IMEST_US;

    /* 从 defaults 起底，再沿继承链往上叠，最后叠自己。
       链长上限 = IMEST_COUNT，防止 inherit 成环时死循环。 */
    StateStyle acc = c->def;
    acc.have = 0xFFFFFFFFu;   /* 求值结果视为"全都有" */

    /* 收集继承链（从最远祖先到最近），最多 IMEST_COUNT 层 */
    int chain[IMEST_COUNT];
    int n = 0;
    int cur = c->st[s].inherit;
    while (cur >= 0 && cur < IMEST_COUNT && n < IMEST_COUNT) {
        int dup = 0;
        for (int i = 0; i < n; i++) if (chain[i] == cur) { dup = 1; break; }
        if (dup) break;                 /* 成环：就此打住 */
        chain[n++] = cur;
        cur = c->st[cur].inherit;
    }
    /* 反向叠：最远的祖先先叠 */
    for (int i = n - 1; i >= 0; i--) OverlayStyle(&acc, &c->st[chain[i]]);
    OverlayStyle(&acc, &c->st[s]);

    /* 兜底：字段值本身非法时回落到 defaults */
    if (acc.size < 1 || acc.size > 256) acc.size = c->def.size;
    if (acc.alpha < 0) acc.alpha = 0;
    if (acc.alpha > 255) acc.alpha = 255;
    if (acc.offsetX < -2048 || acc.offsetX > 2048) acc.offsetX = c->def.offsetX;
    if (acc.offsetY < -2048 || acc.offsetY > 2048) acc.offsetY = c->def.offsetY;
    if (acc.shape < SHAPE_NONE || acc.shape > SHAPE_IMAGE) acc.shape = SHAPE_CIRCLE;
    if (!CfgImageNameValid(acc.image)) acc.image[0] = 0;
    acc.inherit = -1;
    *out = acc;
}

/* 把"没显式设过"的字段按求值结果补齐。
 *
 * 为什么要补：raw 里这些字段留的是**内置初值**，而求值时用的是 defaults 段，
 * 两者可以不同（defaults.shape=triangle 而某状态没设 shape 时，raw 还是
 * circle）。设置程序回写的是控件上显示的那个值 —— 求值结果，所以不补的话
 * 一打开设置就被判成"有未保存的修改"，标题带星号、保存按钮亮着。
 *
 * 只补 inherit < 0 的状态：跟随中的状态本来就不看自己这几个字段。 */
static void MaterializeStates(ImeCfg* c) {
    for (int i = 0; i < IMEST_COUNT; i++) {
        StateStyle* s = &c->st[i];
        if (s->inherit >= 0) continue;
        StateStyle eff;
        CfgStateStyle(c, (ImeState)i, &eff);
        if (!(s->have & SF_ENABLED))   s->enabled   = eff.enabled;
        if (!(s->have & SF_COLOR))     s->rgb       = eff.rgb;
        if (!(s->have & SF_SHAPE))     s->shape     = eff.shape;
        if (!(s->have & SF_IMAGE))     lstrcpynW(s->image, eff.image, CFG_NAME_MAX);
        if (!(s->have & SF_SIZE))      s->size      = eff.size;
        if (!(s->have & SF_OFFSETX))   s->offsetX   = eff.offsetX;
        if (!(s->have & SF_OFFSETY))   s->offsetY   = eff.offsetY;
        if (!(s->have & SF_ALPHA))     s->alpha     = eff.alpha;
        if (!(s->have & SF_TRANSIENT)) s->transient = eff.transient;
    }
}

/* ---------- 颜色解析 ---------- */

static int HexV(WCHAR c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

/* "#RRGGBB" -> 0x00RRGGBB。失败返回 -1。 */
static int ParseColor(const WCHAR* v, DWORD* out) {
    if (!v) return 0;
    while (*v == L' ' || *v == L'\t') v++;
    if (*v != L'#') return 0;
    v++;
    DWORD rgb = 0;
    for (int i = 0; i < 6; i++) {
        int d = HexV(v[i]);
        if (d < 0) return 0;
        rgb = (rgb << 4) | (DWORD)d;
    }
    /* 后面只允许空白或结束 */
    const WCHAR* t = v + 6;
    while (*t == L' ' || *t == L'\t') t++;
    if (*t) return 0;
    *out = rgb;
    return 1;
}

static void FormatColor(DWORD rgb, WCHAR* out, size_t cap) {
    _snwprintf_s(out, cap, _TRUNCATE, L"#%02X%02X%02X",
                 (unsigned)((rgb >> 16) & 0xFF),
                 (unsigned)((rgb >> 8) & 0xFF),
                 (unsigned)(rgb & 0xFF));
}

/* ---------- 读一个状态 ---------- */

static void ReadState(const Jv* o, StateStyle* s) {
    if (!o || o->type != JV_OBJ) return;

    const Jv* v;
    if ((v = JsonGet(o, L"enabled")) != NULL) {
        s->enabled = JsonBool(o, L"enabled", 1);
        s->have |= SF_ENABLED;
    }
    if ((v = JsonGet(o, L"color")) != NULL) {
        DWORD rgb;
        if (ParseColor(JsonStr(o, L"color"), &rgb)) {
            s->rgb = rgb;
            s->have |= SF_COLOR;
        }
    }
    if ((v = JsonGet(o, L"shape")) != NULL) {
        s->shape = CfgShapeFromName(JsonStr(o, L"shape"), s->shape);
        s->have |= SF_SHAPE;
    }
    if ((v = JsonGet(o, L"image")) != NULL) {
        const WCHAR* nm = JsonStr(o, L"image");
        if (CfgImageNameValid(nm)) {
            lstrcpynW(s->image, nm, CFG_NAME_MAX);
            s->have |= SF_IMAGE;
        }
    }
    if ((v = JsonGet(o, L"size")) != NULL) {
        s->size = JsonInt(o, L"size", s->size);
        s->have |= SF_SIZE;
    }
    if ((v = JsonGet(o, L"offsetX")) != NULL) {
        s->offsetX = JsonInt(o, L"offsetX", s->offsetX);
        s->have |= SF_OFFSETX;
    }
    if ((v = JsonGet(o, L"offsetY")) != NULL) {
        s->offsetY = JsonInt(o, L"offsetY", s->offsetY);
        s->have |= SF_OFFSETY;
    }
    if ((v = JsonGet(o, L"alpha")) != NULL) {
        s->alpha = JsonInt(o, L"alpha", s->alpha);
        s->have |= SF_ALPHA;
    }
    if ((v = JsonGet(o, L"transient")) != NULL) {
        s->transient = JsonBool(o, L"transient", 0);
        s->have |= SF_TRANSIENT;
    }
    if ((v = JsonGet(o, L"inherit")) != NULL) {
        const WCHAR* nm = JsonStr(o, L"inherit");
        if (nm) {
            int idx = CfgStateFromName(nm);
            s->inherit = (idx >= 0) ? idx : -1;
        } else {
            s->inherit = -1;
        }
    }
}

/* ---------- 旧 ini 迁移 ---------- */

/* 旧版 ini 的解析：只认迁移需要的键。返回 1 表示读到了文件。 */
typedef struct {
    int   have;
    DWORD en, caps, kbden, cn, jp, kr;
    int   alpha;
    int   size, shape, offX, offY;
    int   pollMs, trackMs, imeStrategy, hideFullscreen, hideComposition;
    int   caretTimeoutMs, autoHideMs, wpsCom;
    int   ahMask;          /* 旧 AutoHideStates 位掩码（按旧 ImeState 位） */
    int   ahHave;
    WCHAR ignore[CFG_MAX_IGNORE][CFG_NAME_MAX];
    int   ignoreCount;
} OldIni;

/* 旧状态位：EN=0, CAPS=1, KBD_EN=2, CN=3, JP=4, KR=5 */
#define OLD_EN     (1u << 0)
#define OLD_CAPS   (1u << 1)
#define OLD_KBDEN  (1u << 2)
#define OLD_CN     (1u << 3)
#define OLD_JP     (1u << 4)
#define OLD_KR     (1u << 5)

static int OldParseInt(const WCHAR* v, int def, int lo, int hi) {
    if (!v || !v[0]) return def;
    const WCHAR* p = v;
    int neg = 0;
    if (*p == L'-') { neg = 1; p++; }
    else if (*p == L'+') p++;
    long long n = 0; int any = 0;
    for (; *p; p++) {
        if (*p < L'0' || *p > L'9') break;
        n = n * 10 + (*p - L'0'); any = 1;
        if (n > 100000000LL) return def;
    }
    if (!any) return def;
    if (neg) n = -n;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return (int)n;
}

/* 旧色值：`0` = 该状态不显示（返回 -1）；#RRGGBB 正常解析；失败返回 def。 */
static int OldParseColor(const WCHAR* v, DWORD def, DWORD* out) {
    if (!v) return 0;
    while (*v == L' ' || *v == L'\t') v++;
    if (v[0] == L'0' && (v[1] == 0 || v[1] == L' ' || v[1] == L'\t' || v[1] == L';')) {
        *out = 0;
        return -1;      /* 不显示 */
    }
    DWORD rgb;
    if (ParseColor(v, &rgb)) { *out = rgb; return 1; }
    *out = def;
    return 0;
}

static void OldParseStates(const WCHAR* v, int* mask) {
    if (!v) return;
    while (*v == L' ' || *v == L'\t') v++;
    if (!*v || *v == L';') return;
    int m = 0, any = 0;
    while (*v && *v != L';') {
        while (*v == L' ' || *v == L'\t' || *v == L',') v++;
        if (!*v || *v == L';') break;
        const WCHAR* tok = v;
        while (*v && *v != L',' && *v != L';' && *v != L' ' && *v != L'\t') v++;
        size_t n = (size_t)(v - tok);
        if (!n) break;
        if (n == 3 && _wcsnicmp(tok, L"all", 3) == 0)  { m = 0x3F; any = 1; continue; }
        if (n == 4 && _wcsnicmp(tok, L"none", 4) == 0) { m = 0;    any = 1; continue; }
        static const struct { const WCHAR* nm; unsigned bit; } bits[] = {
            { L"en", OLD_EN }, { L"caps", OLD_CAPS }, { L"kbden", OLD_KBDEN },
            { L"cn", OLD_CN }, { L"jp", OLD_JP }, { L"kr", OLD_KR },
        };
        for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
            if (wcslen(bits[i].nm) == n && _wcsnicmp(tok, bits[i].nm, n) == 0) {
                m |= (int)bits[i].bit; any = 1; break;
            }
        }
    }
    if (any) { *mask = m; }
}

static void TrimW(WCHAR* s) {
    size_t l = wcslen(s);
    while (l > 0 && (s[l-1] == L' ' || s[l-1] == L'\t' || s[l-1] == L'\r')) s[--l] = 0;
    WCHAR* p = s;
    while (*p == L' ' || *p == L'\t') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(WCHAR));
}

static void StripExeW(WCHAR* s) {
    size_t l = wcslen(s);
    if (l >= 4 && _wcsicmp(s + l - 4, L".exe") == 0) s[l-4] = 0;
}

static int ReadOldIni(const WCHAR* path, OldIni* o) {
    ZeroMemory(o, sizeof(*o));
    o->alpha = 255;
    o->size = 9; o->shape = SHAPE_CIRCLE; o->offX = 2; o->offY = 4;
    o->pollMs = 100; o->trackMs = 15; o->imeStrategy = 0;
    o->hideFullscreen = 1; o->hideComposition = 1;
    o->caretTimeoutMs = 150; o->autoHideMs = 1000; o->wpsCom = 1;
    o->ahMask = (int)OLD_CN;
    o->en = 0xFF0000; o->caps = 0x0080FF; o->kbden = 0x8B00FF;
    o->cn = 0x00A000; o->jp = 0x000000; o->kr = 0x000000;

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); return 0; }
    char* raw = (char*)malloc((size_t)sz + 1);
    if (!raw) { fclose(f); return 0; }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = 0;

    const char* p = raw;
    if (rd >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) p += 3;
    int srcLen = (int)(rd - (size_t)(p - raw));
    int wl = MultiByteToWideChar(CP_UTF8, 0, p, srcLen, NULL, 0);
    if (wl <= 0) { free(raw); return 0; }
    WCHAR* wide = (WCHAR*)malloc(((size_t)wl + 1) * sizeof(WCHAR));
    if (!wide) { free(raw); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, p, srcLen, wide, wl);
    wide[wl] = 0;
    free(raw);

    int sec = -1;   /* 0 General, 1 Colors, 2 Overlay, 3 Ignore */
    WCHAR* line = wide;
    while (line) {
        WCHAR* nl = wcschr(line, L'\n');
        if (nl) *nl = 0;
        TrimW(line);
        if (line[0] == L';' || !line[0]) { /* 注释/空行 */ }
        else if (line[0] == L'[') {
            WCHAR name[CFG_NAME_MAX];
            _snwprintf_s(name, CFG_NAME_MAX, _TRUNCATE, L"%s", line + 1);
            WCHAR* rb = wcschr(name, L']');
            if (rb) *rb = 0;
            TrimW(name);
            if      (_wcsicmp(name, L"general") == 0) sec = 0;
            else if (_wcsicmp(name, L"colors") == 0)  sec = 1;
            else if (_wcsicmp(name, L"overlay") == 0) sec = 2;
            else if (_wcsicmp(name, L"ignore") == 0)  sec = 3;
            else                                      sec = -1;
        } else {
            WCHAR* eq = wcschr(line, L'=');
            if (sec == 3 && !eq) {
                if (o->ignoreCount < CFG_MAX_IGNORE) {
                    WCHAR tmp[CFG_NAME_MAX];
                    _snwprintf_s(tmp, CFG_NAME_MAX, _TRUNCATE, L"%s", line);
                    TrimW(tmp);
                    StripExeW(tmp);
                    if (tmp[0]) {
                        lstrcpynW(o->ignore[o->ignoreCount], tmp, CFG_NAME_MAX);
                        o->ignoreCount++;
                    }
                }
            } else if (eq) {
                *eq = 0;
                WCHAR k[CFG_NAME_MAX], v[512];
                _snwprintf_s(k, CFG_NAME_MAX, _TRUNCATE, L"%s", line);
                _snwprintf_s(v, 512, _TRUNCATE, L"%s", eq + 1);
                TrimW(k);
                TrimW(v);
                if (sec == 0) {
                    if      (_wcsicmp(k, L"pollintervalms") == 0)      o->pollMs = OldParseInt(v, 100, 5, 60000);
                    else if (_wcsicmp(k, L"trackintervalms") == 0)     o->trackMs = OldParseInt(v, 15, 5, 200);
                    else if (_wcsicmp(k, L"imestrategy") == 0)         o->imeStrategy = OldParseInt(v, 0, 0, 2);
                    else if (_wcsicmp(k, L"hidewhenfullscreen") == 0)  o->hideFullscreen = OldParseInt(v, 1, 0, 1);
                    else if (_wcsicmp(k, L"hidewhencomposition") == 0) o->hideComposition = OldParseInt(v, 1, 0, 1);
                    else if (_wcsicmp(k, L"carettimeoutms") == 0)      o->caretTimeoutMs = OldParseInt(v, 150, 20, 5000);
                    else if (_wcsicmp(k, L"autohidems") == 0)          o->autoHideMs = OldParseInt(v, 1000, 0, 60000);
                    else if (_wcsicmp(k, L"autohidestates") == 0)    { OldParseStates(v, &o->ahMask); o->ahHave = 1; }
                    else if (_wcsicmp(k, L"wpscom") == 0)              o->wpsCom = OldParseInt(v, 1, 0, 1);
                } else if (sec == 1) {
                    DWORD tmp;
                    if      (_wcsicmp(k, L"en") == 0)    { int r = OldParseColor(v, o->en, &tmp);   o->en = tmp;   if (r < 0) o->have |= 1; }
                    else if (_wcsicmp(k, L"caps") == 0)  { int r = OldParseColor(v, o->caps, &tmp); o->caps = tmp; if (r < 0) o->have |= 2; }
                    else if (_wcsicmp(k, L"kbden") == 0) { int r = OldParseColor(v, o->kbden, &tmp); o->kbden = tmp; if (r < 0) o->have |= 4; }
                    else if (_wcsicmp(k, L"cn") == 0)    { int r = OldParseColor(v, o->cn, &tmp);   o->cn = tmp;   if (r < 0) o->have |= 8; }
                    else if (_wcsicmp(k, L"jp") == 0)    { int r = OldParseColor(v, o->jp, &tmp);   o->jp = tmp;   if (r < 0) o->have |= 16; }
                    else if (_wcsicmp(k, L"kr") == 0)    { int r = OldParseColor(v, o->kr, &tmp);   o->kr = tmp;   if (r < 0) o->have |= 32; }
                    else if (_wcsicmp(k, L"alpha") == 0)   o->alpha = OldParseInt(v, 255, 0, 255);
                } else if (sec == 2) {
                    if      (_wcsicmp(k, L"size") == 0)    o->size = OldParseInt(v, 9, 1, 64);
                    else if (_wcsicmp(k, L"shape") == 0) {
                        if (_wcsicmp(v, L"triangle") == 0 || _wcsnicmp(v, L"tri", 3) == 0)
                            o->shape = SHAPE_TRIANGLE;
                        else
                            o->shape = SHAPE_CIRCLE;
                    }
                    else if (_wcsicmp(k, L"offsetx") == 0) o->offX = OldParseInt(v, 2, -512, 512);
                    else if (_wcsicmp(k, L"offsety") == 0) o->offY = OldParseInt(v, 4, -512, 512);
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(wide);
    return 1;
}

/* 把旧 ini 映射成新配置。 */
static void MigrateFromOld(const OldIni* o, ImeCfg* c) {
    c->pollMs = o->pollMs;
    c->trackMs = o->trackMs;
    c->imeStrategy = o->imeStrategy;
    c->caretTimeoutMs = o->caretTimeoutMs;
    c->hideFullscreen = o->hideFullscreen;
    c->hideComposition = o->hideComposition;
    c->wpsCom = o->wpsCom;
    c->autoHideMs = o->autoHideMs;

    c->def.size = o->size;
    c->def.shape = o->shape;
    c->def.offsetX = o->offX;
    c->def.offsetY = o->offY;
    c->def.alpha = o->alpha;

    /* 旧色值写 0 = 该状态不显示 -> enabled=0 */
    #define SET_COLOR(idx, rgbv, hidden) do { \
        c->st[idx].rgb = (rgbv); \
        c->st[idx].have |= SF_COLOR; \
        if (hidden) { c->st[idx].enabled = 0; c->st[idx].have |= SF_ENABLED; } \
    } while (0)

    SET_COLOR(IMEST_US,         o->kbden, (o->have & 4) != 0);
    SET_COLOR(IMEST_CN_CN,      o->cn,    (o->have & 8) != 0);
    SET_COLOR(IMEST_CN_EN,      o->en,    (o->have & 1) != 0);
    SET_COLOR(IMEST_JP,         o->jp,    (o->have & 16) != 0);
    SET_COLOR(IMEST_KR,         o->kr,    (o->have & 32) != 0);
    SET_COLOR(IMEST_OTHER,      o->kbden, (o->have & 4) != 0);

    /* 旧 Caps 是"盖掉一切"的独立状态：显式写进所有 *_caps，并断开继承，
       这样迁移后行为与旧版完全一致（用户想改成"跟随"再去设置里勾）。 */
    int capsHidden = (o->have & 2) != 0;
    const ImeState capsStates[] = {
        IMEST_US_CAPS, IMEST_CN_CAPS, IMEST_JP_CAPS, IMEST_KR_CAPS, IMEST_OTHER_CAPS
    };
    for (size_t i = 0; i < sizeof(capsStates) / sizeof(capsStates[0]); i++) {
        ImeState s = capsStates[i];
        c->st[s].rgb = o->caps;
        c->st[s].have |= SF_COLOR;
        c->st[s].inherit = -1;
        if (capsHidden) { c->st[s].enabled = 0; c->st[s].have |= SF_ENABLED; }
    }

    /* 旧 AutoHideStates 位掩码 -> 各状态 transient */
    if (o->ahHave) {
        struct { unsigned oldBit; ImeState st; } map[] = {
            { OLD_EN,    IMEST_CN_EN },
            { OLD_CAPS,  IMEST_US_CAPS },
            { OLD_KBDEN, IMEST_US },
            { OLD_CN,    IMEST_CN_CN },
            { OLD_JP,    IMEST_JP },
            { OLD_KR,    IMEST_KR },
        };
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            int on = (o->ahMask & (int)map[i].oldBit) != 0;
            c->st[map[i].st].transient = on;
            c->st[map[i].st].have |= SF_TRANSIENT;
        }
    }

    c->ignoreCount = o->ignoreCount;
    for (int i = 0; i < o->ignoreCount; i++)
        lstrcpynW(c->ignore[i], o->ignore[i], CFG_NAME_MAX);
    #undef SET_COLOR
}

/* ---------- 读 JSON ---------- */

static int ReadJsonFile(const WCHAR* path, ImeCfg* c) {
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return 0; }
    char* raw = (char*)malloc((size_t)sz + 1);
    if (!raw) { fclose(f); return 0; }
    size_t rd = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[rd] = 0;

    Jv* root = JsonParse(raw, (int)rd);
    free(raw);
    if (!root) return 0;

    int ver = JsonInt(root, L"version", 0);
    (void)ver;   /* 目前只有 v1；将来按版本分支 */

    const Jv* g = JsonGet(root, L"general");
    if (g && g->type == JV_OBJ) {
        c->pollMs          = JsonInt(g, L"pollIntervalMs", c->pollMs);
        c->trackMs         = JsonInt(g, L"trackIntervalMs", c->trackMs);
        c->imeStrategy     = JsonInt(g, L"imeStrategy", c->imeStrategy);
        c->caretTimeoutMs  = JsonInt(g, L"caretTimeoutMs", c->caretTimeoutMs);
        c->hideFullscreen  = JsonBool(g, L"hideWhenFullscreen", c->hideFullscreen);
        c->hideComposition = JsonBool(g, L"hideWhenComposition", c->hideComposition);
        c->wpsCom          = JsonBool(g, L"wpsCom", c->wpsCom);
        c->autoHideMs      = JsonInt(g, L"autoHideMs", c->autoHideMs);
    }

    const Jv* d = JsonGet(root, L"defaults");
    if (d && d->type == JV_OBJ) {
        ReadState(d, &c->def);
        c->def.have |= SF_SIZE | SF_OFFSETX | SF_OFFSETY | SF_ALPHA | SF_SHAPE;
    }

    const Jv* st = JsonGet(root, L"states");
    if (st && st->type == JV_OBJ) {
        for (int i = 0; i < IMEST_COUNT; i++) {
            const Jv* o = JsonGet(st, kStateNames[i]);
            if (o) ReadState(o, &c->st[i]);
        }
    }

    const Jv* ig = JsonGet(root, L"ignore");
    if (ig && ig->type == JV_ARR) {
        c->ignoreCount = 0;
        for (int i = 0; i < ig->count && c->ignoreCount < CFG_MAX_IGNORE; i++) {
            if (ig->items[i]->type != JV_STR || !ig->items[i]->s[0]) continue;
            WCHAR tmp[CFG_NAME_MAX];
            lstrcpynW(tmp, ig->items[i]->s, CFG_NAME_MAX);
            StripExeW(tmp);
            if (tmp[0]) {
                lstrcpynW(c->ignore[c->ignoreCount], tmp, CFG_NAME_MAX);
                c->ignoreCount++;
            }
        }
    }

    JsonFree(root);
    return 1;
}

/* ---------- 写 JSON ---------- */

int CfgSave(const ImeCfg* c) {
    if (!c) return 0;
    Jw w;
    JwInit(&w);
    JwRaw(&w, "{\n  ");
    JwKey(&w, L"version"); JwInt(&w, CFG_VERSION); JwRaw(&w, ",\n\n  ");

    JwKey(&w, L"general"); JwRaw(&w, "{\n    ");
    JwKey(&w, L"pollIntervalMs");     JwInt(&w, c->pollMs);          JwRaw(&w, ",\n    ");
    JwKey(&w, L"trackIntervalMs");    JwInt(&w, c->trackMs);         JwRaw(&w, ",\n    ");
    JwKey(&w, L"imeStrategy");        JwInt(&w, c->imeStrategy);     JwRaw(&w, ",\n    ");
    JwKey(&w, L"caretTimeoutMs");     JwInt(&w, c->caretTimeoutMs);  JwRaw(&w, ",\n    ");
    JwKey(&w, L"hideWhenFullscreen"); JwBool(&w, c->hideFullscreen); JwRaw(&w, ",\n    ");
    JwKey(&w, L"hideWhenComposition");JwBool(&w, c->hideComposition);JwRaw(&w, ",\n    ");
    JwKey(&w, L"wpsCom");             JwBool(&w, c->wpsCom);         JwRaw(&w, ",\n    ");
    JwKey(&w, L"autoHideMs");         JwInt(&w, c->autoHideMs);      JwRaw(&w, "\n  },\n\n  ");

    JwKey(&w, L"defaults"); JwRaw(&w, "{\n    ");
    JwKey(&w, L"size");    JwInt(&w, c->def.size);    JwRaw(&w, ",\n    ");
    JwKey(&w, L"offsetX"); JwInt(&w, c->def.offsetX); JwRaw(&w, ",\n    ");
    JwKey(&w, L"offsetY"); JwInt(&w, c->def.offsetY); JwRaw(&w, ",\n    ");
    JwKey(&w, L"alpha");   JwInt(&w, c->def.alpha);   JwRaw(&w, ",\n    ");
    JwKey(&w, L"shape");   JwStr(&w, CfgShapeName(c->def.shape)); JwRaw(&w, "\n  },\n\n  ");

    JwKey(&w, L"states"); JwRaw(&w, "{\n");
    for (int i = 0; i < IMEST_COUNT; i++) {
        const StateStyle* s = &c->st[i];
        JwRaw(&w, "    ");
        JwKey(&w, kStateNames[i]);
        JwRaw(&w, "{ ");
        int first = 1;
        #define SEP() do { if (!first) JwRaw(&w, ", "); first = 0; } while (0)
        /* inherit 必须**始终**写出来：缺省值对 caps 状态是"继承"，
           不写的话"用户主动取消继承"就存不下来，读回时会被默认值顶成继承。
           null = 不继承。 */
        SEP(); JwKey(&w, L"inherit");
        if (s->inherit >= 0) JwStr(&w, kStateNames[s->inherit]);
        else                 JwRaw(&w, "null");
        if (s->have & SF_ENABLED) {
            SEP(); JwKey(&w, L"enabled"); JwBool(&w, s->enabled);
        }
        if (s->have & SF_COLOR) {
            WCHAR col[16];
            FormatColor(s->rgb, col, 16);
            SEP(); JwKey(&w, L"color"); JwStr(&w, col);
        }
        if (s->have & SF_SHAPE) {
            SEP(); JwKey(&w, L"shape"); JwStr(&w, CfgShapeName(s->shape));
        }
        if (s->have & SF_IMAGE) {
            SEP(); JwKey(&w, L"image"); JwStr(&w, s->image);
        }
        if (s->have & SF_SIZE) {
            SEP(); JwKey(&w, L"size"); JwInt(&w, s->size);
        }
        if (s->have & SF_OFFSETX) {
            SEP(); JwKey(&w, L"offsetX"); JwInt(&w, s->offsetX);
        }
        if (s->have & SF_OFFSETY) {
            SEP(); JwKey(&w, L"offsetY"); JwInt(&w, s->offsetY);
        }
        if (s->have & SF_ALPHA) {
            SEP(); JwKey(&w, L"alpha"); JwInt(&w, s->alpha);
        }
        if (s->have & SF_TRANSIENT) {
            SEP(); JwKey(&w, L"transient"); JwBool(&w, s->transient);
        }
        #undef SEP
        JwRaw(&w, " }");
        JwRaw(&w, (i + 1 < IMEST_COUNT) ? ",\n" : "\n");
    }
    JwRaw(&w, "  },\n\n  ");

    JwKey(&w, L"ignore"); JwRaw(&w, "[");
    for (int i = 0; i < c->ignoreCount; i++) {
        if (i) JwRaw(&w, ", ");
        JwStr(&w, c->ignore[i]);
    }
    JwRaw(&w, "]\n}\n");

    if (w.oom) { JwFree(&w); return 0; }

    WCHAR path[MAX_PATH];
    CfgPath(path, MAX_PATH);
    /* 先写临时文件再改名：写到一半断电不会留下半个 JSON */
    WCHAR tmp[MAX_PATH];
    _snwprintf_s(tmp, MAX_PATH, _TRUNCATE, L"%s.tmp", path);

    FILE* f = NULL;
    if (_wfopen_s(&f, tmp, L"wb") != 0 || !f) { JwFree(&w); return 0; }
    fwrite(w.p, 1, w.len, f);
    fclose(f);
    JwFree(&w);

    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

/* ---------- 加载入口 ---------- */

void CfgLoad(ImeCfg* c) {
    CfgDefaults(c);

    WCHAR path[MAX_PATH];
    CfgPath(path, MAX_PATH);

    if (!ReadJsonFile(path, c)) {         /* 有 JSON 就用它 */
        /* 没有 JSON：看看有没有旧 ini 可以迁移 */
        WCHAR ini[MAX_PATH];
        IniPath(ini, MAX_PATH);
        OldIni old;
        if (ReadOldIni(ini, &old)) {
            MigrateFromOld(&old, c);
            CfgSave(c);                       /* 立刻落盘成新格式 */
            /* 旧文件改名保留，用户想回退还能找到 */
            WCHAR bak[MAX_PATH];
            _snwprintf_s(bak, MAX_PATH, _TRUNCATE, L"%s.bak", ini);
            MoveFileExW(ini, bak, MOVEFILE_REPLACE_EXISTING);
        } else {
            /* 全新安装：写一份默认配置，方便用户出问题时查看 */
            CfgSave(c);
        }
    }

    MaterializeStates(c);
}

/* ---------- 黑名单 ---------- */

int CfgIsIgnored(const ImeCfg* c, const WCHAR* procName) {
    if (!c || !procName || !procName[0] || c->ignoreCount == 0) return 0;
    WCHAR n[CFG_NAME_MAX];
    lstrcpynW(n, procName, CFG_NAME_MAX);
    StripExeW(n);
    for (int i = 0; i < c->ignoreCount; i++)
        if (_wcsicmp(n, c->ignore[i]) == 0) return 1;
    return 0;
}
