#include "ime_indicator.h"
#include "version.h"    /* 构建脚本按当前日期生成（版本号 = 编译日期） */
#include "bridge.h"     /* 降权桥：管理员时替本进程跑 WPS 的 COM 查询 */
#include "wps.h"        /* WpsBridgeHandler：桥子进程的请求处理 */
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
#define IDM_GITHUB  1005

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
    AppendMenuW(m, MF_STRING, IDM_GITHUB, L"打开GitHub仓库");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_LOG, L"记录日志");
    AppendMenuW(m, MF_STRING, IDM_OPENCFG, L"打开配置");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_RESTART, L"重启程序");
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
        case IDM_GITHUB:
            ShellExecuteW(NULL, L"open", L"https://github.com/dsqm/IMEIndicator",
                          NULL, NULL, SW_SHOWNORMAL);
            return 0;
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
                 L"%s v%s%s", APP_NAME, IME_VER_W,
                 ProcIsElevated() ? L"（管理员）" : L"");
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
    case CARET_WPS:       return L"wps";
    case CARET_WPP:       return L"wpp";
    case CARET_ET:        return L"et";
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

/* 该状态是否走"临时显示"（露一小会儿就收）：AutoHideMs=0 或状态不在名单里 = 常显 */
static int StateIsTransient(ImeState s) {
    return g_cfg.autoHideMs > 0 && (g_cfg.autoHideMask & IME_AH_BIT(s)) != 0;
}

/* ---------- 检测线程 ---------- */

/* 上屏宽限：浮窗消失（上屏/Esc）后的这段时间里，光标的前跳/回跳是输入法的
   自动行为，不算"光标变化"—— 连续输入 nihao_w_s_ 时，组合串增长推着光标走、
   上屏又让它前跳，若照常续命，圆点会在"遮挡隐藏 <-> 上屏显示"之间来回闪。
   只影响续命（临时显示的倒计时），坐标跟随照旧。 */
#define IME_COMMIT_GRACE_MS 600

static DWORD WINAPI DetectorThread(LPVOID param) {
    (void)param;
    if (GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))
        SetThreadDescription(GetCurrentThread(), L"detector");
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
    HWND lastTop = NULL;        /* 上一次的前台顶层窗口 */
    int  settleGate = 0;        /* 1=刚换窗口、状态未定，这段不显示圆点 */
    ULONGLONG gateAt = 0;       /* 闸门最近一次"收起"的时刻 */
    int  wasSettling = 0;       /* 上一拍的 settling，用来抓 0->1 跳变 */
    /* ---- 临时显示（AutoHideMs / AutoHideStates） ----
       ahAt = 最近一次"值得亮一下"的时刻。三种由头会刷新它：状态变化、光标坐标变化、
       越过"状态未定"期（换窗口/换焦点控件后放行）。其后 AutoHideMs 内一直显示，
       到期自动收起；再动再亮。起点取线程启动时刻 —— 启动先亮一下告知当前状态。 */
    ULONGLONG ahAt = GetTickCount64();
    CaretPos lastCp;            /* 最近一次查询到的坐标：判"有没有动"、以及超时时顶替 */
    ZeroMemory(&lastCp, sizeof(lastCp));
    int      haveLastCp = 0;    /* 0=还没查到过有效坐标，此时 lastCp 的内容不算数 */
    int      wasOccl = 0;       /* 上一拍的浮窗遮挡状态：抓 1->0 边沿（上屏/取消） */
    ULONGLONG occlEndAt = 0;    /* 浮窗最近一次消失的时刻（上屏宽限起点） */

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

        /* [Ignore] 黑名单边沿：进/出各记一条日志；命中时的行为见下一块注释。 */
        int blocked = CfgBlockedForeground();
        if (blocked != wasBlocked) {
            wasBlocked = blocked;
            if (g_logging) DbgLog(L"%s", blocked ? L"IGNORE: skip-detect" : L"IGNORE: resume-detect");
        }

        /* [Ignore] 命中 / 进程名读不到（受保护进程）= **彻底隐身**：不去碰
           前台程序（跨进程查询是卡顿与干扰的根），圆点也不显示。离开时清掉
           旧坐标，让它第一帧算一次"光标变化"重新亮。 */
        if (blocked) {
            if (shown) { OverlaySetVisible(0); shown = 0; }
            haveLastCp = 0;
            lastPoll = 0;               /* 解除后立即重算状态，别沿用黑名单期间的值 */
            continue;
        }

        /* 前台顶层窗口换了：**立刻**重算状态（别等 pollMs 那一拍，否则旧状态
           会多显示最多 100ms），并进入"状态未定"期 —— 这期间显示任何颜色都是猜的。
           判定用顶层窗口而不是焦点控件：同一个窗口内换控件（浏览器地址栏↔页面）
           不该让点闪一下；那种情况交给 ime.c 的 settling 去挡（它本来就按焦点控件
           判稳定），两者合起来覆盖完整。 */
        ULONGLONG now = GetTickCount64();
        HWND top = GetForegroundWindow();
        if (top != lastTop) {
            lastTop = top;
            lastPoll = 0;
            settleGate = 1;
            gateAt = now;
            if (g_logging) DbgLog(L"settle: arm fg=%p", (void*)top);
        }
        /* 状态（按 pollMs 节流重算颜色） */
        int stateChanged = 0;
        if (now - lastPoll >= (ULONGLONG)pollMs) {
            lastPoll = now;
            ImeState prev = cur;
            int lang = ImeKeyboardLang();
            /* 大写 / 英文键盘 / 日 / 韩 这四条路不查 IME，pr 标记为"本次没探"。
               ★ settling 必须一并清零：这些状态由键盘布局直接决定，没有"未定期"。
               不清的话 settling 残留 1 → 释放条件 !pr.settling 永假 → settleGate
               永久闭合 → 圆点从此不再显示（切到这类布局的窗口就触发，重启才恢复）。 */
            if (ImeIsCapsLock()) { cur = IMEST_CAPS; pr.ok = 0; pr.opened = -1; pr.conv = -1; pr.settling = 0; }
            else if (lang < 0)   { /* 没有前台窗口：沿用上次状态，别去查（会读到自己） */ pr.settling = 0; }
            else if (lang == 0x09) { cur = IMEST_KBD_EN; pr.ok = 0; pr.opened = -1; pr.conv = -1; pr.settling = 0; }
            else if (lang == 0x11) { cur = IMEST_JP;     pr.ok = 0; pr.opened = -1; pr.conv = -1; pr.settling = 0; }
            else if (lang == 0x12) { cur = IMEST_KR;     pr.ok = 0; pr.opened = -1; pr.conv = -1; pr.settling = 0; }
            else if (needMode) {
                int cn = ImeIsChineseModeEx(&pr);
                cur = cn ? IMEST_CN : IMEST_EN;
            } else {
                cur = IMEST_EN;
                pr.ok = 0;
                pr.opened = -1;
                pr.conv = -1;
                pr.settling = 0;
            }
            if (cur != prev) {
                stateChanged = 1;
                OverlaySetColor(StateColor(cur), g_cfg.dotAlpha);
                ahAt = now;                     /* 状态变化 = 一次临时显示的由头 */
            }
        }

        /* 状态：ime.c 一旦报"未定"就跟着收着 —— 它发现焦点变化可能比顶层窗口变化
           晚一拍，只靠上面那条会漏。 */
        if (pr.settling && !wasSettling) { settleGate = 1; gateAt = now; }
        wasSettling = pr.settling;

        /* 前台窗口可能在本次迭代**中途**才换（切前台与焦点控件更新不同拍，
           实测差 20~30ms），而上面那次检查跑在迭代开头 —— 显示前再确认一次，
           把这点缝也堵上。 */
        HWND topLate = GetForegroundWindow();
        if (topLate != lastTop) {
            lastTop = topLate;
            lastPoll = 0;
            settleGate = 1;
            gateAt = now;
            if (g_logging) DbgLog(L"settle: arm (late) fg=%p", (void*)topLate);
        }

        /* 放行条件：读数已稳 **且** 距上次收起至少过了 IME_SETTLE_MS。
           只判 settling 会被"还没刷新的旧值"提前放行 —— 实测出现过闸门当拍就被
           撤掉、旧颜色漏出来 20ms。加上最短按住时长，时序竞争也漏不出来。 */
        if (settleGate && !pr.settling && now - gateAt >= (ULONGLONG)IME_SETTLE_MS) {
            if (g_logging)
                DbgLog(L"settle: release held=%I64u ms", (unsigned long long)(now - gateAt));
            settleGate = 0;
            ahAt = now;   /* 换窗口/换焦点后放行：哪怕坐标没变，也该让人看一眼当前状态 */
        }

        /* 通用规则：当前状态色为 IME_COLOR_NONE -> 不显示圆点（如 Cn=0 时中文态隐藏）。
           "状态未定"期间同样不显示：宁可这段短暂没有点，也不要先亮错颜色再消失。 */
        int want = (!settleGate) && (StateColor(cur) != IME_COLOR_NONE);

        /* 浮窗打字检测：输入法组合窗/候选窗出现即让位。两条判据：
           纯组合期类名（白名单）直判；其余浮动样式窗要求在光标附近 ——
           百度/搜狗五笔/冰凌的常驻悬浮工具栏也挂在焦点线程上，不能误伤。
           want 已是 0 时不查。 */
        int occluded = wasOccl;     /* 本拍没查（want=0）时沿用上一拍，边沿判定才连续 */
        if (want && g_cfg.hideComposition) {
            HWND f = ImeFocusedWindow();
            RECT nd, *nearDot = NULL;
            if (haveLastCp) {       /* 用上一拍光标位置：组合窗总在光标旁出现 */
                nd.left = lastCp.x;             nd.top = lastCp.y;
                nd.right = nd.left + (lastCp.w > 0 ? lastCp.w : 1);
                nd.bottom = nd.top + (lastCp.h > 0 ? lastCp.h : 1);
                nearDot = &nd;
            }
            occluded = (f && ImeFloatOccluding(f, nearDot));
            if (occluded) want = 0;
        }
        if (!occluded && wasOccl) occlEndAt = now;   /* 遮挡结束：上屏或取消 */
        wasOccl = occluded;

        /* 光标追踪：找到就跟随，找不到就隐藏。
           CaretGetPosEx 内部是"交给查询线程 + 按超时等"：前台程序卡住时它返回 0
           而不是把检测线程一起冻住。这种情况**沿用上一轮的显示状态** —— 否则
           浏览器卡一下圆点就消失，看上去跟"坏了"一样。 */
        CaretPos cp;
        ZeroMemory(&cp, sizeof(cp));
        int got = 0, caretTimedOut = 0;
        if (want) {
            got = CaretGetPosEx(&cp, &caretTimedOut);
            /* timeoutOut 时 cp 已被清零，不能直接拿去 Move —— 那样圆点会甩到屏幕
               左上角（前台程序一卡就能看见）。顶替成最近一次的有效坐标即可；
               连这个都没有（从来没查到过）就维持现状，本帧按"没查到"处理。 */
            if (!got && caretTimedOut) {
                if (haveLastCp) cp = lastCp;
                got = (shown && haveLastCp) ? 1 : 0;
            }
        }

        /* 「该亮了」的第三种由头：光标坐标动了。
           坐标连同 h 一起比：换到另一个编辑框时 x/y 可能巧合相同，行高多半不同。
           ★ 超时顶替的坐标不会触发这里（它等于 lastCp），避免前台卡住时被无限续命。 */
        if (got) {
            int moved = !haveLastCp || cp.x != lastCp.x || cp.y != lastCp.y || cp.h != lastCp.h;
            if (moved) {
                /* 遮挡期间与遮挡结束后短宽限内的光标移动不续命：那是输入法的
                   自动行为（组合串增长推着 caret 走、上屏让 caret 前跳），不是
                   用户在动。位置照常更新，圆点该藏就藏、该停就停，不闪。 */
                int inGrace = occluded ||
                              (occlEndAt && now - occlEndAt < (ULONGLONG)IME_COMMIT_GRACE_MS);
                /* 小位移过滤：中文标点（，。？）被输入法**直接上屏**，不产生组合
                   窗 —— 浮窗检测和上屏宽限都盖不到，caret 只前进一两个字符位。
                   切比雪夫距离 ≤ 2×行高（h≈字格）的移动视为输入的直接结果，
                   不续命；副作用是方向键挪一格、点击相邻位置也不触发（已确认
                   接受：小幅移动不需要提示）。 */
                if (!inGrace && haveLastCp) {
                    long dx = cp.x - lastCp.x, dy = cp.y - lastCp.y;
                    if (dx < 0) dx = -dx;
                    if (dy < 0) dy = -dy;
                    long cell = cp.h > 0 ? cp.h : lastCp.h;
                    if (cell > 0 && dx <= 2 * cell && dy <= 2 * cell) inGrace = 1;
                }
                lastCp = cp;
                haveLastCp = 1;
                if (!inGrace) ahAt = now;
            }
        } else {
            haveLastCp = 0;   /* 光标确实没了：丢掉陈旧坐标，再现时算一次变化 */
        }

        /* 临时显示到期：名单里的状态亮够 AutoHideMs 就收起。
           注意收起期间**照旧查询光标** —— 否则察觉不到"用户又开始动了"，圆点就再也
           回不来。开销与"常显状态"完全相同（本来也是每 trackMs 查一次）。 */
        int transient = StateIsTransient(cur);
        ULONGLONG ahLeft = 0;
        if (transient) {
            ULONGLONG held = now - ahAt;
            unsigned ms = (unsigned)g_cfg.autoHideMs;
            if (held >= (ULONGLONG)ms) got = 0;
            else ahLeft = (ULONGLONG)ms - held;
        }

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
                DbgLog(L"state=%s want=%d caret=%s(%d,%d,w=%d,h=%d,d=%d) src=%s fs=%d comp=%d gate=%d to=%d "
                       L"ah=%s ttl=%I64u | "
                       L"opened=%d conv=0x%X ok=%d strat=%d nb=%d | %s",
                       StateName(cur), want, got ? L"hit" : L"miss",
                       cp.x, cp.y, cp.w, cp.h, cp.depth, SrcName(cp.source),
                       CaretIsForegroundFullscreen(), occluded, settleGate, caretTimedOut,
                       transient ? L"temp" : L"keep", (unsigned long long)ahLeft,
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

    SetDpiAwareness();          /* 必须最早（桥子进程也要：它算的是屏幕物理像素） */
    DbgInit();

    /* --log：启动即开日志，省得先去托盘点一下（排查"日志不生成"时用它） */
    const WCHAR* cmdline = GetCommandLineW();
    if (cmdline && wcsstr(cmdline, L"--log")) InterlockedExchange(&g_logging, 1);

    /* --bridge <父pid> <序号>：降权桥子进程。不建托盘、不建浮窗、不抢单例
       —— 它只是"普通权限的自己"，替管理员父进程跑 WPS 的 COM 查询（ROT 按
       完整性级别隔离，父进程自己附不上，见 bridge.c 文件头）。 */
    DWORD bpid = 0, bseq = 0;
    if (BrParseChildSwitch(&bpid, &bseq)) {
        ZeroMemory(&g_cfg, sizeof(g_cfg));
        CfgLoad(&g_cfg);                 /* 只要 wpsCom 开关（用户关了就别查） */
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        int rc = BrServe(bpid, bseq, WpsBridgeHandler);
        if (SUCCEEDED(hr)) CoUninitialize();
        DbgShutdown();
        return rc;
    }

    if (!SingleInstanceAcquire()) return 0;   /* 已有一个实例在跑，直接退出 */

    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    ZeroMemory(&g_cfg, sizeof(g_cfg));
    CfgLoad(&g_cfg);            /* 缺配置时在 exe 同目录生成模板 */

    /* 版本号 = 编译日期（构建脚本生成 src/version.h；exe 属性里也能看到） */
    DbgLog(L"IMEIndicator v%s start", IME_VER_W);

    OverlayInit(&g_cfg);        /* 创建悬浮圆点窗口（本线程，走同一消息循环） */
    OverlaySetColor(StateColor(IMEST_EN), g_cfg.dotAlpha);  /* 首帧色（未变状态前用它） */
    TrayInstall();

    /* 光标查询走独立线程 + 超时：跨进程 UIA/MSAA 调用没有超时参数，前台程序
       卡住会把调用方一起冻住（心跳断档数秒就是这么来的）。 */
    CaretWorkerStart(g_cfg.caretTimeoutMs);

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
    CaretWorkerStop();                     /* 检测线程已退出，此时回收查询线程 */
    BrShutdown();                          /* 通知降权桥子进程收工（它也会自己看父进程） */
    TrayRemove();
    OverlayShutdown();
    SingleInstanceRelease();
    return 0;
}