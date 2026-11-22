#include "settings_internal.h"

/* ================= IMEIndicator 设置程序 =================
   独立 exe，纯 Win32。分页表单（原型变体 C）：自定义页签 + 预览条 + 表单。

   为什么是独立 exe：
   - 主程序常以管理员运行，设置程序需要能以普通权限启动并写配置；
   - 设置 UI 崩了不该带崩指示器；不跑设置时也不占主程序体积。

   保存流程：写 IMEIndicator.json -> 调 IMEIndicator.exe --restart。
   重启失败（典型：设置程序未提权、主程序是管理员）时提示手动重启 ——
   这是有意接受的降级，不做提权。

   ★ 控件**只创建一次**，切页只改内容与显隐。早先是切页时 Destroy 再重建，
     控件逐个消失/出现，看起来就是自上而下的扫描。现在每个页面各有一个面板
     子窗口，切页时整块面板一次 ShowWindow，视觉上是原子的。

   ★ 布局全部按 96 DPI 的逻辑坐标写死，建完后由 ScaleControls() 统一乘 DPI。
     窗口尺寸用 AdjustWindowRectExForDpi 按**客户区**反推 —— 把逻辑尺寸当整窗
     尺寸用，最底下的状态块会被底部按钮压住。 */

ImeCfg  s_cfg;
HWND    g_hwnd = NULL;
HWND    g_preview = NULL;
HWND    g_swState = NULL;   /* 预览的状态切换下拉（预览条的子窗口） */
int     g_page = 0;
int     g_selState = 0;
HFONT   g_font = NULL;
HBRUSH  g_whiteBrush = NULL;
static int     g_dirty = 0;
int     g_dpi = 96;
static UINT    g_startupDpi = 96;
static int     g_loadedWidth = 0;   /* 启动时从配置里恢复的窗口宽度（逻辑像素） */
static int     g_widthDirty = 0;    /* 用户改过窗口宽度：关闭时补写进配置 */
static int     g_sizeInit = 0;      /* 首次 WM_SIZE 只记录宽度，不算用户拖动 */

static HWND  g_layoutPanel = NULL;

static HWND SliderFor(int block, int field);
static void ApplySliderValue(int block, int field, int v);
static int  DefaultFor(int block, int field);
static void SetSliderValue(int block, int field, int v);

static void ShowPage(int page);

/* ---------- 提交 ---------- */
static int Checked(HWND c) { return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED; }

/* 把当前页的控件值写回 s_cfg（用 g_blkState 记住每块当前对应哪个状态） */
static void CommitForm(void) {
    if (g_page < 5) {
        for (int i = 0; i < 3; i++) {
            ImeState s = g_blkState[i];
            if ((int)s < 0) continue;
            Block* b = &g_blk[i];
            StateStyle* raw = &s_cfg.st[s];
            raw->enabled = Checked(b->enable);
            raw->have |= SF_ENABLED;
            /* 跟随中：除「启用 / 跟随」外的控件显示的是基础状态的值，不能写回
               —— 写回去会把这几个字段变成显式值，之后再改基础状态就不跟了。 */
            int inherited = 0;
            if (IsCapsState(s)) {
                inherited = Checked(b->inherit);
                raw->inherit = inherited ? CapsBase(s) : -1;
            }
            if (inherited) continue;
            int sel = (int)SendMessageW(b->shape, CB_GETCURSEL, 0, 0);
            int shape = (sel >= 0 && sel < SHAPE_ORDER_COUNT)
                            ? kShapeOrder[sel] : SHAPE_CIRCLE;
            raw->shape = shape;
            raw->have |= SF_SHAPE;
            if (shape != SHAPE_IMAGE) g_lastShape[s] = shape;
            /* 跟随 cn_cn 的项不能写回：写回去就变成显式值，之后改 cn_cn 不跟了 */
            if (!CfgFieldFollowsCn(&s_cfg, s, SF_SIZE)) {
                raw->size = SliderPos(b->slSize);       raw->have |= SF_SIZE;
            }
            if (!CfgFieldFollowsCn(&s_cfg, s, SF_OFFSETX)) {
                raw->offsetX = SliderPos(b->slOffX);    raw->have |= SF_OFFSETX;
            }
            if (!CfgFieldFollowsCn(&s_cfg, s, SF_OFFSETY)) {
                raw->offsetY = SliderPos(b->slOffY);    raw->have |= SF_OFFSETY;
            }
            if (!CfgFieldFollowsCn(&s_cfg, s, SF_ALPHA)) {
                raw->alpha = SliderPos(b->slAlpha);     raw->have |= SF_ALPHA;
            }
            raw->transient = Checked(b->transient); raw->have |= SF_TRANSIENT;
        }
    } else if (g_page == PAGE_GLOBAL) {
        s_cfg.pollMs = SliderPos(g_slPoll);
        s_cfg.trackMs = SliderPos(g_slTrack);
        s_cfg.caretTimeoutMs = SliderPos(g_slTimeout);
        s_cfg.autoHideMs = SliderPos(g_slAutoHide);
        s_cfg.imeStrategy = (int)SendMessageW(g_cbStrategy, CB_GETCURSEL, 0, 0);
        s_cfg.hideFullscreen = Checked(g_ckFull);
        s_cfg.hideComposition = Checked(g_ckComp);
        s_cfg.wpsCom = Checked(g_ckWps);
    }
    /* 黑名单的增删是即时写进 s_cfg 的，不用提交 */
}

/* ---------- 脏标记 ----------
   只比"值"，不比 have 位：勾选再取消、把数字改回原值都不算修改。
   保存按钮与标题星号都跟着这个判断走。 */
static ImeCfg g_base;

static int StyleSame(const StateStyle* a, const StateStyle* b) {
    return a->enabled == b->enabled && a->rgb == b->rgb && a->shape == b->shape &&
           wcscmp(a->image, b->image) == 0 && a->size == b->size &&
           a->offsetX == b->offsetX && a->offsetY == b->offsetY &&
           a->alpha == b->alpha && a->transient == b->transient &&
           a->inherit == b->inherit;
}

static int CfgSameValues(const ImeCfg* a, const ImeCfg* b) {
    if (a->pollMs != b->pollMs || a->trackMs != b->trackMs ||
        a->imeStrategy != b->imeStrategy ||
        a->caretTimeoutMs != b->caretTimeoutMs ||
        a->hideFullscreen != b->hideFullscreen ||
        a->hideComposition != b->hideComposition ||
        a->wpsCom != b->wpsCom || a->autoHideMs != b->autoHideMs)
        return 0;
    if (!StyleSame(&a->def, &b->def)) return 0;
    for (int i = 0; i < IMEST_COUNT; i++)
        if (!StyleSame(&a->st[i], &b->st[i])) return 0;
    if (a->ignoreCount != b->ignoreCount) return 0;
    for (int i = 0; i < a->ignoreCount; i++)
        if (_wcsicmp(a->ignore[i], b->ignore[i]) != 0) return 0;
    return 1;
}

static void RefreshDirty(void) {
    if (!g_hwnd) return;
    CommitForm();
    g_dirty = !CfgSameValues(&s_cfg, &g_base);

    /* 只在真的变了才动标题与按钮：每次滚动都 SetWindowText/EnableWindow 一遍，
       即便值没变也会让非客户区与按钮重画，拖动滑块时看起来就是一直在闪。 */
    WCHAR t[96];
    _snwprintf_s(t, 96, _TRUNCATE, L"IMEIndicator 设置%s", g_dirty ? L" *" : L"");
    WCHAR cur[96];
    if (GetWindowTextW(g_hwnd, cur, 96) <= 0 || wcscmp(cur, t) != 0)
        SetWindowTextW(g_hwnd, t);

    static int lastDirty = -1;
    if (lastDirty != g_dirty) {
        lastDirty = g_dirty;
        HWND b1 = GetDlgItem(g_hwnd, ID_SAVE);
        HWND b2 = GetDlgItem(g_hwnd, ID_SAVE_RESTART);
        if (b1) EnableWindow(b1, g_dirty);
        if (b2) EnableWindow(b2, g_dirty);
    }
}

/* 窗口宽度属于界面状态，不是配置项：单独补写一次，不把界面上没保存的改动
   带出去 —— 重新读盘、只改这一个字段、写回。 */
static void SaveSettingsWidth(int w) {
    ImeCfg tmp;
    CfgLoad(&tmp);
    if (tmp.settingsWidth == w) return;
    tmp.settingsWidth = w;
    CfgSave(&tmp);
}

/* ---------- 保存 / 重启 ---------- */
static void DoSave(int restart) {
    CommitForm();
    /* 形状=图片但没选到可用图片：回退到上一次选择的形状再存 */
    if (FallbackBadImages()) {
        if (g_page < 5) ShowPage(g_page);
        InvalidatePreview();
    }
    if (!CfgSave(&s_cfg)) {
        MessageBoxW(g_hwnd, L"写入 IMEIndicator.json 失败。\n\n"
                            L"请确认程序所在目录可写（放在 Program Files 下时需要管理员权限）。",
                    L"保存失败", MB_ICONERROR | MB_OK);
        return;
    }
    g_base = s_cfg;
    RefreshDirty();

    if (!restart) {
        MessageBoxW(g_hwnd, L"已保存。\n\n重启 IMEIndicator 后生效。",
                    L"IMEIndicator 设置", MB_ICONINFORMATION | MB_OK);
        return;
    }

    /* 调主程序 --restart。退出码 2 = 有实例在跑但通知失败（典型：权限不一致）。 */
    WCHAR dir[MAX_PATH], exe[MAX_PATH];
    CfgDir(dir, MAX_PATH);
    _snwprintf_s(exe, MAX_PATH, _TRUNCATE, L"%s\\IMEIndicator.exe", dir);

    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwnd, L"已保存，但找不到 IMEIndicator.exe。\n\n请手动启动主程序。",
                    L"IMEIndicator 设置", MB_ICONWARNING | MB_OK);
        return;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    WCHAR cmd[MAX_PATH + 32];
    _snwprintf_s(cmd, MAX_PATH + 32, _TRUNCATE, L"\"%s\" --restart", exe);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, dir, &si, &pi)) {
        MessageBoxW(g_hwnd, L"已保存，但无法启动 IMEIndicator.exe 来重启。\n\n"
                            L"请从托盘菜单点「重启程序」。",
                    L"IMEIndicator 设置", MB_ICONWARNING | MB_OK);
        return;
    }
    WaitForSingleObject(pi.hProcess, 3000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code == 2) {
        MessageBoxW(g_hwnd, L"已保存，但无法通知正在运行的 IMEIndicator 重启。\n\n"
                            L"这通常是因为主程序以管理员身份运行、而设置程序没有。\n"
                            L"请从托盘菜单点「重启程序」。",
                    L"IMEIndicator 设置", MB_ICONWARNING | MB_OK);
    }
}

/* ---------- 页切换 ---------- */
static void ShowPage(int page) {
    ShowWindow(g_layoutPanel, page < 5 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_globalPanel, page == PAGE_GLOBAL ? SW_SHOW : SW_HIDE);
    ShowWindow(g_ignorePanel, page == PAGE_IGNORE ? SW_SHOW : SW_HIDE);
    if (page < 5) {
        for (int i = 0; i < 3; i++)
            FillBlock(i, kPages[page].states[i]);
        /* 最后一块的分隔线会变成页面底部的悬空线，藏掉 */
        int last = -1;
        for (int i = 0; i < 3; i++)
            if ((int)kPages[page].states[i] >= 0) last = i;
        for (int i = 0; i < 3; i++)
            if (g_blk[i].sep)
                ShowWindow(g_blk[i].sep, i < last ? SW_SHOW : SW_HIDE);
    } else if (page == PAGE_GLOBAL) {
        FillGlobalPanel();
    } else {
        FillIgnorePanel();
    }
    InvalidatePreview();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void SwitchPage(int page) {
    if (page < 0 || page >= PAGE_COUNT || page == g_page) return;
    CommitForm();                     /* 先把当前页的值收回去 */
    g_page = page;
    if (page < 5) {
        /* 切页时预览跟着落到本页第一个状态；之后可以再用下拉挑别的 */
        g_selState = (int)kPages[page].states[0];
        SyncPreviewSwitch();
    }
    ShowPage(page);
}

/* ---------- 页签绘制 ---------- */
static int TabWidth(HDC dc, int i) {
    HFONT old = (HFONT)SelectObject(dc, BaseFont());
    SIZE sz;
    GetTextExtentPoint32W(dc, kPages[i].name, (int)wcslen(kPages[i].name), &sz);
    SelectObject(dc, old);
    return sz.cx + DpiScale(26);
}

static int g_hotTab = -1;   /* 鼠标悬停的页签；-1 = 没有 */

static void PaintTabs(HDC dc) {
    RECT crc;
    GetClientRect(g_hwnd, &crc);
    int x = DpiScale(TAB_X);
    int top = DpiScale(TAB_Y), bot = DpiScale(TAB_Y + TAB_H);
    HFONT old = (HFONT)SelectObject(dc, BaseFont());
    SetBkMode(dc, TRANSPARENT);

    /* 分隔线：所有页签都"站"在它上面（现代设置页的下划线页签） */
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xD6, 0xD6, 0xD6));
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, 0, bot - 1, NULL);
    LineTo(dc, crc.right, bot - 1);
    SelectObject(dc, op);
    DeleteObject(pen);

    for (int i = 0; i < PAGE_COUNT; i++) {
        int w = TabWidth(dc, i);
        RECT r = { x, top, x + w, bot };
        if (i == g_page) {
            /* 选中页：强调条从页签脚下长出来，贴住分隔线 */
            RECT ul = { x + DpiScale(8), bot - DpiScale(3), x + w - DpiScale(8), bot - 1 };
            HBRUSH b = CreateSolidBrush(ACCENT_RGB);
            FillRect(dc, &ul, b);
            DeleteObject(b);
        } else if (i == g_hotTab) {
            RECT hr = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
            COLORREF hc = RGB(0xE5, 0xF1, 0xFB);
            PaintRoundRectAA(dc, &hr, DpiScale(4), hc, hc);
        }
        /* 选中页不加粗：粗体只留给段头，切换时文字宽度不跳 */
        SetTextColor(dc, i == g_page ? RGB(0x00, 0x5A, 0x9E)
                        : i == g_hotTab ? RGB(0x00, 0x00, 0x00)
                                        : RGB(0x54, 0x54, 0x54));
        DrawTextW(dc, kPages[i].name, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += w;
    }
    SelectObject(dc, old);
}

static int HitTab(int mx, int my) {
    if (my < DpiScale(TAB_Y) || my > DpiScale(TAB_Y + TAB_H)) return -1;
    HDC dc = GetDC(g_hwnd);
    int x = DpiScale(TAB_X);
    int hit = -1;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int w = TabWidth(dc, i);
        if (mx >= x && mx < x + w) { hit = i; break; }
        x += w;
    }
    ReleaseDC(g_hwnd, dc);
    return hit;
}

/* ---------- 选图片 ---------- */
/* 选一个文件 -> 复制进 shapes\（重名自动加序号）-> 该状态切到图片形状。
   复制而不是引用原路径：配置里只存文件名，换机器/挪目录都不会断。 */
static void PickImage(int block) {
    ImeState s = g_blkState[block];
    if ((int)s < 0) return;

    WCHAR file[MAX_PATH];
    file[0] = 0;
    WCHAR title[128];
    _snwprintf_s(title, 128, _TRUNCATE,
                 L"选择图片（文件 ≤ %u MB，长边 ≤ %u 像素）",
                 IMG_MAX_FILE_BYTES / (1024u * 1024u), IMG_MAX_EDGE);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"图片 (*.png;*.ico;*.bmp;*.jpg)\0*.png;*.ico;*.bmp;*.jpg\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    WCHAR name[CFG_NAME_MAX];
    WCHAR err[256];
    if (!ImgImport(file, name, CFG_NAME_MAX, err, 256)) {
        MessageBoxW(g_hwnd, err[0] ? err : L"导入图片失败。",
                    L"导入图片失败", MB_ICONERROR | MB_OK);
        return;
    }

    StateStyle* raw = &s_cfg.st[s];
    lstrcpynW(raw->image, name, CFG_NAME_MAX);
    raw->shape = SHAPE_IMAGE;
    raw->have |= SF_IMAGE | SF_SHAPE;
    g_dirty = 1;
    FillBlock(block, s);
    InvalidatePreview();
}

/* 恢复出厂默认：只改内存里的配置，界面立刻反映；点「保存」才写盘，
   直接关窗口等于没重置。窗口宽度是界面状态，不参与重置。 */
static void ResetAll(void) {
    if (MessageBoxW(g_hwnd,
                    L"把所有设置恢复为默认值？\n\n"
                    L"恢复后要点「保存」才会写入配置文件；直接关闭窗口不改动。",
                    L"恢复默认值",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return;
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    int keepW = MulDiv(cr.right, 96, g_dpi > 0 ? g_dpi : 96);
    CfgDefaults(&s_cfg);
    s_cfg.settingsWidth = keepW;
    SyncLastShapes();
    ShowPage(g_page);        /* 重填当前页（含预览） */
    RefreshDirty();
}

/* ---------- 命令处理 ---------- */
static void OnCommand(int id, HWND ctl) {
    if (id == ID_RESET_ALL) { ResetAll(); return; }
    if (id >= ID_RESET && id < ID_RESET + 16) {
        int k = id - ID_RESET;
        int block = (k < 12) ? k / 4 : -1;
        int field = (k < 12) ? kStateFields[k % 4] : kGlobalFields[k - 12];
        SetSliderValue(block, field, DefaultFor(block, field));
        return;
    }
    if (id >= ID_PICK_IMAGE && id < ID_PICK_IMAGE + 3) {
        PickImage(id - ID_PICK_IMAGE);
        return;
    }
    if (id == ID_SAVE)         { DoSave(0); return; }
    if (id == ID_SAVE_RESTART) { DoSave(1); return; }
    if (id == ID_CLOSE)        { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }

    if (id == ID_ADD_IGNORE && g_ignoreEdit) {
        WCHAR buf[CFG_NAME_MAX];
        GetWindowTextW(g_ignoreEdit, buf, CFG_NAME_MAX);
        WCHAR* q = buf;
        while (*q == L' ') q++;
        size_t len = wcslen(q);
        while (len > 0 && q[len-1] == L' ') q[--len] = 0;
        if (!q[0] || s_cfg.ignoreCount >= CFG_MAX_IGNORE) return;
        lstrcpynW(s_cfg.ignore[s_cfg.ignoreCount], q, CFG_NAME_MAX);
        s_cfg.ignoreCount++;
        SendMessageW(g_ignoreList, LB_ADDSTRING, 0, (LPARAM)q);
        SetWindowTextW(g_ignoreEdit, L"");
        g_dirty = 1;
        return;
    }
    if (id == ID_DEL_IGNORE && g_ignoreList) {
        int sel = (int)SendMessageW(g_ignoreList, LB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= s_cfg.ignoreCount) return;
        for (int i = sel; i + 1 < s_cfg.ignoreCount; i++)
            lstrcpynW(s_cfg.ignore[i], s_cfg.ignore[i+1], CFG_NAME_MAX);
        s_cfg.ignoreCount--;
        SendMessageW(g_ignoreList, LB_DELETESTRING, (WPARAM)sel, 0);
        g_dirty = 1;
        return;
    }

    Meta* m = FindMeta(ctl);
    if (!m || m->block < 0) return;

    /* 尺寸/偏移/透明度 的「自定义」：不勾 = 跟随中文输入法(cn_cn)。
       勾上时把当前显示的值（cn_cn 的）落成本状态自己的显式值，观感不变；
       取消勾选则清掉显式位，求值时自然回落到 cn_cn。 */
    unsigned cbit = CustomBit(m->field);
    if (cbit) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        StateStyle* raw = &s_cfg.st[s];
        if (Checked(ctl)) {
            StateStyle eff;
            CfgStateStyle(&s_cfg, s, &eff);
            switch (cbit) {
            case SF_SIZE:    raw->size = eff.size;       break;
            case SF_OFFSETX: raw->offsetX = eff.offsetX; break;
            case SF_OFFSETY: raw->offsetY = eff.offsetY; break;
            case SF_ALPHA:   raw->alpha = eff.alpha;     break;
            }
            raw->have |= cbit;
        } else {
            raw->have &= ~cbit;
        }
        g_dirty = 1;
        FillBlock(m->block, s);
        InvalidatePreview();
        return;
    }

    /* 颜色按钮：弹系统取色器 */
    if (m->field == F_COLOR) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        StateStyle* raw = &s_cfg.st[s];
        CHOOSECOLORW cc;
        ZeroMemory(&cc, sizeof(cc));
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = g_hwnd;
        cc.rgbResult = RGB((raw->rgb >> 16) & 0xFF, (raw->rgb >> 8) & 0xFF, raw->rgb & 0xFF);
        static COLORREF custom[16];
        cc.lpCustColors = custom;
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            raw->rgb = ((DWORD)GetRValue(cc.rgbResult) << 16) |
                       ((DWORD)GetGValue(cc.rgbResult) << 8) |
                       (DWORD)GetBValue(cc.rgbResult);
            raw->have |= SF_COLOR;
            g_dirty = 1;
            InvalidateRect(ctl, NULL, TRUE);
            InvalidatePreview();
        }
        return;
    }

    /* 跟随开关：立刻把整块置灰/解灰；取消跟随时把基础状态的值拷进来 */
    if (m->field == F_INHERIT) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        StateStyle* raw = &s_cfg.st[s];
        int on = Checked(ctl);
        if (on) {
            raw->inherit = CapsBase(s);
        } else {
            StateStyle eff;
            CfgStateStyle(&s_cfg, s, &eff);
            raw->inherit = -1;
            raw->enabled = eff.enabled;     raw->have |= SF_ENABLED;
            raw->rgb = eff.rgb;             raw->have |= SF_COLOR;
            raw->shape = eff.shape;         raw->have |= SF_SHAPE;
            raw->size = eff.size;           raw->have |= SF_SIZE;
            raw->offsetX = eff.offsetX;     raw->have |= SF_OFFSETX;
            raw->offsetY = eff.offsetY;     raw->have |= SF_OFFSETY;
            raw->alpha = eff.alpha;         raw->have |= SF_ALPHA;
            raw->transient = eff.transient; raw->have |= SF_TRANSIENT;
            /* 形状=图片时文件名也得一起拷：只拷 shape 会变成「图片 + 空文件名」，
               预览/主程序找不到图就回退成圆点，等于把图片丢了 */
            lstrcpynW(raw->image, eff.image, CFG_NAME_MAX);
            raw->have |= SF_IMAGE;
        }
        g_dirty = 1;
        FillBlock(m->block, s);
        InvalidatePreview();
        return;
    }

    if (m->field == F_SHAPE) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        int shape = (sel >= 0 && sel < SHAPE_ORDER_COUNT)
                        ? kShapeOrder[sel] : SHAPE_CIRCLE;
        s_cfg.st[s].shape = shape;
        s_cfg.st[s].have |= SF_SHAPE;
        if (shape != SHAPE_IMAGE) g_lastShape[s] = shape;
        g_dirty = 1;
        /* 切到/切离「图片」要同步文件名标签的显隐与标红 */
        FillBlock(m->block, s);
        InvalidatePreview();
        return;
    }
    if (m->field == F_ENABLED) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        s_cfg.st[s].enabled = Checked(ctl);
        s_cfg.st[s].have |= SF_ENABLED;
        g_dirty = 1;
        InvalidatePreview();
        return;
    }
    if (m->field == F_TRANSIENT) {
        ImeState s = g_blkState[m->block];
        if ((int)s < 0) return;
        s_cfg.st[s].transient = Checked(ctl);
        s_cfg.st[s].have |= SF_TRANSIENT;
        g_dirty = 1;
        return;
    }

    /* 全局页的复选/下拉 */
    if (m->block == -1) {
        if (m->field == F_FULLSCREEN)  s_cfg.hideFullscreen = Checked(ctl);
        if (m->field == F_COMPOSITION) s_cfg.hideComposition = Checked(ctl);
        if (m->field == F_WPSCOM)      s_cfg.wpsCom = Checked(ctl);
        if (m->field == F_STRATEGY)    s_cfg.imeStrategy = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        g_dirty = 1;
    }
}

/* 滑块与数值框共用的"落到配置"逻辑 */
static HWND SliderFor(int block, int field) {
    if (block >= 0 && block < 3) {
        Block* b = &g_blk[block];
        switch (field) {
        case F_SIZE:  return b->slSize;
        case F_OFFX:  return b->slOffX;
        case F_OFFY:  return b->slOffY;
        case F_ALPHA: return b->slAlpha;
        }
    } else if (block == -1) {
        switch (field) {
        case F_POLL:     return g_slPoll;
        case F_TRACK:    return g_slTrack;
        case F_TIMEOUT:  return g_slTimeout;
        case F_AUTOHIDE: return g_slAutoHide;
        }
    }
    return NULL;
}

static void ApplySliderValue(int block, int field, int v) {
    if (block >= 0) {
        ImeState s = g_blkState[block];
        if ((int)s < 0) return;
        StateStyle* raw = &s_cfg.st[s];
        switch (field) {
        case F_SIZE:  raw->size = v;    raw->have |= SF_SIZE;    break;
        case F_OFFX:  raw->offsetX = v; raw->have |= SF_OFFSETX; break;
        case F_OFFY:  raw->offsetY = v; raw->have |= SF_OFFSETY; break;
        case F_ALPHA: raw->alpha = v;   raw->have |= SF_ALPHA;   break;
        default: return;
        }
        InvalidatePreview();
    } else if (block == -1) {
        switch (field) {
        case F_POLL:     s_cfg.pollMs = v; break;
        case F_TRACK:    s_cfg.trackMs = v; break;
        case F_TIMEOUT:  s_cfg.caretTimeoutMs = v; break;
        case F_AUTOHIDE:
            s_cfg.autoHideMs = v;
            if (g_page < 5) { for (int i = 0; i < 3; i++) FillBlock(i, g_blkState[i]); }
            break;
        default: return;
        }
    }
    g_dirty = 1;

    /* 拖动时整条重画：只重画滑块矩形的话，旧滑块位置会留下残影 */
    HWND sl = SliderFor(block, field);
    if (sl) InvalidateRect(sl, NULL, FALSE);
}

/* 重置到默认值：状态字段用 defaults 段，全局字段用出厂值 */
static int DefaultFor(int block, int field) {
    if (block >= 0) {
        switch (field) {
        case F_SIZE:  return s_cfg.def.size;
        case F_OFFX:  return s_cfg.def.offsetX;
        case F_OFFY:  return s_cfg.def.offsetY;
        case F_ALPHA: return s_cfg.def.alpha;
        }
    } else {
        switch (field) {
        case F_POLL:     return 100;
        case F_TRACK:    return 15;
        case F_TIMEOUT:  return 150;
        case F_AUTOHIDE: return 1000;
        }
    }
    return 0;
}

static void SetSliderValue(int block, int field, int v) {
    HWND sl = SliderFor(block, field);
    if (!sl) return;
    SetSliderPos(sl, v);
    Meta* m = FindMeta(sl);
    if (m && m->numEdit) SetNumText(m->numEdit, v);
    ApplySliderValue(block, field, v);
}

static void OnHScroll(HWND slider) {
    Meta* m = FindMeta(slider);
    if (!m) return;
    int v = SliderPos(slider);
    SetNumText(m->numEdit, v);
    ApplySliderValue(m->block, m->field, v);
}

/* 数值框输入：夹到滑块值域后同步回去 */
static void OnNumEditChanged(HWND e) {
    if (g_syncEdit) return;
    Meta* m = FindMeta(e);
    if (!m) return;
    HWND sl = SliderFor(m->block, m->field);
    if (!sl) return;
    WCHAR buf[32];
    GetWindowTextW(e, buf, 32);
    if (!buf[0]) return;                  /* 清空/输入中：先不动 */
    int v = _wtoi(buf);
    int lo = (int)SendMessageW(sl, TBM_GETRANGEMIN, 0, 0);
    int hi = (int)SendMessageW(sl, TBM_GETRANGEMAX, 0, 0);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    SetSliderPos(sl, v);
    ApplySliderValue(m->block, m->field, v);
}

/* 失焦：用滑块当前值把文本纠正回来（越界/空值不留在框里） */
static void OnNumEditDone(HWND e) {
    Meta* m = FindMeta(e);
    if (!m) return;
    HWND sl = SliderFor(m->block, m->field);
    if (!sl) return;
    SetNumText(e, (int)SendMessageW(sl, TBM_GETPOS, 0, 0));
}

/* ---------- 窗口过程 ---------- */
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        g_dpi = (int)g_startupDpi;
        if (g_dpi <= 0) g_dpi = 96;
        g_whiteBrush = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));

        /* 预览：WS_CLIPCHILDREN 是必须的 —— 状态切换下拉是它的子窗口，
           铺底色时不能盖掉 */
        g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"IMEIndicatorPreview", NULL,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, 100,
            h, NULL, NULL, NULL);

        /* 底部按钮（常驻，不随页销毁） */
        {
            g_parent = h;
            HWND b;
            b = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_SAVE, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            MakePushButton(b);
            b = CreateWindowExW(0, L"BUTTON", L"保存并重启", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_SAVE_RESTART, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            MakePushButton(b);
            b = CreateWindowExW(0, L"BUTTON", L"退出", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_CLOSE, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            MakePushButton(b);
            /* 恢复默认值：靠左放，跟右下角的提交类按钮分开，免得误点 */
            b = CreateWindowExW(0, L"BUTTON", L"恢复默认值", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)ID_RESET_ALL, NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)BaseFont(), TRUE);
            MakePushButton(b);
        }

        /* 三个页面板：全部建好，切页只改显隐 */
        g_layoutPanel = MakePanel();
        g_parent = g_layoutPanel;
        for (int i = 0; i < 3; i++) BuildStateBlock(i, i * 126 + 4);

        g_globalPanel = MakePanel();
        BuildGlobalPanel();

        g_ignorePanel = MakePanel();
        BuildIgnorePanel();

        ScaleControls();
        g_page = 0;
        g_selState = (int)kPages[0].states[0];
        SyncPreviewSwitch();
        ShowPage(0);
        RefreshDirty();          /* 初始无改动：保存按钮置灰，标题不带星号 */
        return 0;

    case WM_SIZE: {
        RECT rc;
        GetClientRect(h, &rc);
        MoveWindow(g_preview, DpiScale(PREVIEW_X), DpiScale(PREVIEW_Y),
                   rc.right - DpiScale(PREVIEW_X * 2), DpiScale(PREVIEW_H), TRUE);
        int pw = rc.right - DpiScale(14);   /* 面板从 x=14 铺到客户区右缘，右列才不会被裁 */
        int ph = rc.bottom - DpiScale(CONTENT_Y) - DpiScale(BOTTOM_H) + DpiScale(4);
        MoveWindow(g_layoutPanel, DpiScale(14), DpiScale(CONTENT_Y), pw, ph, TRUE);
        MoveWindow(g_globalPanel, DpiScale(14), DpiScale(CONTENT_Y), pw, ph, TRUE);
        MoveWindow(g_ignorePanel, DpiScale(14), DpiScale(CONTENT_Y), pw, ph, TRUE);

        int bh = DpiScale(28);
        int by = rc.bottom - DpiScale(BOTTOM_H) + (DpiScale(BOTTOM_H) - bh) / 2;
        HWND b0 = GetDlgItem(h, ID_RESET_ALL);
        HWND b1 = GetDlgItem(h, ID_SAVE);
        HWND b2 = GetDlgItem(h, ID_SAVE_RESTART);
        HWND b3 = GetDlgItem(h, ID_CLOSE);
        /* 左侧一个「恢复默认值」，右侧 Submit 那组靠右：保存 / 保存并重启 / 退出 */
        if (b0) SetWindowPos(b0, HWND_TOP, DpiScale(14), by, DpiScale(96), bh, SWP_NOACTIVATE);
        int bx3 = rc.right - DpiScale(14) - DpiScale(84);
        int bx2 = bx3 - DpiScale(8) - DpiScale(116);
        int bx1 = bx2 - DpiScale(8) - DpiScale(84);
        if (b1) SetWindowPos(b1, HWND_TOP, bx1, by, DpiScale(84), bh, SWP_NOACTIVATE);
        if (b2) SetWindowPos(b2, HWND_TOP, bx2, by, DpiScale(116), bh, SWP_NOACTIVATE);
        if (b3) SetWindowPos(b3, HWND_TOP, bx3, by, DpiScale(84), bh, SWP_NOACTIVATE);

        /* 记住窗口宽度（存逻辑像素，换 DPI 显示器也成立）。
           首次 WM_SIZE 只记录不置脏：窗口会被工作区夹窄，此时 lw 与配置里的
           宽度不同并不代表用户拖过，否则「没拖过就没有这个字段」就失效了。 */
        int lw = MulDiv(rc.right, 96, g_dpi > 0 ? g_dpi : 96);
        if (lw <= 0) return 0;   /* 最小化时客户区为 0，别把 0 当成用户拖窄 */
        if (!g_sizeInit) {
            g_sizeInit = 1;
            s_cfg.settingsWidth = lw;
        } else if (lw != s_cfg.settingsWidth) {
            s_cfg.settingsWidth = lw;
            if (lw != g_loadedWidth) g_widthDirty = 1;
        }
        return 0;
    }

    case WM_GETMINMAXINFO: {
        /* 内容按固定逻辑坐标摆，窗口窄于设计宽度时右列会被裁掉 —— 直接不让缩 */
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        RECT rc = { 0, 0, DpiScale(WIN_W), DpiScale(WIN_H) };
        ClientToWindowSize(&rc, (DWORD)GetWindowLongPtrW(h, GWL_STYLE), 0,
                           (UINT)(g_dpi > 0 ? g_dpi : 96));
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }

    case WM_COMMAND: {
        int code = HIWORD(w);
        HWND ctl = (HWND)l;
        /* 预览的状态切换：只看不改配置，不能影响脏标记 */
        if (code == CBN_SELCHANGE && ctl && ctl == g_swState) {
            ImeState s = PreviewStateAt((int)SendMessageW(ctl, CB_GETCURSEL, 0, 0));
            if ((int)s >= 0 && (int)s != g_selState) {
                g_selState = (int)s;
                InvalidatePreview();
            }
            return 0;
        }
        if (code == EN_CHANGE)    { OnNumEditChanged(ctl); RefreshDirty(); return 0; }
        if (code == EN_KILLFOCUS) { OnNumEditDone(ctl); RefreshDirty(); return 0; }
        if (code == BN_CLICKED && GetPropW(ctl, CHK_PROP)) {
            /* 自绘复选框不会自己翻转状态，这里补上 */
            int st = (int)SendMessageW(ctl, BM_GETCHECK, 0, 0);
            SendMessageW(ctl, BM_SETCHECK,
                         st == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            InvalidateRect(ctl, NULL, FALSE);
        }
        OnCommand(LOWORD(w), ctl);
        RefreshDirty();
        return 0;
    }

    case WM_HSCROLL:
        OnHScroll((HWND)l);
        RefreshDirty();
        return 0;

    case WM_MOUSEMOVE: {
        int t = HitTab((short)LOWORD(l), (short)HIWORD(l));
        if (t != g_hotTab) {
            g_hotTab = t;
            InvalidateRect(h, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme;
        ZeroMemory(&tme, sizeof(tme));
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        TrackMouseEvent(&tme);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hotTab >= 0) {
            g_hotTab = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(l);
        int my = (short)HIWORD(l);
        int t = HitTab(mx, my);
        if (t >= 0) SwitchPage(t);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        /* 光标停在预览状态下拉上时，滚轮直接翻状态 —— 不用点开列表就能
           一路滚着比较各状态的样子。滚轮消息发给焦点窗口，所以按屏幕坐标
           反查光标下面是谁。 */
        POINT pt;
        pt.x = (short)LOWORD(l);
        pt.y = (short)HIWORD(l);
        HWND under = WindowFromPoint(pt);
        if (g_swState && under && (under == g_swState || IsChild(g_swState, under))) {
            int delta = GET_WHEEL_DELTA_WPARAM(w);
            if (delta > 0)      StepPreviewState(-1);
            else if (delta < 0) StepPreviewState(1);
            return 0;
        }
        break;                  /* 其它地方照常滚动 */
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)l;
        if (di->CtlType != ODT_BUTTON) return FALSE;

        if (GetPropW(di->hwndItem, CHK_PROP)) {
            PaintCheck(di);
            return TRUE;
        }

        Meta* mm = FindMeta(di->hwndItem);
        if (!mm || mm->field != F_COLOR) {
            PaintPushButton(di);
            return TRUE;
        }

        /* 颜色按钮：圆角色块 + 细边框 */
        DWORD rgb = 0xFF0000;
        if (mm->block >= 0) {
            ImeState s = g_blkState[mm->block];
            if ((int)s >= 0) {
                StateStyle eff;
                CfgStateStyle(&s_cfg, s, &eff);
                rgb = eff.rgb;
            }
        }
        HBRUSH bg = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
        FillRect(di->hDC, &di->rcItem, bg);
        DeleteObject(bg);

        COLORREF fill = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        COLORREF border = RGB(0xAD, 0xAD, 0xAD);
        if (di->itemState & ODS_DISABLED) {
            /* 置灰：往白色方向混 75%，仍能看出原来的颜色 */
            fill = RGB((GetRValue(fill) + 255 * 3) / 4,
                       (GetGValue(fill) + 255 * 3) / 4,
                       (GetBValue(fill) + 255 * 3) / 4);
            border = RGB(0xD0, 0xD0, 0xD0);
        }
        PaintRoundRectAA(di->hDC, &di->rcItem, DpiScale(3), border, fill);
        return TRUE;
    }

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3));
        FillRect((HDC)w, &rc, bg);
        DeleteObject(bg);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        /* 页签区灰底 */
        HBRUSH bg = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3));
        RECT head = { 0, 0, rc.right, DpiScale(CONTENT_Y - 6) };
        FillRect(dc, &head, bg);
        RECT foot = { 0, rc.bottom - DpiScale(BOTTOM_H) + DpiScale(6), rc.right, rc.bottom };
        FillRect(dc, &foot, bg);
        DeleteObject(bg);
        PaintTabs(dc);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_CLOSE:
        RefreshDirty();          /* 以真实值判断：改回原值就不该拦 */
        if (g_dirty) {
            int r = MessageBoxW(h, L"有未保存的修改，要保存吗？",
                                L"IMEIndicator 设置", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) DoSave(0);
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        HintHide();
        if (g_widthDirty && s_cfg.settingsWidth > 0) SaveSettingsWidth(s_cfg.settingsWidth);
        /* 只删自己建的字体：MakeFont 失败时用的是系统字体，删了会连带影响别的窗口 */
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = NULL; }
        if (g_whiteBrush) { DeleteObject(g_whiteBrush); g_whiteBrush = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------- 入口 ---------- */
static void SetDpiAware(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
    PFN_SetProcessDpiAwarenessContext p1 =
        u ? (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (p1) p1((HANDLE)(INT_PTR)-4);
}

static UINT QueryDpi(HWND h) {
    typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pfn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) pfn = (PFN_GetDpiForWindow)GetProcAddress(u, "GetDpiForWindow");
    }
    if (pfn && h) {
        UINT d = pfn(h);
        if (d) return d;
    }
    HDC dc = h ? GetDC(h) : GetDC(NULL);
    UINT d = 96;
    if (dc) {
        int v = GetDeviceCaps(dc, LOGPIXELSY);
        if (v > 0) d = (UINT)v;
        ReleaseDC(h, dc);
    }
    return d;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;

    SetDpiAware();

    /* WIC 解码图片需要 COM（预览与「图片…」导入都会用到） */
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    CfgLoad(&s_cfg);
    g_base = s_cfg;      /* 脏标记的基线 */
    SyncLastShapes();

    RegisterPanelClass(hInst);

    {   /* 自绘下拉与它的弹出列表 */
        WNDCLASSW sc;
        ZeroMemory(&sc, sizeof(sc));
        sc.lpfnWndProc = SelProc;
        sc.hInstance = hInst;
        sc.lpszClassName = SEL_CLASS;
        sc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassW(&sc);

        WNDCLASSW sp;
        ZeroMemory(&sp, sizeof(sp));
        sp.lpfnWndProc = SelPopupProc;
        sp.hInstance = hInst;
        sp.lpszClassName = SEL_POPUP;
        sp.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassW(&sp);
    }

    static const WCHAR pcls[] = L"IMEIndicatorPreview";
    WNDCLASSW pc;
    ZeroMemory(&pc, sizeof(pc));
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = hInst;
    pc.lpszClassName = pcls;
    pc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&pc);

    {   /* 悬停说明气泡 */
        WNDCLASSW hc;
        ZeroMemory(&hc, sizeof(hc));
        hc.lpfnWndProc = HintProc;
        hc.hInstance = hInst;
        hc.lpszClassName = HINT_CLASS;
        hc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        RegisterClassW(&hc);
    }

    static const WCHAR cls[] = L"IMEIndicatorSettings";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    /* 窗口尺寸按客户区反推：WIN_W/WIN_H 是内容尺寸，标题栏和边框会额外吃掉
       三十来个逻辑像素。DPI 只取一次，WM_CREATE 沿用同一值。
       宽度恢复上次用过的值（不窄于设计宽度，否则右列会被裁掉）。 */
    g_startupDpi = QueryDpi(NULL);
    if (g_startupDpi == 0) g_startupDpi = 96;
    int dpi = (int)g_startupDpi;

    /* WS_CLIPCHILDREN：父窗口重画（切页签、悬停高亮、缩放）时不要再涂到子窗口
       底下去 —— 否则整窗先擦一遍底再让子控件逐个补画，看着就是整片在闪。 */
    DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;

    int logicalW = s_cfg.settingsWidth > WIN_W ? s_cfg.settingsWidth : WIN_W;
    if (logicalW > 1600) logicalW = 1600;      /* 先粗夹一刀，再按工作区夹 */
    g_loadedWidth = logicalW;
    RECT rc = { 0, 0, MulDiv(logicalW, dpi, 96), MulDiv(WIN_H, dpi, 96) };
    ClientToWindowSize(&rc, style, 0, (UINT)dpi);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int waw = wa.right - wa.left, wah = wa.bottom - wa.top;
    if (ww > waw - 20) ww = waw - 20;
    if (wh > wah - 20) wh = wah - 20;
    int wx = wa.left + (waw - ww) / 2;
    int wy = wa.top + (wah - wh) / 2;

    HWND h = CreateWindowExW(WS_EX_COMPOSITED, cls, L"IMEIndicator 设置",
                             style, wx, wy, ww, wh,
                             NULL, NULL, hInst, NULL);
    if (!h) return 1;

    ShowWindow(h, SW_SHOWNORMAL);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ImgShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}
