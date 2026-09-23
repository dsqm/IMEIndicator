/* wps.h —— WPS 系（文字/演示/表格）光标 COM 通道（实现在 wps.c，含完整实测注释） */
#ifndef WPS_CARET_H
#define WPS_CARET_H

#include "ime_indicator.h"

/* 三条 WPS 系光标通道。返回 1=拿到（out=屏幕物理像素）；
   -1=在 WPS 里但拿不到光标/不在编辑区 → 掐断整条探测链（它们的
   guiinfo/UIA/MSAA/IMM 全不可见或报假光标，后续通道白跑一遍）；
   0=前台不是 WPS，与本通道无关。 */
int WpsCaretText(CaretPos* out);   /* 文字 wps.exe：Word 对象模型 GetPoint        */
int WpsCaretShow(CaretPos* out);   /* 演示 wpp.exe：PowerPoint 模型 + 点→像素换算  */
int WpsCaretGrid(CaretPos* out);   /* 表格 et.exe：Excel 模型 ActiveCell + 比例换算 */

/* 降权桥子进程用：吃一帧"查哪条通道"的请求，回一帧"结果 + 矩形"。
   （本进程提权、WPS 普通权限时，上面三个函数会自己把活转给桥。） */
BOOL WpsBridgeHandler(const BYTE* req, DWORD reqLen, BYTE** resp, DWORD* respLen);

#endif /* WPS_CARET_H */
