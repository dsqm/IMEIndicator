#ifndef IME_SETTINGS_INTERNAL_H
#define IME_SETTINGS_INTERNAL_H

#include "ime_indicator.h"
#include "shape.h"
#include "image.h"
#include "uipaint.h"
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <math.h>
#include <commctrl.h>
#include <commdlg.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")  /* GetOpenFileName / ChooseColor */
#pragma comment(lib, "msimg32.lib")   /* AlphaBlend：预览贴抗锯齿图形用 */

#define ID_SAVE         100
#define ID_SAVE_RESTART 101
#define ID_CLOSE        102
#define ID_ADD_IGNORE   103
#define ID_DEL_IGNORE   104
#define ID_RESET_ALL    105   /* 把所有配置项恢复出厂默认 */
#define ID_PICK_IMAGE   110   /* 110..112：三个状态块各一个「图片…」按钮 */
#define ID_RESET        120   /* 120..135：每项数值一个「重置」按钮 */
#define ID_PREVIEW_STATE 140  /* 预览条右侧的状态切换 */

#define F_ENABLED   1
#define F_INHERIT   2
#define F_COLOR     3
#define F_SHAPE     4
#define F_SIZE      5
#define F_OFFX      6
#define F_OFFY      7
#define F_ALPHA     8
#define F_TRANSIENT 9
/* 尺寸/偏移/透明度的「自定义」勾选框字段（10..13）：不勾=跟随 cn_cn */
#define F_CUSTOM_SIZE  10
#define F_CUSTOM_OFFX  11
#define F_CUSTOM_OFFY  12
#define F_CUSTOM_ALPHA 13
#define F_POLL      20
#define F_TRACK     21
#define F_STRATEGY  22
#define F_TIMEOUT   23
#define F_FULLSCREEN 24
#define F_COMPOSITION 25
#define F_WPSCOM    26
#define F_AUTOHIDE  27

#define PAGE_COUNT  7
#define PAGE_GLOBAL 5
#define PAGE_IGNORE 6

/* 逻辑坐标（96 DPI 基准） */
#define X0            14
#define LX            (X0 + 6)
#define RX            (X0 + 428)   /* 右列：两列各 398 宽，列间距 24 */
#define CONTENT_RIGHT (RX + 354 + 44)      /* 右列「重置」按钮右缘 */
#define WIN_W         (CONTENT_RIGHT + 28) /* 内容 + 左右各 14 边距 */
#define WIN_H      556
#define TAB_X      14
#define TAB_Y      8
#define TAB_H      28
#define PREVIEW_X  14
#define PREVIEW_Y  42
#define PREVIEW_H  66
/* 预览条右侧的状态切换（下拉 + 左边的说明文字） */
#define PREVIEW_SW_W      160
#define PREVIEW_SW_H      24
#define PREVIEW_LBL_W     56
#define PREVIEW_SW_MARGIN 10
#define CONTENT_Y  116
#define BOTTOM_H   44

/* 强调色（Windows 10 默认蓝）：主按钮、复选框、滑块、页签下划线共用 */
#define ACCENT_RGB RGB(0x00, 0x78, 0xD7)

#define BTN_HOVER_PROP L"IMEIndicatorBtnHover"
#define CHK_PROP       L"IMEIndicatorCheck"      /* 是自绘复选框 */
#define CHK_STATE_PROP L"IMEIndicatorCheckState" /* 复选框的勾选状态 */
#define HINT_PROP      L"IMEIndicatorHint"       /* 悬停说明的文字（静态字面量） */
#define HINT_CLASS     L"IMEIndicatorHintWnd"
#define HINT_TIMER     1
#define HINT_DELAY_MS  400

#define SEL_MAX_ITEMS 16
#define SEL_MAX_TEXT  32
#define SEL_ITEM_H    26
#define SEL_CLASS     L"IMEIndicatorSelect"
#define SEL_POPUP     L"IMEIndicatorSelectPopup"

#define SHAPE_ORDER_COUNT (int)(sizeof(kShapeOrder) / sizeof(kShapeOrder[0]))

typedef struct {
    const WCHAR* name;
    ImeState     states[3];
} PageDef;

typedef struct {
    HDC     mem;
    HBITMAP bmp;
    HGDIOBJ old;
    int     w, h;
} BufDC;

typedef struct {
    HWND hwnd;
    int  block;   /* 布局块 0..2；-1 全局页；-2 纯装饰 */
    int  field;
    HWND numEdit;
    int  x, y, w, h;
} Meta;

typedef struct {
    int   block, field;
    int   count, sel, hot;
    WCHAR items[SEL_MAX_ITEMS][SEL_MAX_TEXT];
} SelData;

typedef struct {
    HWND title, enable, inherit, color, shape;
    HWND imgBtn, imgLabel;
    HWND sep;         /* 块底分隔线：最后一块的分隔线不显示（会变成悬空线） */
    HWND lbl[6];      /* 颜色/形状/尺寸/偏移X/偏移Y/透明度 的静态标签 */
    HWND reset[4];    /* 尺寸/偏移X/偏移Y/透明度 的重置按钮 */
    HWND custom[4];   /* 尺寸/偏移X/偏移Y/透明度 行首的勾选框（勾上=本状态单独设） */
    HWND slSize, vSize, slOffX, vOffX, slOffY, vOffY, slAlpha, vAlpha;
    HWND transient;
} Block;

extern ImeCfg  s_cfg;
extern HWND    g_hwnd;
extern HWND    g_preview;
extern HWND    g_swState;
extern int     g_page;
extern int     g_selState;
extern HFONT   g_font;
extern HBRUSH  g_whiteBrush;
extern int     g_dpi;
extern HFONT   g_fontBold;
extern Meta    g_meta[200];
extern int     g_metaCount;
extern int     g_syncEdit;
extern HWND    g_imgLabel[3];
extern int     g_imgBad[3];
extern HWND    g_colorLabel[3];
extern int     g_colorDim[3];
extern HWND    g_customLabel[3][4];
extern int     g_customDim[3][4];
extern HWND    g_parent;
extern Block   g_blk[3];
extern ImeState g_blkState[3];
extern int     g_lastShape[IMEST_COUNT];
extern HWND    g_globalPanel;
extern HWND    g_ignorePanel;
extern HWND    g_slPoll, g_vPoll, g_slTrack, g_vTrack, g_slTimeout, g_vTimeout,
               g_slAutoHide, g_vAutoHide, g_cbStrategy, g_ckFull, g_ckComp, g_ckWps;
extern HWND    g_ignoreList, g_ignoreEdit;
extern const PageDef kPages[PAGE_COUNT];
extern const int kShapeOrder[12];
extern const int kStateFields[4];
extern const int kGlobalFields[4];

int DpiScale(int v);
void ClientToWindowSize(RECT* rc, DWORD style, DWORD exstyle, UINT dpi);
HFONT BaseFont(void);
HFONT BoldFont(void);
void PaintRoundRectAA(HDC dc, const RECT* rc, int radius,
                      COLORREF border, COLORREF fill);
int BufBegin(BufDC* b, HDC ref, int w, int h);
void BufEnd(BufDC* b, HDC dc);
void HintHide(void);
LRESULT CALLBACK HintProc(HWND h, UINT m, WPARAM w, LPARAM l);
void MakePushButton(HWND h);
void PaintPushButton(const DRAWITEMSTRUCT* di);
void PaintCheck(const DRAWITEMSTRUCT* di);
void RegMeta(HWND h, int block, int field, HWND numEdit, int x, int y, int w, int hh);
Meta* FindMeta(HWND h);
int SliderPos(HWND s);
void SetSliderPos(HWND sl, int v);
void SetNumText(HWND e, int v);
void ScaleControls(void);
void RegisterPanelClass(HINSTANCE hi);
HWND MakePanel(void);
HWND CLabel(const WCHAR* t, int x, int y, int w, int h);
HWND CCheck(const WCHAR* t, int x, int y, int w, int h,
            int block, int field, int checked);
HWND CButton(const WCHAR* t, int x, int y, int w, int h, int id);
HWND CEdit(int x, int y, int w, int h);
HWND CEditNum(int x, int y, int w, int h, int block, int field);
HWND CCombo(int x, int y, int w, int h, int block, int field);
HWND CColorBtn(int x, int y, int w, int h, int block);
HWND CSlider(int x, int y, int w, int h, int block, int field,
             int lo, int hi, int val, HWND numEdit);
LRESULT CALLBACK SelPopupProc(HWND h, UINT m, WPARAM w, LPARAM l);
LRESULT CALLBACK SelProc(HWND h, UINT m, WPARAM w, LPARAM l);
unsigned CustomBit(int field);
int IsCapsState(ImeState s);
int CapsBase(ImeState s);
void BuildStateBlock(int i, int y);
void FillBlock(int i, ImeState s);
void SyncLastShapes(void);
int FallbackBadImages(void);
void BuildGlobalPanel(void);
void FillGlobalPanel(void);
void BuildIgnorePanel(void);
void FillIgnorePanel(void);
void InvalidatePreview(void);
ImeState PreviewStateAt(int idx);
void SyncPreviewSwitch(void);
void StepPreviewState(int dir);
LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l);

#endif
