/* IMEStatus —— 输入法状态提示（光标左下角圆点）
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

/* 颜色以 0x00RRGGBB 存放（不透明）；透明度在 overlay 里统一换算。
   通用规则：色值为 0 表示该状态不显示圆点（En/Caps/KbdEn/Cn 一致）。 */
typedef struct {
    DWORD        cnrgb;   /* 中文输入（默认 0=不显示；设颜色则中文态显示）      */
    DWORD        enrgb;   /* 英文（默认橘色；0=不显示）                        */
    DWORD        capsrgb; /* 大写键 Caps Lock（默认蓝色；0=不显示）            */
    DWORD        kbdEnrgb;/* 英文键盘布局（默认紫色；0=不显示）               */
    DWORD        dotAlpha;/* 0..255：圆点不透明度                                */
    int          size;    /* 圆点直径（像素）                                    */
    int          offsetX; /* 相对光标左缘的水平偏移（+ 右）                       */
    int          offsetY; /* 相对光标底缘的垂直偏移（+ 下）                       */
    int          pollMs;  /* 状态检测间隔                                        */
    int          trackMs; /* 光标追踪间隔                                        */
} ImeCfg;

/* 状态判定结果 */
typedef enum {
    IMEST_EN = 0,      /* 英文（中文输入法的英文档）       */
    IMEST_CAPS,        /* 大写键 Caps Lock                 */
    IMEST_KBD_EN,      /* 英文键盘布局                     */
    IMEST_CN,          /* 中文输入                          */
    IMEST_COUNT
} ImeState;

/* ---- config.c ---- */
void   CfgLoad(ImeCfg* c);      /* 读 IMEStatus.ini（缺则写模板） */
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