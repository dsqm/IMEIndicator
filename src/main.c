#include "ime_indicator.h"
#include <stdlib.h>  /* calloc / free */
#include <stdio.h>   /* _snwprintf_s */
#include <wchar.h>   /* wcscmp */

/* ================= 主程序：托盘 + 检测线程 =================
   主线程：隐藏托盘宿主窗口的消息循环（托盘点右键 -> 重启/退出）。
   工作线程：循环检测 状态(黑名单/大写/键盘布局) + 光标位置，驱动悬浮圆点。
   g_cfg / g_showDot 为全局共享（worker 写，主线程只读，仅有"晚一拍"差异）。 */

ImeCfg g_cfg;
volatile LONG g_showDot = 0;

#define WM_TRAYICON (WM_APP + 1)
#define IDM_RESTART 1001
#define IDM_EXIT    1002
#define IDM_LOG     1003
#define IDM_OPENCFG 1004

static HWND  g_trayWnd = NULL;
static HICON g_icon = NULL;
static UINT  g_msgTaskbarCreated = 0;   /* explorer 重启通知 */
static const WCHAR* APP_NAME = L"IMEIndicator";

/* ---------- DPI 感知：必须在建任何窗口之前 ---------- */
static void SetDpiAwareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
    typedef BOOL (WINAPI* PFN_SetProcessDpiAware)(void);
    typedef HRESULT (WINAPI* PFN_SetProcessDpiAwareness)(int);
    PFN_SetProcessDpiAwarenessContext p1 =
        u ? (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (p1 && p1((HANDLE)(INT_PTR)-4)) return;   /* PER_MONITOR_AWARE_V2 */
    HMODULE s = GetModuleHandleW(L"shcore.dll");
    PFN_SetProcessDpiAwareness p2 =
        s ? (PFN_SetProcessDpiAwareness)GetProcAddress(s, "SetProcessDpiAwareness") : NULL;
    if (p2 && SUCCEEDED(p2(2))) return;          /* PROCESS_PER_MONITOR_DPI_AWARE */
    PFN_SetProcessDpiAware p3 =
        u ? (PFN_SetProcessDpiAware)GetProcAddress(u, "SetProcessDPIAware") : NULL;
    if (p3) p3();
}

/* ---------- 托盘图标：字母 I ---------- */
static void FillRect32(BYTE* buf, int sz, int x0, int y0, int x1, int y1,
                       BYTE cr, BYTE cg, BYTE cb, BYTE ca) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > sz) x1 = sz; if (y1 > sz) y1 = sz;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            BYTE* p = buf + (y * sz + x) * 4;
            p[0] = cb; p[1] = cg; p[2] = cr; p[3] = ca;
        }
}

/* 衬线体 "I"：上横 + 竖 + 下横。先描一圈深色、再填白 —— 深浅任务栏都看得清。 */
static HICON MakeTrayIcon(void) {
    const int sz = 32;
    BYTE* buf = (BYTE*)calloc((size_t)sz * sz, 4);
    if (!buf) return NULL;
    const int barX0 = 7, barX1 = 25;   /* 上下横：宽 18 */
    const int stemX0 = 13, stemX1 = 19;/* 竖：宽 6 */
    const int topY0 = 6, topY1 = 11;
    const int botY0 = 21, botY1 = 26;
    /* 深色描边（各向外扩 1px） */
    FillRect32(buf, sz, barX0 - 1, topY0 - 1, barX1 + 1, topY1 + 1, 0x1A, 0x1A, 0x1A, 255);
    FillRect32(buf, sz, barX0 - 1, botY0 - 1, barX1 + 1, botY1 + 1, 0x1A, 0x1A, 0x1A, 255);
    FillRect32(buf, sz, stemX0 - 1, topY0 - 1, stemX1 + 1, botY1 + 1, 0x1A, 0x1A, 0x1A, 255);
    /* 白色本体 */
    FillRect32(buf, sz, barX0, topY0, barX1, topY1, 0xFF, 0xFF, 0xFF, 255);
    FillRect32(buf, sz, barX0, botY0, barX1, botY1, 0xFF, 0xFF, 0xFF, 255);
    FillRect32(buf, sz, stemX0, topY0, stemX1, botY1, 0xFF, 0xFF, 0xFF, 255);

    HBITMAP hbm = CreateBitmap(sz, sz, 1, 32, buf);
    free(buf);
    if (!hbm) return NULL;
    HICON ic = NULL;
    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = CreateBitmap(sz, sz, 1, 1, NULL);
    ii.hbmColor = hbm;
    ic = CreateIconIndirect(&ii);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DeleteObject(hbm);
    return ic;
}

/* ---------- 托盘菜单 ---------- */
static void ShowTrayMenu(void) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_LOG, L"记录日志");
    AppendMenuW(m, MF_STRING, IDM_OPENCFG, L"打开配置");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_RESTART, L"重启");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    CheckMenuItem(m, IDM_LOG, g_logging ? MF_CHECKED : MF_UNCHECKED);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_trayWnd);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_trayWnd, NULL);
    /* 宿主窗口不可见 → 菜单不会在点到外面时自动收起；补一条 WM_NULL 唤醒消息循环 */
    PostMessageW(g_trayWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* 用记事本打开配置文件（改完点托盘「重启」生效） */
static void OpenConfig(void) {
    WCHAR path[MAX_PATH];
    CfgPath(path, MAX_PATH);
    ShellExecuteW(NULL, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
}

/* 前台焦点窗口的描述串："[窗口类] 进程名"  —— 诊断哪个程序漂移用 */
static void FgDesc(WCHAR* out, size_t cap) {
    out[0] = 0;
    HWND f = ImeFocusedWindow();
    if (!f) { _snwprintf_s(out, cap, _TRUNCATE, L"[null]"); return; }
    WCHAR cls[128] = L"", proc[MAX_PATH] = L"";
    GetClassNameW(f, cls, 128);
    DWORD pid = 0;
    GetWindowThreadProcessId(f, &pid);
    if (pid) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            DWORD sz = MAX_PATH; WCHAR p[MAX_PATH];
            if (QueryFullProcessImageNameW(h, 0, p, &sz)) {
                WCHAR* s = p;
                for (WCHAR* q = p; *q; q++) if (*q == L'\\') s = q + 1;
                lstrcpynW(proc, s, MAX_PATH);
            }
            CloseHandle(h);
        }
    }
    _snwprintf_s(out, cap, _TRUNCATE, L"[%s] %s", cls, proc);
}

/* ---------- 单实例 ---------- */
static HANDLE g_single = NULL;

static int SingleInstanceAcquire(void) {
    g_single = CreateMutexW(NULL, FALSE, L"IMEIndicator_SingleInstance");
    if (!g_single) return 1;                       /* 建不了就照常跑，别把程序卡死 */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_single);
        g_single = NULL;
        return 0;                                  /* 已有一个实例在跑 */
    }
    return 1;
}

static void SingleInstanceRelease(void) {
    if (g_single) { CloseHandle(g_single); g_single = NULL; }
}

static void RestartApp(void) {
    WCHAR exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) > 0) {
        /* 先放互斥量再拉继任者：否则继任者抢先建锁、发现自己"已存在"直接退出 */
        SingleInstanceRelease();
        ShellExecuteW(NULL, L"open", exe, NULL, NULL, SW_SHOWNORMAL);
    }
    PostQuitMessage(0);
}

static void TrayAddIcon(void);   /* 前向声明：explorer 重启时要重新挂图标 */

static LRESULT CALLBACK TrayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    /* explorer 重启：系统把整个通知区清空，图标连同悬停提示一起没了，
       必须自己重新挂上。提示文案是**静态**的（名字 + 权限），所以重新
       ADD 时写进去就是完整文案 —— 不需要"按状态重建提示"那套逻辑。 */
    if (g_msgTaskbarCreated && m == g_msgTaskbarCreated) { TrayAddIcon(); return 0; }
    if (m == WM_TRAYICON) {
        if (l == WM_RBUTTONUP) { ShowTrayMenu(); return 0; }
    } else if (m == WM_COMMAND) {
        switch (LOWORD(w)) {
        case IDM_RESTART: RestartApp(); return 0;
        case IDM_OPENCFG: OpenConfig();  return 0;
        case IDM_EXIT:    PostQuitMessage(0); return 0;
        case IDM_LOG:
            InterlockedExchange(&g_logging, g_logging ? 0 : 1);
            return 0;
        }
        return 0;
    } else if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 悬停提示：程序名 + 权限标注。
   权限查**进程令牌**而不是配置值 —— 配置与实际不一致时不会误导。
   文案是静态的（两个信息都不会变），所以只要每次挂图标都写完整文案，
   explorer 重启后重新挂也自然正确。 */
static void TrayAddIcon(void) {
    if (!g_trayWnd) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_icon ? g_icon : LoadIconW(NULL, IDI_APPLICATION);
    _snwprintf_s(nid.szTip, sizeof(nid.szTip) / sizeof(WCHAR), _TRUNCATE,
                 L"%s%s", APP_NAME, ProcIsElevated() ? L"（管理员）" : L"");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void TrayInstall(void) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    static const WCHAR cls[] = L"IMEIndicatorTray";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hi;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    g_trayWnd = CreateWindowExW(0, cls, APP_NAME,
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
                                NULL, NULL, hi, NULL);
    if (!g_trayWnd) return;

    if (!g_icon) g_icon = MakeTrayIcon();
    TrayAddIcon();
}

static void TrayRemove(void) {
    if (!g_trayWnd) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_icon) { DestroyIcon(g_icon); g_icon = NULL; }
}

/* 来源名（日志用） */
static const WCHAR* SrcName(CaretSource s) {
    switch (s) {
    case CARET_GUIINFO:   return L"guiinfo";
    case CARET_MSAA:       return L"msaa";
    case CARET_UIA_CARET: return L"uia_caret";
    case CARET_UIA_SEL:   return L"uia_sel";
    case CARET_IME:       return L"ime";
    default:              return L"none";
    }
}
static const WCHAR* StateName(ImeState s) {
    switch (s) {
    case IMEST_CAPS:   return L"CAPS";
    case IMEST_KBD_EN: return L"KBD_EN";
    case IMEST_CN:     return L"CN";
    case IMEST_JP:     return L"JP";
    case IMEST_KR:     return L"KR";
    case IMEST_EN:     return L"EN";
    default:           return L"?";
    }
}
/* 各状态对应的圆点颜色；IME_COLOR_NONE = 该状态不显示圆点 */
static DWORD StateColor(ImeState s) {
    switch (s) {
    case IMEST_CAPS:   return g_cfg.capsrgb;
    case IMEST_KBD_EN: return g_cfg.kbdEnrgb;
    case IMEST_CN:     return g_cfg.cnrgb;
    case IMEST_JP:     return g_cfg.jprgb;
    case IMEST_KR:     return g_cfg.krrgb;
    default:           return g_cfg.enrgb;
    }
}

/* ---------- 检测线程 ---------- */
static DWORD WINAPI DetectorThread(LPVOID param) {
    (void)param;
    int pollMs = g_cfg.pollMs, trackMs = g_cfg.trackMs;
    /* 中英模式查询（跨进程 SendMessage）仅在其结果会影响显示时才做：
       En 与 Cn 至少一个非 0 才查；两个都是 0（都不显示）时跳过，避免卡顿。 */
    const int needMode = (g_cfg.enrgb != IME_COLOR_NONE || g_cfg.cnrgb != IME_COLOR_NONE);
    ImeSetForcedStrategy(g_cfg.imeStrategy);
    ULONGLONG lastPoll = 0, lastLog = 0;
    int shown = 0;
    ImeState cur = IMEST_EN;
    int wasLogging = 0;
    int wasBlocked = 0;
    WCHAR lastFg[256] = L"";
    ImeProbe pr;                /* 保留最近一次探测结果：循环比探测快，
                                   日志心跳不能拿"本轮没探测"当成查询失败 */
    ZeroMemory(&pr, sizeof(pr));
    pr.opened = -1;
    pr.conv = -1;

    for (;;) {
        Sleep(trackMs > 0 ? (DWORD)trackMs : 15);
        if (g_showDot < 0) break;   /* 退出信号 */

        /* 日志开关边沿：上升沿先取（要在 wasLogging 被刷新之前算），
           下降沿释放文件句柄（日志已关闭，文件不再被占用）。 */
        int rising = (g_logging && !wasLogging);
        if (!g_logging && wasLogging) DbgClose();
        wasLogging = g_logging ? 1 : 0;

        /* 上升沿写一版环境头（DPI + 焦点窗口），便于定位坐标空间问题 */
        if (rising) {
            HDC hdc = GetDC(NULL);
            int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 0;
            if (hdc) ReleaseDC(NULL, hdc);
            FgDesc(lastFg, 256);
            DbgLog(L"== logging on  screenDPI=%d fg:%s (process DPI-aware per-monitor)", dpi, lastFg);
        }

        /* [Ignore] 命中：跳过中英状态检测（中文组合查询会跨进程发消息，可能
           卡顿）。光标追踪照常，圆点保持上一次颜色。 */
        int blocked = CfgBlockedForeground();
        if (blocked != wasBlocked) {
            wasBlocked = blocked;
            if (g_logging) DbgLog(L"%s", blocked ? L"IGNORE: skip-detect" : L"IGNORE: resume-detect");
        }

        /* 状态（按 pollMs 节流重算颜色）；命中黑名单时本段跳过 */
        ULONGLONG now = GetTickCount64();
        int stateChanged = 0;
        if (!blocked && now - lastPoll >= (ULONGLONG)pollMs) {
            lastPoll = now;
            ImeState prev = cur;
            int lang = ImeKeyboardLang();
            /* 大写 / 英文键盘 / 日 / 韩 这四条路不查 IME，pr 标记为"本次没探" */
            if (ImeIsCapsLock()) { cur = IMEST_CAPS; pr.ok = 0; pr.opened = -1; pr.conv = -1; }
            else if (lang < 0)   { /* 没有前台窗口：沿用上次状态，别去查（会读到自己） */ }
            else if (lang == 0x09) { cur = IMEST_KBD_EN; pr.ok = 0; pr.opened = -1; pr.conv = -1; }
            else if (lang == 0x11) { cur = IMEST_JP;     pr.ok = 0; pr.opened = -1; pr.conv = -1; }
            else if (lang == 0x12) { cur = IMEST_KR;     pr.ok = 0; pr.opened = -1; pr.conv = -1; }
            else if (needMode) {
                int cn = ImeIsChineseModeEx(&pr);
                cur = cn ? IMEST_CN : IMEST_EN;
            } else {
                cur = IMEST_EN;
                pr.ok = 0;
                pr.opened = -1;
                pr.conv = -1;
            }
            if (cur != prev) {
                stateChanged = 1;
                OverlaySetColor(StateColor(cur), g_cfg.dotAlpha);
            }
        }

        /* 通用规则：当前状态色为 IME_COLOR_NONE -> 不显示圆点（如 Cn=0 时中文态隐藏） */
        int want = (StateColor(cur) != IME_COLOR_NONE);

        /* 光标追踪：找到就跟随，找不到就隐藏 */
        CaretPos cp;
        ZeroMemory(&cp, sizeof(cp));
        int got = (want && CaretGetPos(&cp));

        /* 日志：状态变化 / 前台窗口变化 / 每 500ms 心跳各记一行。
           不能只在"找到光标"时写 —— 光标一丢就整个日志空掉，查不到问题。 */
        if (g_logging) {
            WCHAR fg[256];
            FgDesc(fg, 256);
            int fgChanged = (wcscmp(fg, lastFg) != 0);
            if (fgChanged) {
                lstrcpynW(lastFg, fg, 256);
                RECT wr = {0,0,0,0};
                HWND hf = ImeFocusedWindow();
                if (hf) GetWindowRect(hf, &wr);
                DbgLog(L"FG -> %s  winRect=(%d,%d)-(%d,%d)",
                       fg, wr.left, wr.top, wr.right, wr.bottom);
            }
            if (stateChanged || fgChanged || now - lastLog >= 500) {
                lastLog = now;
                DbgLog(L"state=%s want=%d caret=%s(%d,%d,h=%d) src=%s fs=%d | "
                       L"opened=%d conv=0x%X ok=%d strat=%d nb=%d | %s",
                       StateName(cur), want, got ? L"hit" : L"miss",
                       cp.x, cp.y, cp.h, SrcName(cp.source),
                       CaretIsForegroundFullscreen(),
                       pr.opened, (DWORD)pr.conv, pr.ok, pr.strategy, pr.nonBinary,
                       fg);
            }
        }

        if (got) {
            if (!shown) { OverlaySetVisible(1); shown = 1; }
            OverlayMove(cp.x, cp.y + cp.h);
        } else {
            if (shown) { OverlaySetVisible(0); shown = 0; }
        }
    }
    return 0;
}

int WINAPI WinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE hPrev,
                   _In_ LPSTR lpCmd, _In_ int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    if (!SingleInstanceAcquire()) return 0;   /* 已有一个实例在跑，直接退出 */

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    SetDpiAwareness();          /* 必须最早 */
    DbgInit();

    /* --log：启动即开日志，省得先去托盘点一下（排查"日志不生成"时用它） */
    const WCHAR* cmdline = GetCommandLineW();
    if (cmdline && wcsstr(cmdline, L"--log")) InterlockedExchange(&g_logging, 1);

    ZeroMemory(&g_cfg, sizeof(g_cfg));
    CfgLoad(&g_cfg);            /* 缺配置时在 exe 同目录生成模板 */

    OverlayInit(&g_cfg);        /* 创建悬浮圆点窗口（本线程，走同一消息循环） */
    OverlaySetColor(StateColor(IMEST_EN), g_cfg.dotAlpha);  /* 首帧色（未变状态前用它） */
    TrayInstall();

    HANDLE th = CreateThread(NULL, 0, DetectorThread, NULL, 0, NULL);
    if (!th) return 1;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    InterlockedExchange(&g_showDot, -1);   /* 通知 worker 退出 */
    WaitForSingleObject(th, 1000);
    CloseHandle(th);
    TrayRemove();
    OverlayShutdown();
    SingleInstanceRelease();
    return 0;
}