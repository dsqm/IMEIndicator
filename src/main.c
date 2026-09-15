#include "ime_status.h"
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

static HWND g_trayWnd = NULL;
static HICON g_icon = NULL;

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

/* ---------- 托盘图标：画一个当前英文色的实心圆 ---------- */
static HICON MakeTrayIcon(DWORD rgb) {
    int sz = 32;
    BYTE* buf = (BYTE*)calloc((size_t)sz * sz, 4);
    if (!buf) return NULL;
    double cx = (sz - 1) / 2.0, cy = (sz - 1) / 2.0, r = sz / 2.0 - 1.0;
    BYTE cr = (BYTE)(rgb >> 16), cg = (BYTE)(rgb >> 8), cb = (BYTE)rgb;
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++) {
            double dx = x - cx + 0.5, dy = y - cy + 0.5;
            if (dx * dx + dy * dy <= r * r) {
                BYTE* p = buf + (y * sz + x) * 4;
                p[0] = cb; p[1] = cg; p[2] = cr; p[3] = 255;
            }
        }
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
    DestroyMenu(m);
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

static void RestartApp(void) {
    WCHAR exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH) > 0)
        ShellExecuteW(NULL, L"open", exe, NULL, NULL, SW_SHOWNORMAL);
    PostQuitMessage(0);
}

static LRESULT CALLBACK TrayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TRAYICON) {
        if (l == WM_RBUTTONUP) { ShowTrayMenu(); return 0; }
    } else if (m == WM_COMMAND) {
        switch (LOWORD(w)) {
        case IDM_RESTART: RestartApp(); return 0;
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

static void TrayInstall(void) {
    HINSTANCE hi = GetModuleHandleW(NULL);
    static const WCHAR cls[] = L"IMEStatusTray";
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hi;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);

    g_trayWnd = CreateWindowExW(0, cls, L"IMEStatus",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
                                NULL, NULL, hi, NULL);
    if (!g_trayWnd) return;

    g_icon = MakeTrayIcon(g_cfg.enrgb);

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_trayWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_icon ? g_icon : LoadIconW(NULL, IDI_APPLICATION);
    const WCHAR* tip = L"输入法状态";
    for (int i = 0; tip[i] && i < (int)(sizeof(nid.szTip) / sizeof(WCHAR)) - 1; i++)
        nid.szTip[i] = tip[i];
    Shell_NotifyIconW(NIM_ADD, &nid);
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
    case IMEST_HIDDEN: return L"HIDDEN";
    case IMEST_EN:     return L"EN";
    default:           return L"?";
    }
}

/* ---------- 检测线程 ---------- */
static DWORD WINAPI DetectorThread(LPVOID param) {
    (void)param;
    int pollMs = g_cfg.pollMs, trackMs = g_cfg.trackMs;
    ULONGLONG lastPoll = 0;
    int shown = 0;
    ImeState cur = IMEST_EN;
    int wasLogging = 0;
    WCHAR lastFg[256] = L"";

    for (;;) {
        Sleep(trackMs > 0 ? (DWORD)trackMs : 15);
        if (g_showDot < 0) break;   /* 退出信号 */

        /* 日志开关下降沿：释放文件句柄（日志已关闭，文件不再被占用） */
        if (!g_logging && wasLogging) DbgClose();
        wasLogging = g_logging ? 1 : 0;

        /* 日志开关上升沿：先写一版环境头（DPI + 焦点），便于定位坐标空间 */
        if (g_logging && !wasLogging) {
            HDC hdc = GetDC(NULL);
            int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 0;
            if (hdc) ReleaseDC(NULL, hdc);
            FgDesc(lastFg, 256);
            DbgLog(L"== logging on  screenDPI=%d fg:%s (process DPI-aware per-monitor)", dpi, lastFg);
        }
        wasLogging = g_logging ? 1 : 0;

        /* 黑名单：前台程序命中 -> 整段隐藏 */
        if (CfgBlockedForeground()) {
            if (g_logging) DbgLog(L"BLOCKED (ignore-list)");
            if (shown) { OverlaySetVisible(0); shown = 0; }
            continue;
        }

        /* 状态（按 pollMs 节流重算颜色） */
        ULONGLONG now = GetTickCount64();
        if (now - lastPoll >= (ULONGLONG)pollMs) {
            lastPoll = now;
            if (ImeIsCapsLock())            cur = IMEST_CAPS;
            else if (ImeIsEnglishKeyboard()) cur = IMEST_KBD_EN;
            else if (ImeIsChineseMode())    cur = IMEST_HIDDEN;  /* 中文输入：不显示 */
            else                            cur = IMEST_EN;      /* 中文输入法英文档 */
            DWORD color, alpha = g_cfg.dotAlpha;
            switch (cur) {
            case IMEST_CAPS:   color = g_cfg.capsrgb;   break;
            case IMEST_KBD_EN: color = g_cfg.kbdEnrgb;  break;
            default:           color = g_cfg.cnrgb;     break; /* 显隐由下面 want 决定 */
            }
            OverlaySetColor(color, alpha);
        }

        /* 中文输入不显示；英文/英文键盘受 ShowWhenEnglish 控制；大写键始终显示 */
        int want = (cur == IMEST_CAPS) ||
                   ((cur == IMEST_EN || cur == IMEST_KBD_EN) && g_cfg.showEn);

        /* 光标追踪：找到就跟随，找不到就隐藏 */
        CaretPos cp;
        if (want && CaretGetPos(&cp)) {
            if (g_logging) {
                WCHAR fg[256];
                FgDesc(fg, 256);
                if (wcscmp(fg, lastFg) != 0) {
                    lstrcpynW(lastFg, fg, 256);
                    DbgLog(L"FG -> %s", fg);
                }
                DbgLog(L"state=%s want=1 caret=(%d,%d,h=%d) src=%s fg:%s",
                       StateName(cur), cp.x, cp.y, cp.h, SrcName(cp.source), fg);
            }
            if (!shown) { OverlaySetVisible(1); shown = 1; }
            OverlayMove(cp.x, cp.y + cp.h);
        } else {
            if (g_logging && shown)
                DbgLog(L"hide state=%s want=%d", StateName(cur), want);
            if (shown) { OverlaySetVisible(0); shown = 0; }
        }
    }
    return 0;
}

int WINAPI WinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE hPrev,
                   _In_ LPSTR lpCmd, _In_ int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;

    SetDpiAwareness();          /* 必须最早 */
    DbgInit();

    ZeroMemory(&g_cfg, sizeof(g_cfg));
    CfgLoad(&g_cfg);            /* 缺配置时在 exe 同目录生成模板 */

    OverlayInit(&g_cfg);        /* 创建悬浮圆点窗口（本线程，走同一消息循环） */
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
    return 0;
}