#include "ime_status.h"
#include <stdlib.h>  /* calloc / free */

/* ================= 主程序：托盘 + 检测线程 =================
   主线程：隐藏托盘宿主窗口的消息循环（托盘点右键 -> 重启/退出）。
   工作线程：循环检测 状态(黑名单/大写/键盘布局) + 光标位置，驱动悬浮圆点。
   g_cfg / g_showDot 为全局共享（worker 写，主线程只读，仅有"晚一拍"差异）。 */

ImeCfg g_cfg;
volatile LONG g_showDot = 0;

#define WM_TRAYICON (WM_APP + 1)
#define IDM_RESTART 1001
#define IDM_EXIT    1002

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
    AppendMenuW(m, MF_STRING, IDM_RESTART, L"重启");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_trayWnd);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_trayWnd, NULL);
    DestroyMenu(m);
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

/* ---------- 检测线程 ---------- */
static DWORD WINAPI DetectorThread(LPVOID param) {
    (void)param;
    int pollMs = g_cfg.pollMs, trackMs = g_cfg.trackMs;
    ULONGLONG lastPoll = 0;
    int shown = 0;
    ImeState cur = IMEST_EN;

    for (;;) {
        Sleep(trackMs > 0 ? (DWORD)trackMs : 15);
        if (g_showDot < 0) break;   /* 退出信号 */

        /* 黑名单：前台程序命中 -> 整段隐藏 */
        if (CfgBlockedForeground()) {
            if (shown) { OverlaySetVisible(0); shown = 0; }
            continue;
        }

        /* 状态（按 pollMs 节流重算颜色） */
        ULONGLONG now = GetTickCount64();
        if (now - lastPoll >= (ULONGLONG)pollMs) {
            lastPoll = now;
            if (ImeIsCapsLock()) cur = IMEST_CAPS;
            else if (ImeIsEnglishKeyboard()) cur = IMEST_KBD_EN;
            else cur = IMEST_EN;   /* 中文输入法 / 其它布局：默认英文色（用户可不区分） */
            DWORD color, alpha = g_cfg.dotAlpha;
            switch (cur) {
            case IMEST_CAPS:   color = g_cfg.capsrgb;   break;
            case IMEST_KBD_EN: color = g_cfg.kbdEnrgb;  break;
            default:           color = g_cfg.cnrgb;     break; /* 默认=En 橘色 */
            }
            OverlaySetColor(color, alpha);
        }

        /* 英文态 / 中文不区分态 受 ShowWhenEnglish 控制；
           大写键始终显示 */
        int want = 1;
        if (cur != IMEST_CAPS && !g_cfg.showEn) want = 0;

        /* 光标追踪：找到就跟随，找不到就隐藏 */
        CaretPos cp;
        if (want && CaretGetPos(&cp)) {
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

    SetDpiAwareness();          /* 必须最早 */

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