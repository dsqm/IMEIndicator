/* IMEIndicator —— 输入法状态提示（光标左下角圆点）
 * COPYRIGHT (C) 2026 qm  GPL v3
 */
#ifndef IMESTATUS_H
#define IMESTATUS_H

/* 不用 WIN32_LEAN_AND_MEAN：它会把 RPC/ole2 头裁掉，导致
   `uiautomationclient.h` 里的 `interface` 关键字未定义而无法编译。 */

#include <windows.h>
#include <shellapi.h>
#include <imm.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "advapi32.lib")   /* 进程令牌：OpenProcessToken / GetTokenInformation */

/* ================= 状态模型 =================
   11 个状态 = 键盘布局 × 输入模式 × Caps Lock。
   旧版把 Caps 做成一个"盖掉一切"的独立状态，导致中文输入法下按 Caps 只能
   显示一种样式；这里把 Caps 下放成每个布局自己的变体，于是
   「中文输入法 大写」和「美式键盘 大写」可以配成完全不同的样子。

   求值顺序（main.c）：Caps 开 -> 该布局的 *_caps；否则按布局判模式。 */
typedef enum {
    IMEST_US = 0,      /* 美式键盘 普通                       */
    IMEST_US_CAPS,     /* 美式键盘 大写                       */
    IMEST_CN_CN,       /* 中文输入法 中文                     */
    IMEST_CN_EN,       /* 中文输入法 英文                     */
    IMEST_CN_CAPS,     /* 中文输入法 大写                     */
    IMEST_JP,          /* 日文输入法 普通                     */
    IMEST_JP_CAPS,     /* 日文输入法 大写                     */
    IMEST_KR,          /* 韩文输入法 普通                     */
    IMEST_KR_CAPS,     /* 韩文输入法 大写                     */
    IMEST_OTHER,       /* 其他布局 普通（法/德等非中英日韩）   */
    IMEST_OTHER_CAPS,  /* 其他布局 大写                       */
    IMEST_COUNT
} ImeState;

/* 形状。SHAPE_NONE = 该状态不显示（等价于旧版色值写 0）。
   SHAPE_IMAGE 二期实现，一期解析但渲染回退成圆。 */
#define SHAPE_NONE     0
#define SHAPE_CIRCLE   1   /* 实心圆（默认） */
#define SHAPE_TRIANGLE 2   /* 等边三角形，尖角朝上 */
#define SHAPE_IMAGE    3   /* 自定义图片（二期） */

/* 每个状态可单独覆盖的字段。用位掩码记录"这一项用户显式设过"，
   没设过的才走继承/defaults —— 否则无法区分"用户就是要 20"和"默认是 20"。 */
#define SF_ENABLED   (1u << 0)
#define SF_COLOR     (1u << 1)
#define SF_SHAPE     (1u << 2)
#define SF_IMAGE     (1u << 3)
#define SF_SIZE      (1u << 4)
#define SF_OFFSETX   (1u << 5)
#define SF_OFFSETY   (1u << 6)
#define SF_ALPHA     (1u << 7)
#define SF_TRANSIENT (1u << 8)

#define CFG_MAX_IGNORE 128
#define CFG_NAME_MAX    64
#define CFG_PATH_MAX    260

/* 一个状态的样式。尺寸/偏移都是**逻辑像素**（96 DPI 基准），
   渲染时按光标所在显示器的 DPI 换算成物理像素。 */
typedef struct {
    unsigned have;                    /* SF_* 位掩码：哪些字段是显式设过的 */
    int      enabled;                 /* 0 = 该状态不显示提示               */
    DWORD    rgb;                     /* 0x00RRGGBB                         */
    int      shape;                   /* SHAPE_*                            */
    WCHAR    image[CFG_NAME_MAX];     /* 仅 SHAPE_IMAGE：shapes\ 下的文件名 */
    int      size;                    /* 长边（逻辑像素）                    */
    int      offsetX, offsetY;        /* 相对光标左缘/底缘（逻辑像素，+ 右/+ 下） */
    int      alpha;                   /* 0..255                             */
    int      transient;               /* 1 = 状态变化后只显示一会儿          */
    int      inherit;                 /* -1 = 不继承；否则基础状态索引       */
} StateStyle;

typedef struct {
    /* ---- 全局 ---- */
    int pollMs;            /* 状态检测间隔(ms)                        */
    int trackMs;           /* 光标追踪间隔(ms)                        */
    int imeStrategy;       /* 中英判定策略：0=自动 1=open 2=转换模式    */
    int caretTimeoutMs;    /* 单次光标查询最长等待(ms)                 */
    int hideFullscreen;    /* 1=前景全屏时隐藏                        */
    int hideComposition;   /* 1=输入法组合窗出现时隐藏                 */
    int wpsCom;            /* 1=WPS 走 Office 对象模型取光标           */
    int autoHideMs;        /* 临时显示时长(ms)；0=不启用               */

    StateStyle def;                    /* defaults：未覆盖字段的兜底 */
    StateStyle st[IMEST_COUNT];        /* 每状态覆盖                 */

    WCHAR ignore[CFG_MAX_IGNORE][CFG_NAME_MAX];
    int   ignoreCount;
} ImeCfg;

/* 换窗口后，向 IME 问到的仍是**上一个窗口**的值（实测滞后 200~300ms）。
   读数要稳定这么久才算可信 —— ime.c 判稳定、检测线程收圆点共用这时长。 */
#define IME_SETTLE_MS 200

/* ---- config.c ---- */
void   CfgLoad(ImeCfg* c);              /* 读 IMEIndicator.json；缺则迁移 ini 或写默认 */
int    CfgSave(const ImeCfg* c);        /* 写回 JSON；成功返回 1 */
void   CfgPath(WCHAR* out, size_t cap); /* 配置文件完整路径 */
void   CfgDir(WCHAR* out, size_t cap);  /* exe 所在目录（不带尾反斜杠） */
void   CfgDefaults(ImeCfg* c);          /* 填出厂默认值 */
/* 把状态 s 的样式解析成最终值（继承链 + defaults），结果写进 out。
   带环保护：inherit 指回自己/成环时按"不继承"处理。 */
void   CfgStateStyle(const ImeCfg* c, ImeState s, StateStyle* out);
const WCHAR* CfgStateName(ImeState s);  /* "cn_cn" 这类 JSON 键名 */
int    CfgStateFromName(const WCHAR* name); /* 反查；认不出返回 -1 */
const WCHAR* CfgShapeName(int shape);   /* "circle" / "triangle" / "none" / "image" */
int    CfgShapeFromName(const WCHAR* name, int def);
/* 进程名（可带 .exe）是否命中黑名单。纯字符串比较，不碰窗口 API ——
   设置程序也要用它，所以不能依赖 ime.c。 */
int    CfgIsIgnored(const ImeCfg* c, const WCHAR* procName);

/* ---- debug.c ---- */
extern volatile LONG g_logging; /* 托盘「记录日志」开关（1=写 log） */
void   DbgInit(void);           /* 启动时取 exe 目录（供日志文件路径） */
void   DbgLog(const WCHAR* fmt, ...); /* 开启时追加一行日志（仅检测线程调） */
void   DbgClose(void);          /* 释放日志文件句柄（日志关闭时调，仅检测线程） */
void   DbgShutdown(void);       /* 退出时释放 */

/* ---- ime.c ---- */
int    ImeIsCapsLock(void);     /* Caps Lock 是否开启                */
int    ImeIsChineseMode(void);  /* 焦点 IME 是否处于中文组合状态        */
int    ImeIsEnglishKeyboard(void); /* 前台键盘布局是否英文语言（主语言 0x09） */
HWND   ImeFocusedWindow(void);  /* 前台线程的焦点窗口                */
int    ImeKeyboardLang(void);   /* 前台键盘布局主语言 ID（0x04/0x09/0x11/0x12…）；无前台窗口返回 -1 */
int    ProcIsElevated(void);    /* 本进程是否以管理员运行（查进程令牌） */

/* 一次中英探测拿到的原始信号与判定依据（日志诊断用）。
   不同输入法暴露的开关不一样：有的只动 open 状态、有的只动转换模式、
   有的 open 状态还不是 0/1 —— 所以要两个都取，再看哪个在变。 */
typedef struct {
    HWND  hwnd;      /* 本次参与探测的焦点窗口                     */
    int   ok;        /* 1=向 IME 窗口取到了值                      */
    int   opened;    /* IMC_GETOPENSTATUS 原始值（-1=没取到）      */
    int   conv;      /* IMC_GETCONVERSIONMODE 原始值（-1=没取到）  */
    int   strategy;  /* 0=复合兜底 1=open 状态 2=转换模式          */
    int   nonBinary; /* 1=该输入法 open 状态不是 0/1               */
    int   settling;  /* 1=刚换了窗口、读数尚未稳定（值不可信）      */
} ImeProbe;

int    ImeIsChineseModeEx(ImeProbe* p);  /* p 可为 NULL：同 ImeIsChineseMode */
void   ImeSetForcedStrategy(int s);      /* 0=自动 1=open 状态 2=转换模式   */
int    ImeFloatOccluding(HWND focus, const RECT* nearDot); /* 焦点线程上是否出现打字浮窗；nearDot=光标大致位置（可 NULL） */

/* ---- caret.c ---- */
/* 本次坐标由哪条通道提供（日志诊断漂移用） */
typedef enum {
    CARET_NONE = 0,
    CARET_GUIINFO,   /* 经典控件 caret 矩形 */
    CARET_MSAA,      /* MSAA OBJID_CARET accLocation */
    CARET_UIA_CARET, /* UIA TextPattern2::GetCaretRange */
    CARET_UIA_SEL,   /* UIA TextPattern::GetSelection */
    CARET_IME,       /* IME 组合窗口 */
    CARET_WPS,       /* WPS 文字：Word 对象模型 GetPoint（见 wps.c） */
    CARET_WPP,       /* WPS 演示：PowerPoint 对象模型 Bound* + 点→像素（见 wps.c） */
    CARET_ET,        /* WPS 表格：Excel 对象模型 ActiveCell + 点→像素（见 wps.c） */
    CARET_FLUTTER    /* Flutter：引擎锚点缓存（caret 被引擎销毁后的兜底，见 caret.c） */
} CaretSource;
typedef struct { int x, y, h, w; int found; int depth; CaretSource source; } CaretPos;
/* w 与 depth 只是**诊断信息**（w=矩形宽、depth=文本模式在焦点链上的层数，-1=该通道
   没有这个概念）：把点摆到"行首"这类漂移，只能靠"宽得像整行"与"模式来自祖先文档"
   这两个线索分辨，所以随日志一起打出来。 */

/* caret.c 与 wps.c（WPS COM 通道）共用的检测设施 */
void   LogChFail(const WCHAR* ch, const WCHAR* why, HRESULT hr); /* 通道失败日志（1 秒节流） */
UINT   WindowDpi(HWND hwnd);      /* 窗口 DPI（GetDpiForWindow，取不到回 0）       */
UINT   MonitorDpi(HWND hwnd);     /* 显示器 DPI（GetDpiForMonitor，兜底主屏 96）  */
extern volatile LONG g_gen;       /* 查询线程代际（重建 +1；wps.c 换代时丢 COM 代理） */

/* ---- caret.c ----
   光标查询会**跨进程**调 UIA/MSAA，这些调用没有任何超时机制：前台程序
   （尤其浏览器/Electron）线程一卡，调用就悬着不走，检测线程跟着冻住数秒
   （日志表现为心跳断档）。所以查询放在**独立线程**里做，调用方只按
   caretTimeoutMs 等事件；超时就放弃本轮（沿用上一轮结果），检测线程照常转。
   CaretWorkerStart 在检测线程初始化时调一次，CaretWorkerStop 在退出时调。 */
void   CaretWorkerStart(int timeoutMs);   /* 建查询线程；timeoutMs<=0 用默认值 */
void   CaretWorkerStop(void);             /* 通知线程退出并回收（最坏等一次查询） */
/* out 可为 NULL（只想知道成功与否）。timeoutOut 可传 NULL：
   返回 0 且 *timeoutOut=1 表示"本轮查询超时"（前台程序卡住），
   与"确实没有光标"要分开处理 —— 前者沿用上一轮显示，后者才藏起来。
   返回 1 时 out 里是屏幕物理像素坐标。 */
int    CaretGetPosEx(CaretPos* out, int* timeoutOut);
int    CaretIsForegroundFullscreen(void); /* 前景窗口是否全屏（日志诊断用） */
int    CaretFocusIsFlutterView(void);  /* 焦点控件是否为 Flutter 视图（main.c 组合收起时补"亮一下"用） */

/* ---- overlay.c ---- */
void   OverlayInit(void);           /* 创建透明悬浮窗（需在含消息循环的线程）*/
void   OverlayShutdown(void);       /* 销毁窗口                              */
void   OverlaySetVisible(int visible);
/* 应用一个状态的最终样式。尺寸/偏移是逻辑像素，渲染时按光标所在显示器 DPI 换算。
   样式没变时内部直接返回，不重建位图。 */
void   OverlayApply(const StateStyle* st);
void   OverlayMove(int screenX, int baselineY); /* 移窗 + 重绘 */

/* ---- main.c ---- */
extern ImeCfg g_cfg;
extern volatile LONG g_showDot;   /* 由检测线程写，主线程只读 */

#endif
