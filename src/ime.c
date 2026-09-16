#include "ime_indicator.h"

/* 某些 Windows SDK 的 imm.h 未定义 IMC_GETCONVERSIONMODE，这里按标准值补上 */
#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif
#ifndef IMC_GETOPENSTATUS
#define IMC_GETOPENSTATUS     0x0005
#endif

/* 向别的进程的 IME 窗口发消息，最多等这么久（超时即视为本次没取到，沿用上次结果） */
#define IME_QUERY_TIMEOUT_MS 200

/* 信号要稳定保持 IME_SETTLE_MS 才认：切换瞬间两个信号会各飘一次，立刻采会读到
   中间值（表现为"切了没反应"或闪一下）；换窗口同理 —— 那时问到的还是上一个窗口的
   值（实测滞后 200~300ms）。检测线程也用同一时长决定"状态未定"期间不收圆点。 */

/* 前台线程的焦点窗口：GetForegroundWindow 只管"哪个顶层窗口在前台"，
   输入附着在焦点控件（hwndFocus）上，键盘布局 / IME 都要按它的线程取。 */
HWND ImeFocusedWindow(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return NULL;
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    GUITHREADINFO gi;
    ZeroMemory(&gi, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (tid && GetGUIThreadInfo(tid, &gi)) {
        if (gi.hwndFocus) return gi.hwndFocus;
        if (gi.hwndActive) return gi.hwndActive;
    }
    return fg;
}

int ImeIsCapsLock(void) {
    return (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;
}

/* ---------- 运行权限（查进程令牌，不看配置值） ---------- */
static int TokenElevated(HANDLE hProc) {
    HANDLE tok = NULL;
    int el = 0;
    if (OpenProcessToken(hProc, TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION e;
        DWORD sz = 0;
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &sz))
            el = e.TokenIsElevated ? 1 : 0;
        CloseHandle(tok);
    }
    return el;   /* 取不到就按"非管理员"处理：宁可少标，不要乱标 */
}

/* 本进程是否以管理员运行（托盘提示据此标注）—— 进程级信息，查一次即可 */
int ProcIsElevated(void) {
    static int cached = -1;
    if (cached < 0) cached = TokenElevated(GetCurrentProcess());
    return cached;
}

/* 指定窗口所属进程是否提权：用于判断"查询失败是不是 UIPI 挡的" */
static int WindowProcElevated(HWND hwnd) {
    DWORD pid = 0;
    if (!hwnd || !GetWindowThreadProcessId(hwnd, &pid) || !pid) return 0;
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return 0;
    int el = TokenElevated(hp);
    CloseHandle(hp);
    return el;
}

/* 取 IME 窗口：焦点窗口拿不到（UWP / 部分自绘控件 / 权限更高进程）时逐级退，
   最后退回本线程的默认 IME 窗口，别一上来就判"查不到"=英文。 */
static HWND FindImeWindow(HWND focus) {
    HWND ime = NULL;
    if (focus) ime = ImmGetDefaultIMEWnd(focus);
    if (!ime) {
        HWND fg = GetForegroundWindow();
        if (fg && fg != focus) ime = ImmGetDefaultIMEWnd(fg);
    }
    if (!ime) ime = ImmGetDefaultIMEWnd(NULL);
    return ime;
}

static void WarnUipiOnce(void);   /* 前向声明：ImeQuery 用到 */

/* 向 IME 窗口问一次 open 状态与转换模式。**两项都取到才算 ok**：
   只取到一项时另一项仍是 -1，而 (-1 & IME_CMODE_NATIVE) 为真 → 会误判成中文，
   还会把策略锁死在"看转换模式"上。宁可这次不算、沿用上次状态。 */
static int ImeQuery(HWND focus, int* opened, int* conv) {
    *opened = -1; *conv = -1;
    HWND ime = FindImeWindow(focus);
    if (!ime) return 0;
    int gotOpen = 0, gotConv = 0;
    DWORD_PTR v = 0;
    if (SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_GETOPENSTATUS, 0,
                            SMTO_ABORTIFHUNG, IME_QUERY_TIMEOUT_MS, &v)) {
        *opened = (int)v; gotOpen = 1;
    }
    if (SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_GETCONVERSIONMODE, 0,
                            SMTO_ABORTIFHUNG, IME_QUERY_TIMEOUT_MS, &v)) {
        *conv = (int)v; gotConv = 1;
    }
    if (!gotOpen || !gotConv) {
        /* 非管理员的我们向**管理员**窗口发消息会被 UIPI 静默拦掉（无任何报错），
           现象是"在某些程序里圆点不跟随"。查一下对方权限，命中就记日志提示。 */
        if (!ProcIsElevated() && WindowProcElevated(ime)) WarnUipiOnce();
        return 0;
    }
    return 1;
}

/* 同一条提示别刷屏：最多 60 秒记一次 */
static void WarnUipiOnce(void) {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 60000) return;
    last = now;
    DbgLog(L"UIPI: 目标程序以更高权限运行，IME 查询被系统拦截 —— 建议以管理员启动本程序");
}

/* ---------- 自适应判定 ----------
   输入法切中英时暴露的变化没有统一约定：
     - 多数第三方输入法（搜狗/百度/讯飞…）只翻 open 状态，转换模式恒定；
     - 有的只改转换模式，open 状态恒为 1；
     - 还有的 open 状态不是 0/1（例如 1024/1025 这种非二进制值）。
   所以两个信号都取，观察"哪个变了"来选定权威信号（strategy），
   非二进制 open 则记住它代表中文/英文的那两个具体值。
   思路与 InputTip 的 GeneralStrategy 一致。 */
typedef struct {
    HWND      lastHwnd;
    ULONGLONG hwndChanged;
    int       initPending;     /* 换窗口后先等稳定，别读上一窗口的残留值 */
    int       lastOpened;
    int       lastConv;
    long long pendingKey;      /* 待稳定的 (opened,conv) 组合 */
    ULONGLONG pendingTime;
    int       strategy;
    int       nonBinary;
    int       cnValue, enValue;
    int       pendingNonBinary;
    ULONGLONG nonBinaryTime;
    int       lastState;
} ImeStrat;

static ImeStrat g_s;
static int      g_forced = 0;   /* 用户在 ini 里强制指定的策略 */

void ImeSetForcedStrategy(int s) {
    g_forced = (s == 1 || s == 2) ? s : 0;
    if (g_forced) g_s.strategy = g_forced;
}

static void NonBinaryReset(void) {
    g_s.nonBinary = 0;
    g_s.cnValue = 0;
    g_s.enValue = 0;
    g_s.pendingNonBinary = 0;
    g_s.nonBinaryTime = 0;
}

int ImeIsChineseModeEx(ImeProbe* p) {
    HWND focus = ImeFocusedWindow();
    int opened = -1, conv = -1;
    /* 没有前台窗口时**绝不能**去查 IME：FindImeWindow 会退到本线程的默认 IME
       窗口，读回来的是"我自己"的输入法状态，不是前台应用的。 */
    int ok = focus ? ImeQuery(focus, &opened, &conv) : 0;
    ULONGLONG now = GetTickCount64();
    int skip = 0;

    if (focus != g_s.lastHwnd) {
        g_s.lastHwnd = focus;
        g_s.hwndChanged = now;
        g_s.pendingKey = 0;
        g_s.pendingTime = 0;
        g_s.pendingNonBinary = 0;
        g_s.nonBinaryTime = 0;
        g_s.initPending = 1;
        skip = 1;
    } else if (g_s.initPending) {
        if (now - g_s.hwndChanged < IME_SETTLE_MS) skip = 1;
        else {
            g_s.lastOpened = opened;
            g_s.lastConv = conv;
            g_s.initPending = 0;
        }
    }

    if (!skip && ok && !g_forced) {
        int openedChanged = (opened != g_s.lastOpened);
        int convChanged   = (conv != g_s.lastConv);
        if (openedChanged || convChanged) {
            long long key = (((long long)opened) << 32) | (long long)(DWORD)conv;
            if (key != g_s.pendingKey) {
                g_s.pendingKey = key;
                g_s.pendingTime = now;
                skip = 1;
            } else if (now - g_s.pendingTime < IME_SETTLE_MS) {
                skip = 1;
            } else if (convChanged) {
                if (g_s.strategy == 1 && g_s.nonBinary) NonBinaryReset();
                g_s.strategy = 2;
            } else {   /* openedChanged */
                if (g_s.strategy == 2 && g_s.nonBinary) g_s.nonBinary = 0;
                g_s.strategy = 1;
                if (opened > 1) {
                    if (g_s.pendingNonBinary != opened) {
                        g_s.pendingNonBinary = opened;
                        g_s.nonBinaryTime = now;
                        skip = 1;
                    } else if (now - g_s.nonBinaryTime < IME_SETTLE_MS) {
                        skip = 1;
                    } else {
                        g_s.nonBinary = 1;
                        g_s.cnValue = opened;
                        g_s.enValue = (g_s.lastOpened == -1 || g_s.lastOpened > 1)
                                      ? 0 : g_s.lastOpened;
                        g_s.pendingNonBinary = 0;
                        g_s.nonBinaryTime = 0;
                    }
                } else {
                    if (g_s.lastOpened >= 0 && g_s.lastOpened <= 1) {
                        if (g_s.nonBinary) NonBinaryReset();
                    } else {
                        g_s.pendingNonBinary = 0;
                        g_s.nonBinaryTime = 0;
                    }
                }
            }
            if (!skip) {
                g_s.lastOpened = opened;
                g_s.lastConv = conv;
                g_s.pendingKey = 0;
                g_s.pendingTime = 0;
            }
        } else {
            g_s.pendingKey = 0;
            g_s.pendingTime = 0;
        }
    }

    int state = g_s.lastState;
    if (ok && !g_s.initPending) {
        if (g_s.strategy == 1)
            state = g_s.nonBinary ? (opened == g_s.cnValue) : (opened ? 1 : 0);
        else if (g_s.strategy == 2)
            state = (conv & IME_CMODE_NATIVE) ? 1 : 0;
        else if (g_s.nonBinary)
            state = (opened == g_s.cnValue);
        else if (conv == 0)
            state = (opened ? 1 : 0);
        else if (opened == 0)
            state = 0;
        else if (conv & IME_CMODE_NATIVE)
            state = 1;
        else
            state = 0;
        g_s.lastState = state;
    }

    if (p) {
        p->hwnd = focus;
        p->ok = ok;
        p->opened = opened;
        p->conv = conv;
        p->strategy = g_s.strategy;
        p->nonBinary = g_s.nonBinary;
        /* 换窗口后的一小段时间里，向 IME 问到的还是**上一个窗口**的值（实测滞后
           200~280ms），此时返回的判定结果并不可信。调用方据此在"未定"期间不显示，
           免得先亮一个错颜色再消失（切窗口时的红点闪烁就是这么来的）。 */
        p->settling = g_s.initPending;
    }
    return state;
}

int ImeIsChineseMode(void) {
    return ImeIsChineseModeEx(NULL);
}

/* 前台键盘布局的主语言 ID：HKL 低 16 位是语言 ID，低字节是主语言
   （0x04 中文 / 0x09 英文 / 0x11 日文 / 0x12 韩文）。
   没有前台窗口返回 -1 —— 调用方据此沿用上次状态，别当成英文。 */
int ImeKeyboardLang(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return -1;
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    HKL hkl = GetKeyboardLayout(tid);
    return (int)(((DWORD)(UINT_PTR)hkl) & 0xFF);
}

/* 是否英文键盘布局：英文布局（0409/0809/0c09…）主语言都是 0x09 */
int ImeIsEnglishKeyboard(void) {
    return ImeKeyboardLang() == 0x09;
}
