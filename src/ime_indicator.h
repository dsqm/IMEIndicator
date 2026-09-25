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

/* 颜色以 0x00RRGGBB 存放（不透明）；透明度统一用 Alpha。
   ★ "不显示该状态"用哨兵 IME_COLOR_NONE 表示，不能用 0 —— 0 是**黑色**，
   否则想设成黑色就只能关掉该状态。 */
#define IME_COLOR_NONE 0xFFFFFFFFu

/* 圆点形状（配置 Shape）：两者都是"尖角朝上"的对称形状，尺寸含义一致
   —— Size 都是外接方形边长，所以换形状不用重新调大小。 */
#define SHAPE_CIRCLE   0   /* 实心圆（默认） */
#define SHAPE_TRIANGLE 1   /* 等边三角形，尖角朝上 */

typedef struct {
    DWORD        cnrgb;   /* 中文输入（默认不显示；设颜色则中文态显示）        */
    DWORD        enrgb;   /* 英文（默认红色）                                  */
    DWORD        capsrgb; /* 大写键 Caps Lock（默认蓝色）                      */
    DWORD        kbdEnrgb;/* 英文键盘布局（默认紫色）                          */
    DWORD        jprgb;   /* 日文输入法（默认黑色）                            */
    DWORD        krrgb;   /* 韩文输入法（默认黑色）                            */
    DWORD        dotAlpha;/* 0..255：圆点不透明度                                */
    int          size;    /* 圆点直径（像素）                                    */
    int          shape;    /* SHAPE_CIRCLE / SHAPE_TRIANGLE                      */
    int          offsetX; /* 相对光标左缘的水平偏移（+ 右）                       */
    int          offsetY; /* 相对光标底缘的垂直偏移（+ 下）                       */
    int          pollMs;  /* 状态检测间隔                                        */
    int          trackMs; /* 光标追踪间隔                                        */
    int          imeStrategy; /* 中英判定策略：0=自动学习 1=open状态 2=转换模式    */
    int          hideFullscreen; /* 1=前景窗口全屏时不显示圆点（看视频不遮挡）    */
    int          hideComposition;/* 1=输入法组合窗显示时隐藏圆点（它贴在光标处）  */
    int          caretTimeoutMs; /* 单次光标查询最长等待(ms)：超时即放弃本轮，
                                    前台程序卡住时不让检测线程跟着冻住        */
    int          autoHideMs;    /* "临时显示"时长(ms)：清单里的状态只显示这么久
                                    就自动消失；0=不用临时显示（全部常显）      */
    int          autoHideMask;  /* 参与临时显示的状态集合：1<<ImeState 位掩码    */
    int          wpsCom;        /* 1=WPS 文字走 Word 对象模型（COM）取光标位置    */
} ImeCfg;

/* 换窗口后，向 IME 问到的仍是**上一个窗口**的值（实测滞后 200~300ms）。
   读数要稳定这么久才算可信 —— ime.c 判稳定、检测线程收圆点共用这时长。 */
#define IME_SETTLE_MS 200

/* 状态判定结果 */typedef enum {
    IMEST_EN = 0,      /* 英文（中文输入法的英文档）       */
    IMEST_CAPS,        /* 大写键 Caps Lock                 */
    IMEST_KBD_EN,      /* 英文键盘布局                     */
    IMEST_CN,          /* 中文输入                          */
    IMEST_JP,          /* 日文输入法（键盘布局主语言 0x11） */
    IMEST_KR,          /* 韩文输入法（键盘布局主语言 0x12） */
    IMEST_COUNT
} ImeState;

/* AutoHideStates 在内存里按位掩码存（每状态 1 位） */
#define IME_AH_BIT(s) (1u << (int)(s))
#define IME_AH_ALL    ((1u << (int)IMEST_COUNT) - 1u)

/* ---- config.c ---- */
void   CfgLoad(ImeCfg* c);      /* 读 IMEIndicator.ini（缺则写模板） */
void   CfgPath(WCHAR* out, size_t cap); /* 配置文件完整路径（托盘「打开配置」用） */
int    CfgBlockedForeground(void); /* 前台命中 [Ignore]，或进程名读不到（受保护进程） */

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
    CARET_ET         /* WPS 表格：Excel 对象模型 ActiveCell + 点→像素（见 wps.c） */
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

/* ---- overlay.c ---- */
void   OverlayInit(ImeCfg* c);      /* 创建透明悬浮窗（需在含消息循环的线程）*/
void   OverlayShutdown(void);       /* 销毁窗口                              */
void   OverlaySetVisible(int visible);
void   OverlaySetColor(DWORD rgb, DWORD alpha);
void   OverlayMove(int screenX, int baselineY); /* 移窗 + 重绘 */
/* ---- main.c ---- */
extern ImeCfg g_cfg;
extern volatile LONG g_showDot;   /* 由检测线程写，主线程只读 */

#endif