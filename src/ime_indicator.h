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

typedef struct {
    DWORD        cnrgb;   /* 中文输入（默认不显示；设颜色则中文态显示）        */
    DWORD        enrgb;   /* 英文（默认红色）                                  */
    DWORD        capsrgb; /* 大写键 Caps Lock（默认蓝色）                      */
    DWORD        kbdEnrgb;/* 英文键盘布局（默认紫色）                          */
    DWORD        jprgb;   /* 日文输入法（默认黑色）                            */
    DWORD        krrgb;   /* 韩文输入法（默认黑色）                            */
    DWORD        dotAlpha;/* 0..255：圆点不透明度                                */
    int          size;    /* 圆点直径（像素）                                    */
    int          offsetX; /* 相对光标左缘的水平偏移（+ 右）                       */
    int          offsetY; /* 相对光标底缘的垂直偏移（+ 下）                       */
    int          pollMs;  /* 状态检测间隔                                        */
    int          trackMs; /* 光标追踪间隔                                        */
    int          imeStrategy; /* 中英判定策略：0=自动学习 1=open状态 2=转换模式    */
    int          hideFullscreen; /* 1=前景窗口全屏时不显示圆点（看视频不遮挡）    */
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

/* ---- config.c ---- */
void   CfgLoad(ImeCfg* c);      /* 读 IMEIndicator.ini（缺则写模板） */
void   CfgPath(WCHAR* out, size_t cap); /* 配置文件完整路径（托盘「打开配置」用） */
int    CfgBlockedForeground(void); /* 前台程序进程名是否命中 [Ignore] */

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

/* ---- caret.c ---- */
/* 本次坐标由哪条通道提供（日志诊断漂移用） */
typedef enum {
    CARET_NONE = 0,
    CARET_GUIINFO,   /* 经典控件 caret 矩形 */
    CARET_MSAA,      /* MSAA OBJID_CARET accLocation */
    CARET_UIA_CARET, /* UIA TextPattern2::GetCaretRange */
    CARET_UIA_SEL,   /* UIA TextPattern::GetSelection */
    CARET_IME        /* IME 组合窗口 */
} CaretSource;
typedef struct { int x, y, h; int found; CaretSource source; } CaretPos;
int    CaretGetPos(CaretPos* out);   /* 0=失败 1=成功，坐标=屏幕物理像素 */
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