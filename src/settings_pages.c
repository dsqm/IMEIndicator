#include "settings_internal.h"

/* 重置按钮按「块 × 字段」编号，这里给出两组的字段顺序 */
const int kStateFields[4]  = { F_SIZE, F_OFFX, F_OFFY, F_ALPHA };
const int kGlobalFields[4] = { F_POLL, F_TRACK, F_TIMEOUT, F_AUTOHIDE };

/* 尺寸/偏移/透明度 四项对应的"显式设过"位；顺序与 Block 里的滑块一致 */
static const unsigned kNumBits[4] = { SF_SIZE, SF_OFFSETX, SF_OFFSETY, SF_ALPHA };

/* 「自定义」勾选框字段 -> 对应数值项的 have 位；不是这类字段返回 0 */
unsigned CustomBit(int field) {
    switch (field) {
    case F_CUSTOM_SIZE:  return SF_SIZE;
    case F_CUSTOM_OFFX:  return SF_OFFSETX;
    case F_CUSTOM_OFFY:  return SF_OFFSETY;
    case F_CUSTOM_ALPHA: return SF_ALPHA;
    }
    return 0;
}

/* 形状下拉的顺序与标签：按视觉相似度排，图片放最后。
   顺序与 SHAPE_* 枚举值无关，配置里存的是名字。 */
const int kShapeOrder[] = {
    SHAPE_NONE, SHAPE_CIRCLE, SHAPE_SEMICIRCLE, SHAPE_RING, SHAPE_ROUNDED,
    SHAPE_SQUARE, SHAPE_DIAMOND, SHAPE_TRIANGLE, SHAPE_TRAPEZOID,
    SHAPE_HEXAGON, SHAPE_SPARKLE, SHAPE_IMAGE
};
static const WCHAR* const kShapeLabels[] = {
    L"不显示", L"圆形", L"半圆", L"圆环", L"圆角方形",
    L"方形", L"菱形", L"三角形", L"梯形", L"六边形", L"四角星", L"图片"
};

/* ---------- 页定义 ---------- */

const PageDef kPages[PAGE_COUNT] = {
    /* 中文输入法排第一：绝大多数人只调它，打开设置默认就落在这一页。
       其余顺序不变；「全局」「黑名单」必须在最后（PAGE_GLOBAL / PAGE_IGNORE）。 */
    { L"中文输入法", { IMEST_CN_CN, IMEST_CN_EN, IMEST_CN_CAPS } },
    { L"美式键盘",   { IMEST_US, IMEST_US_CAPS, (ImeState)-1 } },
    { L"日文输入法", { IMEST_JP, IMEST_JP_CAPS, (ImeState)-1 } },
    { L"韩文输入法", { IMEST_KR, IMEST_KR_CAPS, (ImeState)-1 } },
    { L"其他布局",   { IMEST_OTHER, IMEST_OTHER_CAPS, (ImeState)-1 } },
    { L"全局",       { (ImeState)-1, (ImeState)-1, (ImeState)-1 } },
    { L"黑名单",     { (ImeState)-1, (ImeState)-1, (ImeState)-1 } },
};

static const WCHAR* StateLabel(ImeState s) {
    switch (s) {
    case IMEST_CN_CN: return L"中文";
    case IMEST_CN_EN: return L"英文";
    case IMEST_US_CAPS: case IMEST_CN_CAPS: case IMEST_JP_CAPS:
    case IMEST_KR_CAPS: case IMEST_OTHER_CAPS: return L"大写";
    default: return L"普通";
    }
}

int IsCapsState(ImeState s) {
    return s == IMEST_US_CAPS || s == IMEST_CN_CAPS || s == IMEST_JP_CAPS ||
           s == IMEST_KR_CAPS || s == IMEST_OTHER_CAPS;
}

int CapsBase(ImeState s) {
    switch (s) {
    case IMEST_US_CAPS:    return IMEST_US;
    case IMEST_CN_CAPS:    return IMEST_CN_CN;
    case IMEST_JP_CAPS:    return IMEST_JP;
    case IMEST_KR_CAPS:    return IMEST_KR;
    case IMEST_OTHER_CAPS: return IMEST_OTHER;
    default:               return -1;
    }
}

/* ---------- 布局面板：3 个固定块 ---------- */

Block g_blk[3];
ImeState g_blkState[3] = { (ImeState)-1, (ImeState)-1, (ImeState)-1 };

/* 每个状态「上一次选择的非图片形状」：形状=图片但没选到可用图片时，
   保存时回退到它，避免存下一个显示不出来的配置。 */
int g_lastShape[IMEST_COUNT];

HWND  g_globalPanel = NULL;
HWND  g_ignorePanel = NULL;

/* 行首勾选框的悬停说明（静态存储：气泡只存指针）。只讲未勾选时的效果。 */
static WCHAR kHintCustom[] = L"未勾选时，数值跟随「中文输入法 (cn_cn)」";

void BuildStateBlock(int i, int y) {
    Block* b = &g_blk[i];
    const int x0 = X0;
    const int lx = LX;
    const int rx = RX;

    b->title = CLabel(L"", lx, y + 2, 150, 20);
    SendMessageW(b->title, WM_SETFONT, (WPARAM)BoldFont(), TRUE);
    b->enable = CCheck(L"启用", lx + 156, y, 60, 22, i, F_ENABLED, 1);
    b->inherit = CCheck(L"跟随基础状态", lx + 220, y, 120, 22, i, F_INHERIT, 0);
    y += 24;

    /* 形状在左、颜色在右；标签与下面数值行的标签对齐（cx + 22） */
    b->lbl[0] = CLabel(L"颜色", rx + 22, y + 4, 54, 20);
    g_colorLabel[i] = b->lbl[0];
    b->color = CColorBtn(rx + 80, y, 60, 22, i);
    b->lbl[1] = CLabel(L"形状", lx + 22, y + 4, 54, 20);
    b->shape = CCombo(lx + 80, y, 110, 180, i, F_SHAPE);
    for (int k = 0; k < SHAPE_ORDER_COUNT; k++)
        SendMessageW(b->shape, CB_ADDSTRING, 0, (LPARAM)kShapeLabels[k]);
    /* 「图片…」按钮：选文件 -> 复制进 shapes\ -> 形状自动切到图片。
       注册成 block=i 的控件，跟随基础状态时会被整块置灰。 */
    b->imgBtn = CreateWindowExW(0, L"BUTTON", L"图片…",
                                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                lx + 196, y, 76, 22, g_parent,
                                (HMENU)(INT_PTR)(ID_PICK_IMAGE + i), NULL, NULL);
    SendMessageW(b->imgBtn, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    MakePushButton(b->imgBtn);
    RegMeta(b->imgBtn, i, 0, NULL, lx + 196, y, 76, 22);
    b->imgLabel = CLabel(L"", lx + 278, y + 4, 120, 20);
    g_imgLabel[i] = b->imgLabel;
    y += 26;

    struct { const WCHAR* nm; int field; int custom; int lo, hi; } sl[4] = {
        { L"尺寸",   F_SIZE,  F_CUSTOM_SIZE,   4,  96 },
        { L"偏移 X", F_OFFX,  F_CUSTOM_OFFX, -40,  40 },
        { L"偏移 Y", F_OFFY,  F_CUSTOM_OFFY, -40,  40 },
        { L"透明度", F_ALPHA, F_CUSTOM_ALPHA,  0, 255 },
    };
    HWND* psl[4] = { &b->slSize, &b->slOffX, &b->slOffY, &b->slAlpha };
    HWND* pvl[4] = { &b->vSize,  &b->vOffX,  &b->vOffY,  &b->vAlpha };
    /* 视觉排布：左列 尺寸/透明度，右列 偏移Y/偏移X —— 两个偏移放同一列方便一起调。
       k 仍是字段下标（尺寸/偏移X/偏移Y/透明度），这里只换摆放位置。 */
    static const int kSlot[4] = { 0, 3, 1, 2 };  /* k -> 槽位：0 左上, 1 右上, 2 左下, 3 右下 */
    for (int k = 0; k < 4; k++) {
        int slot = kSlot[k];
        int cx = (slot % 2 == 0) ? lx : rx;
        int cy = y + (slot / 2) * 23;
        /* 行首一个**没有文字**的勾选框：不勾 = 跟随中文输入法(cn_cn)的这项值
           （滑块/数值/重置整行置灰），勾上 = 本状态单独设置。含义靠悬停气泡说明。
           挂在 block=i 上，整块跟随基础状态时会一起置灰。 */
        b->custom[k] = CCheck(L"", cx, cy, 18, 22, i, sl[k].custom, 0);
        SetPropW(b->custom[k], HINT_PROP, (HANDLE)kHintCustom);
        b->lbl[2 + k] = CLabel(sl[k].nm, cx + 22, cy + 3, 54, 20);
        g_customLabel[i][k] = b->lbl[2 + k];
        *pvl[k] = CEditNum(cx + 302, cy, 48, 22, i, sl[k].field);
        *psl[k] = CSlider(cx + 78, cy, 220, 22, i, sl[k].field,
                          sl[k].lo, sl[k].hi, sl[k].lo, *pvl[k]);
        b->reset[k] = CreateWindowExW(0, L"BUTTON", L"重置",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      cx + 354, cy, 44, 22, g_parent,
                                      (HMENU)(INT_PTR)(ID_RESET + i * 4 + k), NULL, NULL);
        SendMessageW(b->reset[k], WM_SETFONT, (WPARAM)BaseFont(), TRUE);
        MakePushButton(b->reset[k]);
        RegMeta(b->reset[k], i, 0, NULL, cx + 354, cy, 44, 22);
    }
    y += 46;

    b->transient = CCheck(L"", lx, y, 330, 22, i, F_TRANSIENT, 0);
    y += 22;

    HWND sep = CreateWindowExW(0, L"STATIC", NULL,
        WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, x0, y + 2, WIN_W - 28, 2,
        g_parent, NULL, NULL, NULL);
    RegMeta(sep, -2, 0, NULL, x0, y + 2, WIN_W - 28, 2);
    b->sep = sep;
}

/* 隐藏一个块的全部控件 */
static void HideBlock(int i) {
    Block* b = &g_blk[i];
    HWND all[] = { b->title, b->enable, b->inherit, b->color, b->shape,
                   b->imgBtn, b->imgLabel,
                   b->slSize, b->vSize, b->slOffX, b->vOffX,
                   b->slOffY, b->vOffY, b->slAlpha, b->vAlpha, b->transient };
    for (size_t k = 0; k < sizeof(all)/sizeof(all[0]); k++)
        if (all[k]) ShowWindow(all[k], SW_HIDE);
    for (int k = 0; k < 6; k++)
        if (b->lbl[k]) ShowWindow(b->lbl[k], SW_HIDE);
    for (int k = 0; k < 4; k++)
        if (b->reset[k]) ShowWindow(b->reset[k], SW_HIDE);
    for (int k = 0; k < 4; k++)
        if (b->custom[k]) ShowWindow(b->custom[k], SW_HIDE);
    if (b->sep) ShowWindow(b->sep, SW_HIDE);
    g_blkState[i] = (ImeState)-1;
}

static void ShowBlock(int i) {
    Block* b = &g_blk[i];
    HWND all[] = { b->title, b->enable, b->color, b->shape,
                   b->imgBtn, b->imgLabel,
                   b->slSize, b->vSize, b->slOffX, b->vOffX,
                   b->slOffY, b->vOffY, b->slAlpha, b->vAlpha, b->transient };
    for (size_t k = 0; k < sizeof(all)/sizeof(all[0]); k++)
        if (all[k]) ShowWindow(all[k], SW_SHOW);
    for (int k = 0; k < 6; k++)
        if (b->lbl[k]) ShowWindow(b->lbl[k], SW_SHOW);
    for (int k = 0; k < 4; k++)
        if (b->reset[k]) ShowWindow(b->reset[k], SW_SHOW);
    for (int k = 0; k < 4; k++)
        if (b->custom[k]) ShowWindow(b->custom[k], SW_SHOW);
    if (b->sep) ShowWindow(b->sep, SW_SHOW);
    /* 跟随开关只对 caps 状态有意义；非 caps 必须显式隐藏，
       否则会留着上一页的状态继续显示 */
    ShowWindow(b->inherit, IsCapsState(g_blkState[i]) ? SW_SHOW : SW_HIDE);
}

/* 把某个状态的内容填进第 i 块；s < 0 表示该块本页不用 */
void FillBlock(int i, ImeState s) {
    if ((int)s < 0) { HideBlock(i); return; }
    Block* b = &g_blk[i];
    g_blkState[i] = s;

    StateStyle* raw = &s_cfg.st[s];
    StateStyle eff;
    CfgStateStyle(&s_cfg, s, &eff);
    int isCaps = IsCapsState(s);
    int inherited = isCaps && raw->inherit >= 0;

    WCHAR title[160];
    _snwprintf_s(title, 160, _TRUNCATE, L"%s（%s）", StateLabel(s), CfgStateName(s));
    SetWindowTextW(b->title, title);

    SendMessageW(b->enable, BM_SETCHECK, raw->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(b->inherit, BM_SETCHECK, inherited ? BST_CHECKED : BST_UNCHECKED, 0);
    int shapeSel = 1;   /* 认不出就落到圆形 */
    for (int k = 0; k < SHAPE_ORDER_COUNT; k++)
        if (kShapeOrder[k] == eff.shape) { shapeSel = k; break; }
    SendMessageW(b->shape, CB_SETCURSEL, shapeSel, 0);

    SetSliderPos(b->slSize, eff.size);
    SetSliderPos(b->slOffX, eff.offsetX);
    SetSliderPos(b->slOffY, eff.offsetY);
    SetSliderPos(b->slAlpha, eff.alpha);
    WCHAR vb[64];
    SetNumText(b->vSize, eff.size);
    SetNumText(b->vOffX, eff.offsetX);
    SetNumText(b->vOffY, eff.offsetY);
    SetNumText(b->vAlpha, eff.alpha);

    if (s_cfg.autoHideMs > 0)
        _snwprintf_s(vb, 64, _TRUNCATE, L"状态变化后只显示 %d ms", s_cfg.autoHideMs);
    else
        _snwprintf_s(vb, 64, _TRUNCATE, L"状态变化后常显（全局已关闭）");
    SetWindowTextW(b->transient, vb);
    SendMessageW(b->transient, BM_SETCHECK, eff.transient ? BST_CHECKED : BST_UNCHECKED, 0);

    ShowBlock(i);

    /* 图片行：只在形状=图片时露文件名与选择按钮；文件缺失/解码失败标红 */
    g_imgBad[i] = 0;
    if (eff.shape == SHAPE_IMAGE) {
        ShowWindow(b->imgBtn, SW_SHOW);
        ShowWindow(b->imgLabel, SW_SHOW);
        ImgView iv;
        WCHAR err[128];
        if (eff.image[0] && ImgGet(eff.image, 64, &iv, err, 128)) {
            SetWindowTextW(b->imgLabel, eff.image);
        } else {
            SetWindowTextW(b->imgLabel, eff.image[0] ? L"图片不可用" : L"未选择图片");
            g_imgBad[i] = 1;
        }
    } else {
        ShowWindow(b->imgBtn, SW_HIDE);
        ShowWindow(b->imgLabel, SW_HIDE);
    }
    InvalidateRect(b->imgLabel, NULL, TRUE);

    /* 跟随中：除「启用 / 跟随」外整块置灰 */
    for (int k = 0; k < g_metaCount; k++) {
        Meta* m = &g_meta[k];
        if (m->block != i) continue;
        if (m->field == F_INHERIT || m->field == F_ENABLED) continue;
        EnableWindow(m->hwnd, inherited ? FALSE : TRUE);
        if (m->numEdit) EnableWindow(m->numEdit, inherited ? FALSE : TRUE);
    }

    /* 形状=图片：图片原样使用、不染色，颜色项整行置灰（跟随中已整块置灰） */
    int colorOn = !inherited && eff.shape != SHAPE_IMAGE;
    EnableWindow(b->color, colorOn ? TRUE : FALSE);
    if (b->lbl[0]) EnableWindow(b->lbl[0], colorOn ? TRUE : FALSE);
    g_colorDim[i] = colorOn ? 0 : 1;
    if (b->lbl[0]) InvalidateRect(b->lbl[0], NULL, FALSE);
    InvalidateRect(b->color, NULL, TRUE);

    /* 行首勾选框：不勾 = 跟随 cn_cn（滑块/数值/重置置灰，标签也变灰）。
       整块跟随基础状态时轮不到它，cn_cn 自己更是来源 —— 都藏掉。 */
    int customizable = (s != IMEST_CN_CN) && !inherited;
    HWND* psl[4] = { &b->slSize, &b->slOffX, &b->slOffY, &b->slAlpha };
    HWND* pvl[4] = { &b->vSize,  &b->vOffX,  &b->vOffY,  &b->vAlpha };
    for (int k = 0; k < 4; k++) {
        if (!b->custom[k]) continue;
        if (!customizable) {
            ShowWindow(b->custom[k], SW_HIDE);
            g_customDim[i][k] = 0;
            continue;
        }
        ShowWindow(b->custom[k], SW_SHOW);
        int custom = !CfgFieldFollowsCn(&s_cfg, s, kNumBits[k]);
        SendMessageW(b->custom[k], BM_SETCHECK,
                     custom ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(b->custom[k], TRUE);
        EnableWindow(*psl[k], custom ? TRUE : FALSE);
        EnableWindow(*pvl[k], custom ? TRUE : FALSE);
        EnableWindow(b->reset[k], custom ? TRUE : FALSE);
        g_customDim[i][k] = custom ? 0 : 1;
        if (b->lbl[2 + k]) InvalidateRect(b->lbl[2 + k], NULL, FALSE);
    }
}

/* 从当前配置同步「上一次选择的形状」：图片本身不算，回落到圆 */
void SyncLastShapes(void) {
    for (int i = 0; i < IMEST_COUNT; i++) {
        StateStyle eff;
        CfgStateStyle(&s_cfg, (ImeState)i, &eff);
        g_lastShape[i] = (eff.shape == SHAPE_IMAGE) ? SHAPE_CIRCLE : eff.shape;
    }
}

/* 形状=图片但没选到可用图片（没选 / 文件缺失 / 解码失败）时，回退到
   上一次选择的形状。保存前调用：存下一个显示不出来的配置没有意义。
   返回 1 表示有状态被改。 */
int FallbackBadImages(void) {
    int changed = 0;
    for (int i = 0; i < IMEST_COUNT; i++) {
        StateStyle* raw = &s_cfg.st[i];
        if (raw->inherit >= 0) continue;      /* 跟随基础状态：由基础状态兜底 */
        StateStyle eff;
        CfgStateStyle(&s_cfg, (ImeState)i, &eff);
        if (eff.shape != SHAPE_IMAGE) continue;
        ImgView iv;
        if (eff.image[0] && ImgGet(eff.image, 64, &iv, NULL, 0)) continue;
        int back = g_lastShape[i];
        if (back == SHAPE_IMAGE) back = SHAPE_CIRCLE;
        raw->shape = back;
        raw->have |= SF_SHAPE;
        changed = 1;
    }
    return changed;
}

/* ---------- 全局面板 ---------- */
HWND g_slPoll, g_vPoll, g_slTrack, g_vTrack, g_slTimeout, g_vTimeout,
            g_slAutoHide, g_vAutoHide, g_cbStrategy, g_ckFull, g_ckComp, g_ckWps;

void BuildGlobalPanel(void) {
    g_parent = g_globalPanel;
    int y = 0, x0 = 14;
    HWND hDetect = CLabel(L"检测", x0, y, 200, 22);
    SendMessageW(hDetect, WM_SETFONT, (WPARAM)BoldFont(), TRUE);
    y += 24;

    struct { const WCHAR* nm; int field; int lo, hi; HWND* sl; HWND* vl; } sl[4] = {
        { L"状态检测间隔", F_POLL,      5, 2000,  &g_slPoll,    &g_vPoll },
        { L"光标追踪间隔", F_TRACK,     5, 200,   &g_slTrack,   &g_vTrack },
        { L"光标查询超时", F_TIMEOUT,  20, 2000,  &g_slTimeout, &g_vTimeout },
        { L"临时显示时长", F_AUTOHIDE,  0, 10000, &g_slAutoHide,&g_vAutoHide },
    };
    int vals[4] = { s_cfg.pollMs, s_cfg.trackMs, s_cfg.caretTimeoutMs, s_cfg.autoHideMs };
    for (int i = 0; i < 4; i++) {
        CLabel(sl[i].nm, x0 + 6, y + 3, 110, 20);
        *sl[i].vl = CEditNum(x0 + 346, y, 64, 22, -1, sl[i].field);
        CLabel(L"ms", x0 + 414, y + 3, 30, 20);
        *sl[i].sl = CSlider(x0 + 120, y, 220, 22, -1, sl[i].field,
                            sl[i].lo, sl[i].hi, vals[i], *sl[i].vl);
        HWND rb = CreateWindowExW(0, L"BUTTON", L"重置",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  x0 + 448, y, 44, 22, g_parent,
                                  (HMENU)(INT_PTR)(ID_RESET + 12 + i), NULL, NULL);
        SendMessageW(rb, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
        MakePushButton(rb);
        RegMeta(rb, -1, 0, NULL, x0 + 448, y, 44, 22);
        y += 26;
    }

    y += 8;
    CLabel(L"中英判定策略", x0 + 6, y + 3, 110, 20);
    g_cbStrategy = CCombo(x0 + 120, y, 220, 180, -1, F_STRATEGY);
    SendMessageW(g_cbStrategy, CB_ADDSTRING, 0, (LPARAM)L"自动学习（推荐）");
    SendMessageW(g_cbStrategy, CB_ADDSTRING, 0, (LPARAM)L"只看 IME open 状态");
    SendMessageW(g_cbStrategy, CB_ADDSTRING, 0, (LPARAM)L"只看转换模式");
    SendMessageW(g_cbStrategy, CB_SETCURSEL, s_cfg.imeStrategy, 0);
    y += 34;

    HWND hHide = CLabel(L"隐藏", x0, y, 200, 22);
    SendMessageW(hHide, WM_SETFONT, (WPARAM)BoldFont(), TRUE);
    y += 24;
    g_ckFull = CCheck(L"前景窗口全屏时隐藏", x0 + 6, y, 300, 22, -1, F_FULLSCREEN, s_cfg.hideFullscreen);
    y += 24;
    g_ckComp = CCheck(L"输入法组合窗出现时隐藏", x0 + 6, y, 300, 22, -1, F_COMPOSITION, s_cfg.hideComposition);
    y += 24;
    g_ckWps = CCheck(L"启用 WPS 光标通道", x0 + 6, y, 300, 22, -1, F_WPSCOM, s_cfg.wpsCom);
    y += 34;

    CLabel(L"尺寸与偏移按 96 DPI 的逻辑像素计算，高 DPI 屏上会自动放大。",
           x0, y, 640, 40);
}

void FillGlobalPanel(void) {
    SetSliderPos(g_slPoll, s_cfg.pollMs);
    SetSliderPos(g_slTrack, s_cfg.trackMs);
    SetSliderPos(g_slTimeout, s_cfg.caretTimeoutMs);
    SetSliderPos(g_slAutoHide, s_cfg.autoHideMs);
    SetNumText(g_vPoll, s_cfg.pollMs);
    SetNumText(g_vTrack, s_cfg.trackMs);
    SetNumText(g_vTimeout, s_cfg.caretTimeoutMs);
    SetNumText(g_vAutoHide, s_cfg.autoHideMs);
    SendMessageW(g_cbStrategy, CB_SETCURSEL, s_cfg.imeStrategy, 0);
    SendMessageW(g_ckFull, BM_SETCHECK, s_cfg.hideFullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_ckComp, BM_SETCHECK, s_cfg.hideComposition ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_ckWps, BM_SETCHECK, s_cfg.wpsCom ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* ---------- 黑名单面板 ---------- */
HWND g_ignoreList = NULL, g_ignoreEdit = NULL;

void BuildIgnorePanel(void) {
    g_parent = g_ignorePanel;
    int y = 0, x0 = 14;
    CLabel(L"命中名单的程序彻底隐身：不做状态检测、不做光标查询、不显示提示。",
           x0, y, 740, 22);
    y += 22;
    CLabel(L"每行一个进程名，.exe 可写可不写。", x0, y, 740, 22);
    y += 26;

    g_ignoreList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        x0, y, 420, 258, g_parent, NULL, NULL, NULL);
    SendMessageW(g_ignoreList, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    RegMeta(g_ignoreList, -2, 0, NULL, x0, y, 420, 258);
    y += 268;

    g_ignoreEdit = CEdit(x0, y, 300, 24);
    CButton(L"添加", x0 + 310, y, 70, 24, ID_ADD_IGNORE);
    CButton(L"删除选中", x0 + 386, y, 90, 24, ID_DEL_IGNORE);
}

void FillIgnorePanel(void) {
    SendMessageW(g_ignoreList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < s_cfg.ignoreCount; i++)
        SendMessageW(g_ignoreList, LB_ADDSTRING, 0, (LPARAM)s_cfg.ignore[i]);
}

/* ---------- 预览窗口 ---------- */

/* 预览的状态切换：11 个状态都能看，不然预览永远只有当前页第一个状态。
   下拉沿用自绘的 SEL_CLASS，挂在预览条自己身上 —— 几何跟着预览走，缩放时
   不用主窗口再摆一次。 */
void InvalidatePreview(void);

static int PreviewStates(ImeState* out, int cap) {
    int n = 0;
    for (int p = 0; p < 5; p++) {
        for (int k = 0; k < 3; k++) {
            ImeState s = kPages[p].states[k];
            if ((int)s < 0) continue;
            if (n >= cap) return n;
            out[n++] = s;
        }
    }
    return n;
}

ImeState PreviewStateAt(int idx) {
    ImeState list[IMEST_COUNT];
    int n = PreviewStates(list, IMEST_COUNT);
    return (idx >= 0 && idx < n) ? list[idx] : IMEST_US;
}

static int PreviewIndexOf(ImeState s) {
    ImeState list[IMEST_COUNT];
    int n = PreviewStates(list, IMEST_COUNT);
    for (int i = 0; i < n; i++)
        if (list[i] == s) return i;
    return -1;
}

/* 标签与下拉的几何；画标签、摆控件共用一份，避免两边错位 */
static void PreviewSwitchRects(int cw, int ch, RECT* sw, RECT* lbl) {
    int w = DpiScale(PREVIEW_SW_W), h = DpiScale(PREVIEW_SW_H);
    int x = cw - DpiScale(PREVIEW_SW_MARGIN) - w;
    int y = (ch - h) / 2;
    sw->left = x;
    sw->top = y;
    sw->right = x + w;
    sw->bottom = y + h;
    lbl->right = x - DpiScale(8);
    lbl->left = lbl->right - DpiScale(PREVIEW_LBL_W);
    lbl->top = y;
    lbl->bottom = y + h;
}

void SyncPreviewSwitch(void) {
    if (!g_swState) return;
    int idx = PreviewIndexOf((ImeState)g_selState);
    if (idx >= 0) SendMessageW(g_swState, CB_SETCURSEL, (WPARAM)idx, 0);
}

/* 滚轮翻状态：-1 上一个，+1 下一个。到头就停住，不绕回 —— 不然一直滚
   下去会不知道自己在列表的哪个位置。 */
void StepPreviewState(int dir) {
    ImeState list[IMEST_COUNT];
    int n = PreviewStates(list, IMEST_COUNT);
    if (n <= 0) return;
    int idx = PreviewIndexOf((ImeState)g_selState);
    if (idx < 0) idx = 0;
    int next = idx + dir;
    if (next < 0) next = 0;
    if (next >= n) next = n - 1;
    if (next == idx) return;
    g_selState = (int)list[next];
    SyncPreviewSwitch();
    InvalidatePreview();
}

static void BuildPreviewSwitch(HWND parent) {
    g_swState = CreateWindowExW(0, SEL_CLASS, NULL,
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                0, 0, 10, 10, parent,
                                (HMENU)(INT_PTR)ID_PREVIEW_STATE, NULL, NULL);
    if (!g_swState) return;
    SelData* d = (SelData*)calloc(1, sizeof(SelData));
    if (d) {
        d->sel = 0;
        d->hot = -1;
        SetWindowLongPtrW(g_swState, GWLP_USERDATA, (LONG_PTR)d);
    }
    SendMessageW(g_swState, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
    /* 项的顺序必须与 PreviewStates() 一致（都按状态枚举顺序） */
    for (int p = 0; p < 5; p++) {
        for (int k = 0; k < 3; k++) {
            ImeState s = kPages[p].states[k];
            if ((int)s < 0) continue;
            WCHAR t[SEL_MAX_TEXT];
            _snwprintf_s(t, SEL_MAX_TEXT, _TRUNCATE, L"%s · %s",
                         kPages[p].name, StateLabel(s));
            SendMessageW(g_swState, CB_ADDSTRING, 0, (LPARAM)t);
        }
    }
    SyncPreviewSwitch();
}

/* 预览条的整块内容。单独拆出来是为了能画进内存位图（双缓冲）：
   底色、示例文字、光标竖线、圆点分四次往同一块 DC 写，直接画到窗口上时
   拖动滑块（每次滚动都要重画整个预览条）会看到中间那几帧。 */
static void PaintPreview(HDC dc, const RECT* prc) {
    RECT rc = *prc;

    HBRUSH bg = CreateSolidBrush(RGB(0xFA, 0xFA, 0xFA));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    /* 右侧状态切换的几何：画标签和避让文字都用这一份 */
    RECT sw, lbl;
    PreviewSwitchRects(rc.right, rc.bottom, &sw, &lbl);

    int baseY = rc.bottom / 2 + DpiScale(2);
    HFONT old = (HFONT)SelectObject(dc, BaseFont());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x33, 0x33, 0x33));
    const WCHAR* sample = L"输入法状态";
    TextOutW(dc, DpiScale(16), baseY - DpiScale(18), sample, 5);
    SIZE ts;
    GetTextExtentPoint32W(dc, sample, 5, &ts);

    int caretX = DpiScale(16) + ts.cx;
    RECT caret = { caretX, baseY - DpiScale(18), caretX + DpiScale(2), baseY };
    HBRUSH cb = CreateSolidBrush(RGB(0x22, 0x22, 0x22));
    FillRect(dc, &caret, cb);
    DeleteObject(cb);
    SelectObject(dc, old);

    StateStyle st;
    CfgStateStyle(&s_cfg, (ImeState)g_selState, &st);
    if (st.enabled && st.shape != SHAPE_NONE) {
        int sz = DpiScale(st.size);
        if (sz < 4) sz = 4;
        if (sz > DpiScale(96)) sz = DpiScale(96);
        int x = caretX + DpiScale(st.offsetX);
        int y = baseY + DpiScale(st.offsetY);

        int shape = st.shape;
        ImgView iv = { 0, 0, NULL };
        int haveImg = 0;
        if (shape == SHAPE_IMAGE) {
            if (ImgGet(st.image, sz, &iv, NULL, 0)) haveImg = 1;
            else shape = SHAPE_CIRCLE;   /* 图片缺失/解码失败：回退圆点 */
        }
        int dw = haveImg ? iv.w : sz;
        int dh = haveImg ? iv.h : sz;

        /* 走和悬浮窗**同一份**光栅化再 AlphaBlend 贴上去：
           用 GDI 的 Ellipse/Polygon 是无抗锯齿的，预览就会比实际毛糙。 */
        BITMAPINFO bmi;
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = dw;
        bmi.bmiHeader.biHeight = -dh;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = NULL;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = mem ? CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
        if (mem && bmp && bits) {
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            if (haveImg) ImgBlit((BYTE*)bits, &iv, st.alpha);
            else ShapeRasterize((BYTE*)bits, sz, st.rgb, st.alpha, shape);
            BLENDFUNCTION bf;
            bf.BlendOp = AC_SRC_OVER;
            bf.BlendFlags = 0;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(dc, x, y, dw, dh, mem, 0, 0, dw, dh, bf);
            SelectObject(mem, oldBmp);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
    } else {
        /* 不显示的状态也要能看出来：否则会以为预览坏了 */
        RECT hr = { DpiScale(16), baseY + DpiScale(5), lbl.left - DpiScale(12), rc.bottom };
        if (hr.right > hr.left) {
            HFONT oldf = (HFONT)SelectObject(dc, BaseFont());
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0x99, 0x99, 0x99));
            DrawTextW(dc, L"该状态不显示提示", -1, &hr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, oldf);
        }
    }

    /* 右侧切换的说明文字 */
    {
        HFONT oldf = (HFONT)SelectObject(dc, BaseFont());
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x66, 0x66, 0x66));
        DrawTextW(dc, L"预览状态", -1, &lbl,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldf);
    }
}

LRESULT CALLBACK PreviewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        BuildPreviewSwitch(h);
        return 0;
    case WM_SIZE: {
        RECT pr;
        GetClientRect(h, &pr);
        RECT sw, lbl;
        PreviewSwitchRects(pr.right, pr.bottom, &sw, &lbl);
        if (g_swState)
            SetWindowPos(g_swState, HWND_TOP, sw.left, sw.top,
                         sw.right - sw.left, sw.bottom - sw.top, SWP_NOACTIVATE);
        return 0;
    }
    case WM_COMMAND:            /* 下拉选中：转给主窗口 @see WM_COMMAND */
        return SendMessageW(g_hwnd, m, w, l);
    case WM_ERASEBKGND:
        return 1;               /* 底色算在 PaintPreview 里，别擦第二遍 */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        BufDC buf;
        if (BufBegin(&buf, dc, rc.right, rc.bottom)) {
            PaintPreview(buf.mem, &rc);
            BufEnd(&buf, dc);
        } else {
            PaintPreview(dc, &rc);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

void InvalidatePreview(void) {
    if (g_preview) InvalidateRect(g_preview, NULL, FALSE);
}
