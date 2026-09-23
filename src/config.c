#include "ime_indicator.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ================= 配置模块（IMEIndicator.ini，与 exe 同目录） =================
   INI 风格，UTF-8 编码；`;` 开头为注释，空行忽略，[段名] 切段。
   [General]   键值项（Key = Value）
   [Colors]    三/四状态颜色（#RRGGBB 或 #RRGGBBAA）+ 全局透明度
   [Overlay]   圆点尺寸与光标偏移、检测间隔
   [Ignore]    程序黑名单：前台是这些进程时跳过中英状态检测（规避游戏卡顿）
   文件不存在时写一份带注释的模板。颜色值只认前导 #RRGGBB 解析，
   解析失败回退默认。 */

#define CFG_MAX_ENTRIES 128
#define CFG_NAME_MAX    64

static WCHAR g_ignore[CFG_MAX_ENTRIES][CFG_NAME_MAX];
static int   g_ignoreCount = 0;

/* ---------- 小工具 ---------- */

#define ISAME(s,w) (_stricmp(s,w)==0)

/* ---------- AutoHideStates：逗号分隔的状态名列表 ----------
   状态名沿用 [Colors] 的键名（En/Caps/KbdEn/Cn/Jp/Kr，大小写不敏感），
   另支持 all（全部状态都走临时显示）与 none（都不走 = 关掉这个功能）。
   ★ 值里可以跟行内注释（`AutoHideStates = Cn  ; 只有中文态`）：遇到 ';'
   一律停读，否则注释里的中文词会被当成状态名去匹配。
   空值（键写了没给值）或全是认不出的词 —— 保持传入的默认掩码。 */
static const struct { const char* name; unsigned bit; } kStateBits[] = {
    { "en",    IME_AH_BIT(IMEST_EN)     },
    { "caps",  IME_AH_BIT(IMEST_CAPS)   },
    { "kbden", IME_AH_BIT(IMEST_KBD_EN) },
    { "cn",    IME_AH_BIT(IMEST_CN)     },
    { "jp",    IME_AH_BIT(IMEST_JP)     },
    { "kr",    IME_AH_BIT(IMEST_KR)     },
};

static int ParseStatesA(const char* v, int defMask) {
    if (!v) return defMask;
    while (*v==' '||*v=='\t') v++;
    if (!*v || *v==';') return defMask;      /* 没给值 */
    int mask = 0, any = 0;
    while (*v && *v != ';') {
        while (*v==' '||*v=='\t'||*v==',') v++;
        if (!*v || *v==';') break;
        const char* tok = v;
        while (*v && *v!=',' && *v!=';' && *v!=' ' && *v!='\t') v++;
        size_t n = (size_t)(v - tok);
        if (!n) break;
        if (n==3 && !_strnicmp(tok,"all",3))       { mask = (int)IME_AH_ALL; any = 1; continue; }
        if (n==4 && !_strnicmp(tok,"none",4))      { mask = 0;               any = 1; continue; }
        for (size_t i = 0; i < sizeof(kStateBits)/sizeof(kStateBits[0]); i++) {
            if (strlen(kStateBits[i].name)==n && !_strnicmp(tok,kStateBits[i].name,n)) {
                mask |= (int)kStateBits[i].bit;
                any = 1;
                break;
            }
        }
    }
    return any ? mask : defMask;
}

/* ---------- 已有配置文件补写新参数 ----------
   程序升级后模板里新增的键，用户手上的旧 ini 里没有 —— 光靠默认值，用户想改
   却找不到那一行。这里把缺的键（连注释）补插到对应段标题行之后。
   按字节处理：文件是 UTF-8（可能带 BOM），模板行以 \n 收尾；插入文本的行尾
   跟随文件现有风格（检测首行用的是 \r\n 还是 \n）。 */
typedef struct {
    const char* key;      /* 检测这个键名是否已存在（裸 ASCII 子串即可） */
    const char* section;  /* 插到这个段标题行之后，如 "[General]" */
    const char* block;    /* 要插入的文本（\n 行尾，写入时统一转换） */
} CfgUpgrade;

static const CfgUpgrade kUpgrades[] = {
    { "ImeStrategy",
      "[General]",
      "; 中英判定策略：0=自动学习(推荐) 1=只看 IME open 状态 2=只看转换模式\n"
      "; 自动学习会观察切换时哪个信号在变；个别输入法识别不准时可手动指定。\n"
      "ImeStrategy = 0\n"
      ";\n" },
    { "HideWhenFullscreen",
      "[General]",
      "; 前景窗口处于全屏时（看视频/演示）隐藏圆点：全屏下光标检测会拿到上一次\n"
      "; 的陈旧坐标，圆点会一直钉在画面上挡视线。\n"
      "HideWhenFullscreen = 1\n"
      ";\n" },
    { "CaretTimeoutMs",
      "[General]",
      "; 单次光标查询最长等待(ms)：光标检测要跨进程问 UIA/MSAA，对方程序卡住时\n"
      "; 会一直不返回。超时即放弃本轮查询（沿用上一轮显示），不冻结检测线程。\n"
      "CaretTimeoutMs = 150\n"
      ";\n" },
    { "Shape",
      "[Overlay]",
      "; 形状：circle=圆（默认） triangle=三角形（等边，尖角朝上）\n"
      "; 两者尺寸都按同一个 Size 算，换形状不用重新调大小。\n"
      "Shape = circle\n" },
    { "HideWhenComposition",
      "[General]",
      ";\n"
      "; 输入法组合窗显示时隐藏圆点：微软拼音打字时会在光标旁画拼音串（还有候选\n"
      "; 列表），和圆点正好重叠，0 = 不管它。组合结束（上屏/Esc）圆点自动回来。\n"
      "HideWhenComposition = 1\n" },
    { "AutoHideMs",
      "[General]",
      ";\n"
      "; 「临时显示」：名单里的状态只在发生变化后露一小会儿就自动消失，而不是一直\n"
      "; 钉在光标旁。这个值 = 每次显示多久(ms)；0 = 不启用临时显示（所有状态常显）。\n"
      "AutoHideMs = 1000\n" },
    /* ★ 与上一条拆开写：两条各自判定"缺失"。合成一条的话，只缺其中一个的用户
       会被整块补写，于是多出一行重复键（后写的覆盖用户手改的值）。 */
    { "AutoHideStates",
      "[General]",
      ";\n"
      "; 参与临时显示的状态名单：逗号分隔，状态名同下面 [Colors] 的键名；\n"
      "; all = 全部状态都临时显示，none = 全部维持常显。\n"
      "; ★ 色值写 0 的状态始终不显示，这条优先于名单。\n"
      "AutoHideStates = Cn\n" },
    { "WpsCom",
      "[General]",
      ";\n"
      "; WPS（wps.exe / wpp.exe / et.exe）的光标对标准接口全不可见：自身不用 Win32\n"
      "; 光标、无 MSAA caret、无 UIA 文本、走 TSF 不走 IMM；演示还会报出屏幕原点上\n"
      "; 的 1×1 假光标（MSAA 也照抄那份）。只能走它抄自 Office 的对象模型：\n"
      "; 文字 = Selection.Range + ActiveWindow.GetPoint；演示 = 把 Selection.TextRange\n"
      "; 追到 TextFrame.TextRange，按插入点取逐字符框 Bound*（空文本框退回 Font.Size\n"
      "; 当行高，插入点取段落左缘）；表格 = ActiveCell 的 Left/Top/Height +\n"
      "; PointsToScreenPixels(0) + DPI×Zoom 比例（PTS 的跨度是 1:1，不能用），圆点\n"
      "; 放在当前格左缘；0 = 关掉这条通道（WPS 里就不会显示圆点）。\n"
      "WpsCom = 1\n" },
};

/* buf 里找裸键名（行首可有空白，键名后跟空白或'='，避免撞上别的单词） */
static int HasKey(const char* buf, size_t n, const char* key) {
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl <= n; i++) {
        if (memcmp(buf + i, key, kl) != 0) continue;
        char prev = (i == 0) ? '\n' : buf[i-1];
        char next = (i + kl < n) ? buf[i+kl] : '\n';
        int prevOk = (prev == '\n' || prev == '\r' || prev == ' ' || prev == '\t');
        int nextOk = (next == '=' || next == ' ' || next == '\t' || next == '\r' || next == '\n');
        if (prevOk && nextOk) return 1;
    }
    return 0;
}

/* 找段标题行（如 "[General]"）行尾之后的位置；找不到返回 (size_t)-1 */
static size_t FindSectionEnd(const char* buf, size_t n, const char* section) {
    size_t sl = strlen(section);
    for (size_t i = 0; i + sl <= n; i++) {
        if (memcmp(buf + i, section, sl) != 0) continue;
        /* 必须顶格（行首），且后面紧跟 ] 或空白 —— 精确匹配段标题 */
        char prev = (i == 0) ? '\n' : buf[i-1];
        char next = (i + sl < n) ? buf[i+sl] : '\n';
        if (prev != '\n' && prev != '\r') continue;
        if (next != '\n' && next != '\r') continue;
        /* 返回该行的行尾之后 */
        size_t j = i + sl;
        while (j < n && buf[j] != '\n') j++;
        return (j < n) ? j + 1 : j;
    }
    return (size_t)-1;
}

/* 读入 path，把缺失的键补写进去（有缺才写文件）。返回 1=文件被更新。 */
static int UpgradeIniFile(const WCHAR* path) {
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) { fclose(f); return 0; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;

    /* 行尾风格：文件里出现 \r\n 就用 \r\n，否则 \n（模板本身是 \n） */
    int crlf = 0;
    for (size_t i = 0; i + 1 < rd; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n') { crlf = 1; break; }

    /* 逐个补缺失键：每次插入都重新在最新 buf 上找段位置，块最多 4 个，开销无所谓 */
    int changed = 0;
    for (size_t u = 0; u < sizeof(kUpgrades)/sizeof(kUpgrades[0]); u++) {
        const CfgUpgrade* up = &kUpgrades[u];
        if (HasKey(buf, rd, up->key)) continue;
        size_t at = FindSectionEnd(buf, rd, up->section);
        if (at == (size_t)-1) continue;
        /* 展开插入文本的行尾 */
        size_t bl = strlen(up->block);
        char* ins = (char*)malloc(bl * 2 + 1);
        if (!ins) continue;
        size_t k = 0;
        for (size_t m = 0; m < bl; m++) {
            if (up->block[m] == '\n' && !(k > 0 && ins[k-1] == '\r')) {
                if (crlf) ins[k++] = '\r';
            }
            ins[k++] = up->block[m];
        }
        char* nb = (char*)malloc(rd + k + 1);
        if (!nb) { free(ins); continue; }
        memcpy(nb, buf, at);
        memcpy(nb + at, ins, k);
        memcpy(nb + at + k, buf + at, rd - at);
        free(ins);
        free(buf);
        buf = nb;
        rd += k;
        buf[rd] = 0;
        changed = 1;
    }

    if (changed) {
        FILE* w = NULL;
        if (_wfopen_s(&w, path, L"wb") == 0 && w) {
            fwrite(buf, 1, rd, w);
            fclose(w);
        } else {
            changed = 0;   /* 写不回去别报成功 */
        }
    }
    free(buf);
    return changed;
}

static void TrimW(WCHAR* s) {
    size_t l = wcslen(s);
    while (l > 0 && (s[l-1]==L' '||s[l-1]==L'\t'||s[l-1]==L'\r')) s[--l]=0;
    WCHAR* p = s;
    while (*p==L' '||*p==L'\t') p++;
    if (p!=s) memmove(s, p, (wcslen(p)+1)*sizeof(WCHAR));
}

static void StripExeW(WCHAR* s) {
    size_t l = wcslen(s);
    if (l>=4 && _wcsicmp(s+l-4, L".exe")==0) s[l-4]=0;
}

static int ParseIntA(const char* v, int def, int lo, int hi) {
    if (!v||!v[0]) return def;
    const char* p=v;
    int neg=0;
    if (*p=='-') { neg=1; p++; }
    else if (*p=='+') p++;
    long long n=0; int any=0;
    for (; *p; p++) {
        if (*p<'0'||*p>'9') break;
        n=n*10+(*p-'0'); any=1;
        if (n>100000000LL) return def;
    }
    if (!any) return def;
    if (neg) n=-n;
    if (n<lo) n=lo; if (n>hi) n=hi;
    return (int)n;
}

/* 只认 #RRGGBB：透明度一律走 Alpha，颜色值不携带 AA。 */
static int HexV(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return 0;
}
/* 通用规则：色值写 0（可带行内注释）= 该状态不显示圆点。
   ★ 用 IME_COLOR_NONE 哨兵而不是 0 —— 0 是**黑色**，两者不能混。 */
static DWORD ParseColor6(const char* v, DWORD defRgb) {
    if (!v) return defRgb;
    while (*v==' '||*v=='\t') v++;
    if (v[0]=='0' && (v[1]==0 || v[1]==' ' || v[1]=='\t' || v[1]==';'))
        return IME_COLOR_NONE;
    if (*v!='#') return defRgb;
    v++;
    int len=0;
    while (v[len]>='0'&&v[len]<='9'||(v[len]>='A'&&v[len]<='F')||(v[len]>='a'&&v[len]<='f')) len++;
    if (len<6) return defRgb;
    return ((DWORD)HexV(v[0])<<20)|((DWORD)HexV(v[1])<<16)|
           ((DWORD)HexV(v[2])<<12)|((DWORD)HexV(v[3])<<8)|
           ((DWORD)HexV(v[4])<<4)|(DWORD)HexV(v[5]);
}

/* 形状按单词写（circle / triangle），不认数字：读 ini 的人一眼能看出改成什么。
   非法值回落默认圆，跟其它配置项的"解析失败即默认"一致。 */
static int ParseShapeA(const char* v, int def) {
    if (!v) return def;
    while (*v==' '||*v=='\t') v++;
    if (ISAME(v,"circle") || ISAME(v,"c")) return SHAPE_CIRCLE;
    /* 词尾允许跟注释/空白：只比前缀 */
    if (!_strnicmp(v,"triangle",8) || !_strnicmp(v,"tri",3)) return SHAPE_TRIANGLE;
    return def;
}

/* ---------- 默认值 ---------- */
#define DEF_EN_RGB   0xFF0000  /* 红                                     */
#define DEF_CAPS_RGB 0x0080FF  /* 蓝                                     */
#define DEF_KBDEN_RGB 0x8B00FF /* 紫                                     */
#define DEF_CN_RGB   0x00A000  /* 绿（中文态；临时显示，不长期占位）       */
#define DEF_JP_RGB   0x000000  /* 黑                                     */
#define DEF_KR_RGB   0x000000  /* 黑                                     */
#define DEF_ALPHA   255

static void SrcOfPath(WCHAR* out, size_t cap) {
    WCHAR exe[MAX_PATH]; DWORD n=GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash=NULL;
    for (DWORD i=0;i<n;i++) if (exe[i]==L'\\') slash=&exe[i];
    if (slash) *slash=0;
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\IMEIndicator.ini", exe);
}


/* 配置文件完整路径（托盘「打开配置」用） */
void CfgPath(WCHAR* out, size_t cap) { SrcOfPath(out, cap); }

void CfgLoad(ImeCfg* c) {
    WCHAR path[MAX_PATH]; SrcOfPath(path, MAX_PATH);
    int defAl=DEF_ALPHA;
    DWORD en=DEF_EN_RGB, caps=DEF_CAPS_RGB, kbden=DEF_KBDEN_RGB;
    DWORD jp=DEF_JP_RGB, kr=DEF_KR_RGB;
    DWORD cn=DEF_CN_RGB;       /* 中文态：绿色，且默认走"临时显示"（见 AutoHideStates） */
    c->size=9; c->offsetX=2; c->offsetY=4; c->pollMs=100; c->trackMs=15;
    c->imeStrategy=0;
    c->hideFullscreen=1;
    c->hideComposition=1;
    c->caretTimeoutMs=150;
    c->shape=SHAPE_CIRCLE;
    c->autoHideMs=1000;                    /* 临时显示时长 */
    c->autoHideMask=(int)IME_AH_BIT(IMEST_CN);  /* 默认只有中文态临时显示 */
    c->wpsCom=1;                           /* WPS 文字/演示走 Office 对象模型取光标 */

    /* 旧配置文件里缺新参数时先补写（本次启动就能读到新键） */
    UpgradeIniFile(path);

    FILE* f=NULL;
    if (_wfopen_s(&f, path, L"rb")!=0 || !f) {
        /* 文件不存在：写模板 */
        static const char kTpl[] =
            "; IMEIndicator 配置文件（UTF-8，与 exe 同目录）\n"
            "; `;` 开头为注释（空行忽略）。改动后重启程序生效。\n"
            "[General]\n"
            ";\n"
            "PollIntervalMs = 100    ; 状态(中英/大写/黑名单)检测间隔(ms)\n"
            "TrackIntervalMs = 15    ; 光标坐标追踪间隔(ms)\n"
            "; 中英判定策略：0=自动学习(推荐) 1=只看 IME open 状态 2=只看转换模式\n"
            "; 自动学习会观察切换时哪个信号在变；个别输入法识别不准时可手动指定。\n"
            "ImeStrategy = 0\n"
            ";\n"
            "; 前景窗口处于全屏时（看视频/演示）隐藏圆点：全屏下光标检测会拿到上一次\n"
            "; 的陈旧坐标，圆点会一直钉在画面上挡视线。\n"
            "HideWhenFullscreen = 1\n"
            ";\n"
            "; 输入法组合窗显示时隐藏圆点：微软拼音打字时会在光标旁画拼音串（还有候选\n"
            "; 列表），和圆点正好重叠，0 = 不管它。组合结束（上屏/Esc）圆点自动回来。\n"
            "HideWhenComposition = 1\n"
            ";\n"
            "; 单次光标查询最长等待(ms)：光标检测要跨进程问 UIA/MSAA，对方程序卡住时\n"
            "; 会一直不返回。超时即放弃本轮查询（沿用上一轮显示），不冻结检测线程。\n"
            "CaretTimeoutMs = 150\n"
            ";\n"
            "; 「临时显示」：名单里的状态只在发生变化后露一小会儿就自动消失，\n"
            "; 而不是一直钉在光标旁。AutoHideMs = 每次显示多久(ms)，0 = 不启用\n"
            "; 临时显示（所有状态都常显）。\n"
            "AutoHideMs = 1000\n"
            ";\n"
            "; 参与临时显示的状态名单，逗号分隔，状态名同下面 [Colors] 的键名；\n"
            "; all = 全部状态都临时显示，none = 全部维持常显。\n"
            "; ★ 色值写 0 的状态始终不显示，这条优先于名单。\n"
            "AutoHideStates = Cn\n"
            ";\n"
            "; WPS（wps.exe / wpp.exe / et.exe）的光标对标准接口全不可见：自身不用 Win32\n"
            "; 光标、无 MSAA caret、无 UIA 文本、走 TSF 不走 IMM；演示还会报出屏幕原点上\n"
            "; 的 1×1 假光标（MSAA 也照抄那份）。只能走它抄自 Office 的对象模型：\n"
            "; 文字 = Selection.Range + ActiveWindow.GetPoint；演示 = 把 Selection.TextRange\n"
            "; 追到 TextFrame.TextRange，按插入点取逐字符框 Bound*（空文本框退回 Font.Size\n"
            "; 当行高，插入点取段落左缘）；表格 = ActiveCell 的 Left/Top/Height +\n"
            "; PointsToScreenPixels(0) + DPI×Zoom 比例（PTS 的跨度是 1:1，不能用），圆点\n"
            "; 放在当前格左缘；0 = 关掉这条通道（WPS 里就不会显示圆点）。\n"
            "WpsCom = 1\n"
            ";\n"
            "[Colors]\n"
            "; 颜色格式固定 #RRGGBB（6 位十六进制，必须带 #）；透明度一律用下面的 Alpha\n"
            "; 通用规则：色值写 0 表示该状态不显示圆点（0 不等于黑色，黑色写 #000000）\n"
            "En   = #FF0000   ; 英文（默认红色）\n"
            "Caps = #0080FF   ; 大写键 Caps Lock（默认蓝色）\n"
            "KbdEn= #8B00FF   ; 英文键盘布局（默认紫色）\n"
            "Jp   = #000000   ; 日文输入法（默认黑色）\n"
            "Kr   = #000000   ; 韩文输入法（默认黑色）\n"
            "Cn   = #00A000   ; 中文输入（默认绿色；默认属 AutoHideStates，只闪一下）\n"
            "Alpha= 255       ; 圆点全局不透明度 0..255（0=全透）\n"
            ";\n"
            "[Overlay]\n"
            "Size    = 9      ; 圆点直径（像素）\n"
            "; 形状：circle=圆（默认） triangle=三角形（等边，尖角朝上）\n"
            "; 两者尺寸都按同一个 Size 算，换形状不用重新调大小。\n"
            "Shape   = circle\n"
            "OffsetX = 2      ; 相对光标左缘的水平偏移（+ 右）\n"
            "OffsetY = 4      ; 相对光标底缘的垂直偏移（+ 下）\n"
            ";\n"
            "[Ignore]\n"
            "; 前台程序命中名单时**彻底隐身**：不做状态检测、不做光标查询、不显示圆点。\n"
            "; 每行一个进程名：完全匹配、大小写不敏感、.exe 后缀可写可不写。\n"
            "; 例：\n"
            "; somegame.exe\n";
        FILE* w=NULL;
        if (_wfopen_s(&w, path, L"wb")==0 && w) {
            /* 写 BOM + 模板，避免记事本按 ANSI 误读中文注释 */
            fputs("\xEF\xBB\xBF", w);
            fwrite(kTpl,1,sizeof(kTpl)-1,w);
            fclose(w);
        }
    } else {
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        if (sz>0 && sz<=64*1024) {
            char* raw=(char*)malloc((size_t)sz+1);
            if (raw) {
                size_t rd=fread(raw,1,(size_t)sz,f); raw[rd]=0;
                const char* p=raw;
                if (rd>=3 && (unsigned char)p[0]==0xEF&&(unsigned char)p[1]==0xBB&&(unsigned char)p[2]==0xBF) p+=3;
                int srcLen=(int)(rd-(size_t)(p-raw));
                int wl=MultiByteToWideChar(CP_UTF8,0,p,srcLen,NULL,0);
                if (wl>0) {
                    WCHAR* wide=(WCHAR*)malloc(((size_t)wl+1)*sizeof(WCHAR));
                    if (wide) {
                        MultiByteToWideChar(CP_UTF8,0,p,srcLen,wide,wl); wide[wl]=0;
                        int sec=-1; /* 0 General,1 Colors,2 Overlay,3 Ignore */
                        WCHAR* line=wide;
                        while (line) {
                            WCHAR* nl=wcschr(line,L'\n'); if(nl)*nl=0;
                            TrimW(line);
                            if (line[0]==L';'||!line[0]) { /* ignore */ }
                            else if (line[0]==L'[') {
                                WCHAR name[CFG_NAME_MAX];
                                _snwprintf_s(name,CFG_NAME_MAX,_TRUNCATE,L"%s",line+1);
                                WCHAR* rb=wcschr(name,L']'); if(rb)*rb=0; TrimW(name);
                                if      (_wcsicmp(name,L"general")==0) sec=0;
                                else if (_wcsicmp(name,L"colors")==0)  sec=1;
                                else if (_wcsicmp(name,L"overlay")==0) sec=2;
                                else if (_wcsicmp(name,L"ignore")==0)  sec=3;
                                else                                   sec=-1;
                            } else {
                                WCHAR* eq=wcschr(line,L'=');
                                if (sec==0 && eq) {
                                    *eq=0;
                                    WCHAR k[CFG_NAME_MAX], v[128];
                                    _snwprintf_s(k,CFG_NAME_MAX,_TRUNCATE,L"%s",line);
                                    _snwprintf_s(v,128,_TRUNCATE,L"%s",eq+1);
                                    TrimW(k); TrimW(v);
                                    char kb[CFG_NAME_MAX*4], vb[512];
                                    WideCharToMultiByte(CP_UTF8,0,k,-1,kb,(int)sizeof(kb),0,0);
                                    WideCharToMultiByte(CP_UTF8,0,v,-1,vb,(int)sizeof(vb),0,0);
                                    if (ISAME(kb,"pollintervalms"))   c->pollMs=ParseIntA(vb,100,5,60000);
                                    else if (ISAME(kb,"trackintervalms")) c->trackMs=ParseIntA(vb,15,5,200);
                                    else if (ISAME(kb,"imestrategy")) c->imeStrategy=ParseIntA(vb,0,0,2);
                                    else if (ISAME(kb,"hidewhenfullscreen")) c->hideFullscreen=ParseIntA(vb,1,0,1);
                                    else if (ISAME(kb,"hidewhencomposition")) c->hideComposition=ParseIntA(vb,1,0,1);
                                    else if (ISAME(kb,"carettimeoutms")) c->caretTimeoutMs=ParseIntA(vb,150,20,5000);
                                    else if (ISAME(kb,"autohidems")) c->autoHideMs=ParseIntA(vb,1000,0,60000);
                                    else if (ISAME(kb,"autohidestates")) c->autoHideMask=ParseStatesA(vb,c->autoHideMask);
                                    else if (ISAME(kb,"wpscom")) c->wpsCom=ParseIntA(vb,1,0,1);
                                } else if (sec==1 && eq) {
                                    *eq=0;
                                    WCHAR k[CFG_NAME_MAX], v[128];
                                    _snwprintf_s(k,CFG_NAME_MAX,_TRUNCATE,L"%s",line);
                                    _snwprintf_s(v,128,_TRUNCATE,L"%s",eq+1);
                                    TrimW(k); TrimW(v);
                                    char kb[CFG_NAME_MAX*4], vb[512];
                                    WideCharToMultiByte(CP_UTF8,0,k,-1,kb,(int)sizeof(kb),0,0);
                                    WideCharToMultiByte(CP_UTF8,0,v,-1,vb,(int)sizeof(vb),0,0);
                                    if (ISAME(kb,"en"))    en   =ParseColor6(vb,en);
                                    else if (ISAME(kb,"caps"))  caps =ParseColor6(vb,caps);
                                    else if (ISAME(kb,"kbden")) kbden=ParseColor6(vb,kbden);
                                    else if (ISAME(kb,"cn"))    cn   =ParseColor6(vb,cn);
                                    else if (ISAME(kb,"jp"))    jp   =ParseColor6(vb,jp);
                                    else if (ISAME(kb,"kr"))    kr   =ParseColor6(vb,kr);
                                    else if (ISAME(kb,"alpha")) defAl=ParseIntA(vb,255,0,255);
                                } else if (sec==2 && eq) {
                                    *eq=0;
                                    WCHAR k[CFG_NAME_MAX], v[128];
                                    _snwprintf_s(k,CFG_NAME_MAX,_TRUNCATE,L"%s",line);
                                    _snwprintf_s(v,128,_TRUNCATE,L"%s",eq+1);
                                    TrimW(k); TrimW(v);
                                    char kb[CFG_NAME_MAX*4], vb[512];
                                    WideCharToMultiByte(CP_UTF8,0,k,-1,kb,(int)sizeof(kb),0,0);
                                    WideCharToMultiByte(CP_UTF8,0,v,-1,vb,(int)sizeof(vb),0,0);
                                    if (ISAME(kb,"size"))    c->size=ParseIntA(vb,9,1,64);
                                    else if (ISAME(kb,"shape")) c->shape=ParseShapeA(vb,SHAPE_CIRCLE);
                                    else if (ISAME(kb,"offsetx")) c->offsetX=ParseIntA(vb,2,-512,512);
                                    else if (ISAME(kb,"offsety")) c->offsetY=ParseIntA(vb,4,-512,512);
                                } else if (sec==3 && !eq) {
                                    if (g_ignoreCount<CFG_MAX_ENTRIES) {
                                        WCHAR tmp[CFG_NAME_MAX];
                                        _snwprintf_s(tmp,CFG_NAME_MAX,_TRUNCATE,L"%s",line);
                                        TrimW(tmp); StripExeW(tmp);
                                        if (tmp[0]) { wcscpy_s(g_ignore[g_ignoreCount],CFG_NAME_MAX,tmp); g_ignoreCount++; }
                                    }
                                }
                            }
                            line = nl? nl+1 : NULL;
                        }
                        free(wide);
                    }
                }
                free(raw);
            }
        }
        fclose(f);
    }
    c->enrgb=en; c->capsrgb=caps; c->kbdEnrgb=kbden; c->cnrgb=cn;
    c->jprgb=jp; c->krrgb=kr;
    c->dotAlpha=defAl;
}

/* ---------- 前台进程名 ---------- */
static int GetFocusProcessName(HWND focus, WCHAR* out, size_t cap) {
    if (!focus||!out||cap==0) return 0;
    out[0]=0;
    DWORD pid=0; GetWindowThreadProcessId(focus,&pid);
    if (!pid) return 0;
    HANDLE hp=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!hp) return 0;
    WCHAR exe[MAX_PATH]; DWORD sz=MAX_PATH;
    BOOL ok=QueryFullProcessImageNameW(hp,0,exe,&sz);
    CloseHandle(hp);
    if (!ok||!sz) return 0;
    WCHAR* fname=exe;
    for (WCHAR* q=exe;*q;q++) if (*q==L'\\') fname=q+1;
    _snwprintf_s(out,cap,_TRUNCATE,L"%s",fname);
    return out[0]!=0;
}

/* 前台（焦点）程序是否命中 [Ignore] 黑名单 */
int CfgBlockedForeground(void) {
    if (g_ignoreCount==0) return 0;
    extern HWND ImeFocusedWindow(void);
    HWND w=ImeFocusedWindow();
    if (!w) return 0;
    WCHAR name[MAX_PATH];
    if (!GetFocusProcessName(w,name,MAX_PATH)) return 0;
    WCHAR n[CFG_NAME_MAX];
    _snwprintf_s(n,CFG_NAME_MAX,_TRUNCATE,L"%s",name);
    StripExeW(n);
    for (int i=0;i<g_ignoreCount;i++)
        if (_wcsicmp(n,g_ignore[i])==0) return 1;
    return 0;
}