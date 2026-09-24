#include "ime_indicator.h"
#include "wps.h"

#include <uiautomationclient.h>
#include <oleauto.h>
#include <oleacc.h>   /* MSAA：AccessibleObjectFromWindow + accLocation */

/* ================= 光标位置检测（多级策略） =================
   顺序（见 CaretProbeOnce 里的 TRY_CHANNEL 链）：
   0) WPS 系（拆到 wps.c：文字走 Word、演示走 PowerPoint 的对象模型 COM；
      它们的标准接口全不可见，演示还会报出屏幕原点上的假光标，所以必须
      抢在最前面）；
   1) GUI 线程 caret 矩形（GetGUIThreadInfo，经典 Win32 编辑器）；
   2) UIA TextPattern2::GetCaretRange（VS Code 等现代编辑器）；
   3) MSAA OBJID_CARET accLocation（Chromium 系的兜底）；
   4) UIA TextPattern::GetSelection（先收成插入点再取矩形）；
   5) IME 组合窗口 ImmGetCompositionWindow（候选框定位）。
   坐标一律换算为屏幕物理像素（进程已声明 DPI 感知）。
   这些调用都是**跨进程**的且没有超时参数 → 整条链跑在查询线程里（文件末尾）。 */

static IUIAutomation* g_uia = NULL;
/* worker 代际：重建 +1，旧代见之即退（声明在 ime_indicator.h —— wps.c 的
   WPS 通道也要看它：换代后必须丢掉上一代拿到的 COM 代理，见 wps.c 的
   WpsAttach）。 */
volatile LONG g_gen = 0;
/* 树遍历器：IUIAutomation 活着期间内容不变，取一次缓存住即可。原来每次探测
   取一对、用完 Release，66Hz 下就是每秒 132 次白做的 COM 调用。与 g_uia
   同生共死（换代重建时一起置 NULL，理由同 g_uia）。 */
static IUIAutomationTreeWalker* g_walkCV = NULL;
static IUIAutomationTreeWalker* g_walkRV = NULL;

static void EnsureUia(void) {
    if (g_uia) return;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* RPC_E_CHANGED_MODE 表示本线程已按别的模式初始化过 COM，照样能用 */
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&g_uia));
    if (g_uia) {
        if (!g_walkCV) g_uia->get_ControlViewWalker(&g_walkCV);
        if (!g_walkRV) g_uia->get_RawViewWalker(&g_walkRV);
    }
    if (!g_uia) {
        /* 创建失败必须留痕：否则"UIA 全通道静默失败 → 永远 miss"无从查起 */
        static ULONGLONG last = 0;
        ULONGLONG now = GetTickCount64();
        if (!last || now - last >= 5000) {
            last = now;
            DbgLog(L"uia: CoCreateInstance failed hr=0x%08lX (UIA unavailable)", (unsigned long)hr);
        }
    }
}

/* 通道失败原因日志：1 秒节流（"没有光标"是常态，不能刷屏）。
   之前 UIA/MSAA 失败全部静默 → 用户日志里只见 caret=miss src=none，
   看不出是哪条路、为什么死。 */
void LogChFail(const WCHAR* ch, const WCHAR* why, HRESULT hr) {
    static ULONGLONG last = 0;
    static WCHAR lastWhy[128] = L"";
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 1000) return;
    if (lastWhy[0] && wcscmp(lastWhy, why) == 0 && now - last < 5000) return;
    last = now;
    lstrcpynW(lastWhy, why, 128);
    DbgLog(L"ch fail: %s %s (hr=0x%08lX)", ch, why, (unsigned long)hr);
}

/* 文本范围的边界矩形 -> CaretPos（BoundingRectangles 是 double SAFEARRAY，
   每个矩形 4 个元素：左 上 宽 高；单位=屏幕坐标）。宽度写进 out->w（日志诊断用，
   "宽得像整行"的框就是漂移线索）。
   ★ 折叠插入点（光标）常读回**空数组**，必须先 ExpandToEnclosingUnit 才有矩形
   —— 实测 Obsidian 与 WindowsTerminal 都是 raw 为空、expand 后才有值，所以取
   矩形一律走下面的 RectViaRange，别在调用点自己拼顺序。 */
static int UiRect(CaretPos* out, IUIAutomationTextRange* range, const WCHAR* ch) {
    SAFEARRAY* arr = NULL;
    HRESULT hr = range->GetBoundingRectangles(&arr);
    if (FAILED(hr) || !arr) {
        LogChFail(ch, L"GetBoundingRectangles failed", hr);
        return 0;
    }
    long lb = 0, ub = -1;
    SafeArrayGetLBound(arr, 1, &lb);
    SafeArrayGetUBound(arr, 1, &ub);
    long n = ub - lb + 1;
    int ok = 0;
    if (n >= 4 && (n % 4) == 0) {   /* 每矩形 4 个元素，长度不是 4 的倍数即数据异常 */
        double* d = NULL;
        if (SUCCEEDED(SafeArrayAccessData(arr, (void**)&d)) && d) {
            out->x = (int)d[0];
            out->y = (int)d[1];
            out->w = (int)d[2];
            out->h = (int)d[3];
            ok = 1;
            SafeArrayUnaccessData(arr);
        }
    }
    SafeArrayDestroy(arr);
    if (!ok) {
        /* 空数组/元素数异常：某些 provider 就是给不出矩形，留痕便于区分 */
        static ULONGLONG last = 0;
        ULONGLONG now = GetTickCount64();
        if (!last || now - last >= 2000) {
            last = now;
            DbgLog(L"%s: BoundingRectangles empty (n=%ld)", ch, n);
        }
    }
    return ok;
}

/* UIA 文本范围 -> 插入点坐标。**只接受"正常字符格"**：1 < w ≤ 1.5×h
   （CJK 字格≈1.0×高、拉丁≈0.5×高，行距算进高里只会更宽裕）。
   其余形态一律不信、返回 0 让链路落到 MSAA —— OBJID_CARET 的 1px 光标条在
   Chromium 系（Obsidian / WorkBuddy / GitHub Desktop）实测全部精准。退化范围
   （插入点）自己的矩形有三种坏形态，全都实测过：
     a) 空数组 —— Obsidian 正文与标题、WindowsTerminal；
     b) 1px 细条但位置在**行首** —— 光标在标题行末尾时，真光标在 826，
        UIA 却给 (677,1124,1,38)（CodeMirror 装饰 span 把"字符"映射到了行首），
        直接采信就是"光标在后、点在前"；
     c) 宽块（>1.5×h）—— 装饰 span / 整行的框。
   早先的"先读 raw"、"按行扩展取右缘"分别踩过 c 和 b，都已废弃。
   细条被拒后由 MSAA 兜底，所以"可编辑性闸门"必须留在本通道（见调用处）——
   它挡住"焦点不在文本里时 MSAA 报上一次光标"的老问题。
   若将来遇到"MSAA 无 caret 对象 + UIA 只有细条"的程序（行尾圆点消失），
   再考虑把细条当最后兜底 —— 靠日志的 w/d 取证，别拍脑袋。 */
static int RectViaRange(CaretPos* out, IUIAutomationTextRange* range, const WCHAR* ch) {
    if (FAILED(range->ExpandToEnclosingUnit(TextUnit_Character))) return 0;
    return UiRect(out, range, ch) && out->w > 1 && out->w <= (out->h * 3) / 2;
}

/* 持有该文本模式的元素是不是**可编辑文本框**。
   为什么要它：Chromium/Electron 在焦点控件上报 isActive=FALSE 却给得出正确矩形
   （Obsidian 实测：d0 = 可编辑 Edit、isActive=0、expand 后矩形有效），而同一个
   引擎在"焦点不在文本里"时给出的文档级范围是**上一次的光标** —— 那正是当初
   "点钉在屏幕上"的根因，当时拿 isActive 当判据，代价是把整个 Electron 生态一起
   判死。所以 inactive 的范围只在持有者可编辑时才采信。 */
static int OwnerIsEditable(IUIAutomationElement* el) {
    int ct = 0;
    if (FAILED(el->get_CurrentControlType(&ct))) return 0;
    return ct == UIA_EditControlTypeId;
}

/* 焦点是否在 Win32 弹出菜单（上下文菜单，类名 #32768）上。
   菜单永远没有文本光标，但原来每 15ms 仍会对菜单做 GetFocusedElement +
   两条 UIA 通道各爬 24 层祖先 + MSAA caret 查询 —— 全是对空气输出，右键
   菜单打开期间内存/CPU 因此持续上涨。这里直接短路成"没有光标"。
   （Chromium/传统 Win32 程序的右键菜单都是 #32768；WinUI 的 XAML 弹窗
   类名不同，由下面的负结果缓存兜住。） */
static int FocusIsContextMenu(void) {
    HWND fw = ImeFocusedWindow();
    if (!fw) return 0;
    WCHAR cls[16];
    if (!GetClassNameW(fw, cls, 16)) return 0;
    return wcscmp(cls, L"#32768") == 0 ? 1 : 0;
}

/* ---------- 跨进程 DPI 换算（guiinfo 通道专用） ----------
   rcCaret 是**对方窗口**的客户端坐标，ClientToScreen 只是把它接在窗口原点后面，
   而那个原点是**我们**看到的物理像素 —— 两者坐标空间未必一致：对方若不是
   Per-Monitor 感知（MMC/任务计划程序这类 unaware 程序在 175% 下 logical=96、
   physical=168），偏移量会被低估一个固定比值，表现为**光标越往右、打的字越多
   圆点偏得越远**（用户实测）。换算：物理 = 原点 + 偏移 × 显示器DPI / 窗口DPI。
   本进程是 Per-Monitor V2，所以比值恒为 1 时完全不改动原行为。 */
typedef UINT (WINAPI* PFN_GETDPIFORWINDOW)(HWND);
typedef HRESULT (WINAPI* PFN_GETDPIFORMONITOR)(HMONITOR, int, UINT*, UINT*);

UINT WindowDpi(HWND hwnd) {
    static PFN_GETDPIFORWINDOW fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        fn = (PFN_GETDPIFORWINDOW)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                 "GetDpiForWindow");
    }
    if (!fn) return 0;      /* 取不到就不换算（宁可沿用旧行为） */
    return fn(hwnd);
}

UINT MonitorDpi(HWND hwnd) {
    static PFN_GETDPIFORMONITOR fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE sh = LoadLibraryW(L"shcore.dll");
        if (sh) fn = (PFN_GETDPIFORMONITOR)GetProcAddress(sh, "GetDpiForMonitor");
    }
    UINT dx = 0, dy = 0;
    if (fn) {
        HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (hm && SUCCEEDED(fn(hm, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dy)
            return dy;
    }
    HDC hdc = GetDC(NULL);                 /* 兜底：主显示器 DPI */
    UINT d = hdc ? (UINT)GetDeviceCaps(hdc, LOGPIXELSY) : 0;
    if (hdc) ReleaseDC(NULL, hdc);
    return d ? d : 96;
}

/* 方法0：WPS 系（文字/演示/表格的 COM 通道）拆在 src/wps.c —— 那边自带
   全部实测注释与三条通道的实现，这里只按顺序调用（见下方 TRY_CHANNEL）。 */


/* 方法1：前台线程的 caret 矩形（GetGUIThreadInfo 可跨进程读） */
static int ViaGuiInfo(CaretPos* out) {
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    GUITHREADINFO gi;
    ZeroMemory(&gi, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndCaret) {
        int x = gi.rcCaret.left, y = gi.rcCaret.top;
        int h = gi.rcCaret.bottom - gi.rcCaret.top;
        int w = gi.rcCaret.right - gi.rcCaret.left;
        POINT org = { 0, 0 }, pt = { x, y };
        if (ClientToScreen(gi.hwndCaret, &org) && ClientToScreen(gi.hwndCaret, &pt)) {
            UINT dpiW = WindowDpi(gi.hwndCaret), dpiM = MonitorDpi(gi.hwndCaret);
            if (dpiW && dpiM && dpiW != dpiM) {
                /* 实测（本机 175%）：unaware 进程里光标在 20 个字符后，
                   rcCaret.left=160（96 逻辑像素），真实物理位置 = 窗口原点 609 + 280；
                   旧算法直接把 160 当物理偏移 → 偏 121px，且随字符数线性放大。 */
                int ox = pt.x - org.x, oy = pt.y - org.y;
                pt.x = org.x + MulDiv(ox, (int)dpiM, (int)dpiW);
                pt.y = org.y + MulDiv(oy, (int)dpiM, (int)dpiW);
                h = MulDiv(h, (int)dpiM, (int)dpiW);
                w = MulDiv(w, (int)dpiM, (int)dpiW);
                /* 只在比值变化时记一行（同一个程序里会连着几百轮） */
                static UINT lastW = 0, lastM = 0;
                if (lastW != dpiW || lastM != dpiM) {
                    lastW = dpiW; lastM = dpiM;
                    DbgLog(L"guiinfo: DPI %lu -> %lu, caret offset scaled",
                           (unsigned long)dpiW, (unsigned long)dpiM);
                }
            }
            out->x = pt.x; out->y = pt.y; out->h = h; out->w = w;
            return 1;
        }
    }
    return 0;
}

/* 前置声明：ViaMsaa 在 CaretPlausible 定义之前用到它 */
static int CaretPlausible(const CaretPos* cp);

/* ★ 全屏判定（判定方式取自 InputTip `utils.ahk:isFullscreen`）：
   无标题栏 + 窗口尺寸 >= 所在显示器的 98%。
   为什么要它：全屏看视频时，MSAA 的 OBJID_CARET 仍会报出**上一次出现过的
   光标位置**（日志里表现为坐标十几秒一动不动），圆点就钉在画面上挡视线。
   本进程是 Per-Monitor V2，GetWindowRect 与 MONITORINFO 同为物理像素，可直接比。 */
int CaretIsForegroundFullscreen(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    if (GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) return 0;
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return 0;
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    if (ww <= 0 || wh <= 0) return 0;
    HMONITOR hm = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (!hm) return 0;
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return 0;
    int mw = mi.rcMonitor.right - mi.rcMonitor.left;
    int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return (ww * 100 >= mw * 98 && wh * 100 >= mh * 98) ? 1 : 0;
}

/* 方法2：MSAA OBJID_CARET accLocation。
   InputTip 依赖它得到「真实光标」，对 Chromium 等效果比 GetSelection 可靠。
   注意用的是**焦点控件**而不是前台窗口（InputTip 同款）：OBJID_CARET 属于
   焦点控件所在的线程，拿顶层窗口会读到过期/别人的 caret。
   返回的是绝对屏幕坐标（accLocation 约定）；**很多控件在没有 caret 时返回
   (0,0,0,0)** —— 那是假数据，这里直接丢（合理性过滤拦不住它：原点就落在窗口内，
   放过去的结果是圆点画到屏幕左上角，实测 explorer / M365Copilot 都会这样）。 */
static int ViaMsaa(CaretPos* out) {
    HWND hwnd = ImeFocusedWindow();
    if (!hwnd) return 0;
    IAccessible* acc = NULL;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, (DWORD)(LONG)OBJID_CARET, IID_IAccessible, (void**)&acc);
    if (FAILED(hr) || !acc) {
        LogChFail(L"msaa", L"AccessibleObjectFromWindow failed", hr);
        return 0;
    }
    long x = 0, y = 0, w = 0, h = 0;
    VARIANT child;
    child.vt = VT_I4;
    child.lVal = 0;                     /* CHILDID_SELF */
    hr = acc->accLocation(&x, &y, &w, &h, child);
    acc->Release();
    if (FAILED(hr)) {
        LogChFail(L"msaa", L"accLocation failed", hr);
        return 0;
    }
    if (x == 0 && y == 0 && h <= 0) {   /* (0,0,0,0)：没有 caret 对象的假数据 */
        LogChFail(L"msaa", L"accLocation returned (0,0,0,0)", 0);
        return 0;
    }
    if (w <= 0) {   /* 零宽"光标"：真实插入条至少 1px 宽。QQ（CEF）实测在焦点
                       不在输入框时 OBJID_CARET 报 (x,y,0,h) 的残留假数据 ——
                       且 QQ 的焦点元素没有 TextPattern，UIA 闸门拦不到它，
                       不在这里拒掉，圆点就钉在没有输入框的位置。 */
        LogChFail(L"msaa", L"accLocation returned zero-width caret", 0);
        return 0;
    }
    out->x = (int)x; out->y = (int)y; out->w = (int)w; out->h = (int)h;   /* ★ w 之前漏赋值，日志里 msaa 的 w 恒 0 */
    return 1;   /* 其余合理性由 CaretProbeOnce 的 TRY_CHANNEL 统一过滤（这样 reject 日志才打得出） */
}

/* ---------- 负结果缓存：这条焦点链上没有目标 pattern ----------
   上下文菜单 / 资源管理器 / 桌面这类"永远没有文本光标"的焦点，原来每 15ms
   都要把祖先链爬满 24 层（两条 UIA 通道 = 每秒 3000+ 次跨进程调用），每次
   右键菜单打开期间内存与 CPU 都持续上涨、停手也不回落（UIA 客户端为每个
   碰过的元素建封送/缓存结构，不主动还给系统）。现在：同一焦点元素判定过
   "没有 pattern"后 500ms 内直接跳过；焦点一换 RuntimeId 就不同，立即重查，
   不影响正常编辑器。查到 pattern 时清缓存。 */
#define NOPAT_TTL_MS 500
static unsigned long long g_noPatKey[2] = {0, 0}; /* [0]=TextPattern2 [1]=TextPattern */
static ULONGLONG          g_noPatTick[2] = {0, 0};

/* 焦点元素的 RuntimeId 哈希（FNV-1a）。取不到/太长都返回 ok=0，调用方
   当作"没缓存"处理 —— 顶多多爬几层，不会错。 */
static unsigned long long RidHash(IUIAutomationElement* e, int* ok) {
    *ok = 0;
    SAFEARRAY* psa = NULL;
    if (FAILED(e->GetRuntimeId(&psa)) || !psa) return 0;
    long lb = 0, ub = -1;
    SafeArrayGetLBound(psa, 1, &lb);
    SafeArrayGetUBound(psa, 1, &ub);
    unsigned long long h = 1469598103934665603ull;
    if (ub >= lb && (ub - lb) < 16) {
        long* v = NULL;
        if (SUCCEEDED(SafeArrayAccessData(psa, (void**)&v)) && v) {
            for (long i = lb; i <= ub; i++) {
                h ^= (unsigned long long)(unsigned long)v[i];
                h *= 1099511628211ull;
            }
            SafeArrayUnaccessData(psa);
            *ok = 1;
        }
    }
    SafeArrayDestroy(psa);
    return h;
}

/* 自 GetFocusedElement 向上（NVDA 式）找最近一个实现了指定文本模式的元素：
   Chromium/WinUI 的焦点元素常是深处的子节点，文本模式与光标属于其祖先——
   只在焦点元素上查 GetCaretRange/GetSelection 会拿到别的元素/过期的选区（漂移根因）。
   depthOut 非空时回填 pattern 所在层数（0=焦点元素自身；诊断"模式来自祖先文档"用）。 */
static IUIAutomationElement* UiaFindPattern(IUIAutomationElement* start, BOOL want2, int* depthOut) {
    int slot = want2 ? 0 : 1;
    if (depthOut) *depthOut = -1;
    IUIAutomationElement* e = start;
    if (e) e->AddRef();
    /* 负结果缓存命中：同一焦点刚判定过"没有目标 pattern"，别再爬 24 层 */
    if (e) {
        int okh = 0;
        unsigned long long k = RidHash(e, &okh);
        if (okh && k && k == g_noPatKey[slot] &&
            GetTickCount64() - g_noPatTick[slot] < NOPAT_TTL_MS) {
            e->Release();
            return NULL;
        }
    }
    /* ControlView walker：向上导航（无则退回 RawView；两种都要不到父级就停）。
       两个 walker 在 EnsureUia 里随 g_uia 缓存，这里只借用，不 Release。 */
    IUIAutomationTreeWalker* walker = g_walkCV;
    IUIAutomationTreeWalker* rawWalker = g_walkRV;

    int found = 0;
    for (int depth = 0; e && depth < 24; depth++) {
        IUnknown* pat = NULL;
        if (SUCCEEDED(e->GetCurrentPattern(want2 ? UIA_TextPattern2Id : UIA_TextPatternId, &pat)) && pat) {
            pat->Release();
            if (depth > 0)
                DbgLog(L"uia: pattern found at depth %d (want2=%d)", depth, want2);
            g_noPatKey[slot] = 0;      /* 有 pattern：负缓存作废 */
            if (depthOut) *depthOut = depth;
            return e;                 /* 找到了，调用方持有引用 */
        }
        if (pat) pat->Release();
        IUIAutomationElement* parent = NULL;
        HRESULT hr = E_FAIL;
        if (walker) hr = walker->GetParentElement(e, &parent);
        if (FAILED(hr) || !parent) {
            if (rawWalker) hr = rawWalker->GetParentElement(e, &parent);
        }
        if (FAILED(hr) || !parent) { e->Release(); e = NULL; break; }
        e->Release();
        e = parent;
        found = depth + 1;
    }
    if (e) e->Release();
    /* 爬到顶都没有：记入负缓存（同一焦点 500ms 内不再爬）并留痕 */
    if (start) {
        int okh = 0;
        unsigned long long k = RidHash(start, &okh);
        if (okh && k) { g_noPatKey[slot] = k; g_noPatTick[slot] = GetTickCount64(); }
    }
    {
        static ULONGLONG last = 0;
        ULONGLONG now = GetTickCount64();
        if (!last || now - last >= 2000) {
            last = now;
            DbgLog(L"uia: no TextPattern%s ancestor (want2=%d, walked %d levels)",
                   want2 ? L"2" : L"", want2, found);
        }
    }
    return NULL;
}

/* 方法2：UIA TextPattern2::GetCaretRange（向上找文本节点） */
static int ViaUiaCaretRange(CaretPos* out) {
    if (!g_uia) return 0;
    IUIAutomationElement* focus = NULL;
    HRESULT hr = g_uia->GetFocusedElement(&focus);
    if (FAILED(hr) || !focus) {
        /* 防御：个别失败路径会回填非空指针，覆盖前必须放掉
           （CapsEnhance comptr.h 的规矩：包起来的 = 我们负责放的） */
        if (focus) { focus->Release(); focus = NULL; }
        /* 兜底：按焦点窗口句柄取元素。GetFocusedElement 依赖对端 UIA provider
           的焦点上报，某些应用（权限差异/沙箱/provider 忙）会拿不到焦点元素，
           但窗口句柄仍然有效 —— Chromium 系应用常见。 */
        HWND fw = ImeFocusedWindow();
        if (fw && SUCCEEDED(g_uia->ElementFromHandle(fw, &focus)) && focus) {
            LogChFail(L"uia_caret", L"GetFocusedElement failed -> fallback hwnd", hr);
        } else {
            if (focus) { focus->Release(); focus = NULL; }
            LogChFail(L"uia_caret", L"GetFocusedElement & hwnd both failed", hr);
            return 0;
        }
    }
    int depth = -1;
    IUIAutomationElement* el = UiaFindPattern(focus, TRUE, &depth);
    if (!el) { focus->Release(); return 0; }
    out->depth = depth;
    int ok = 0;
    IUnknown* pat = NULL;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_TextPattern2Id, &pat)) && pat) {
        IUIAutomationTextPattern2* tp2 = NULL;
        if (SUCCEEDED(pat->QueryInterface(IID_PPV_ARGS(&tp2)))) {
            BOOL active = FALSE;
            IUIAutomationTextRange* range = NULL;
            if (SUCCEEDED(tp2->GetCaretRange(&active, &range)) && range) {
                /* ★ isActive=FALSE **不等于没有光标**：MS 文档说它只表示"含光标的
                   文本控件不持有键盘焦点"。Chromium/Electron 系（Obsidian、VS Code…）
                   常年在焦点控件上报 FALSE，却给得出正确矩形 —— 照旧取矩形即可。
                   判据因此从"isActive 说什么"改成"**焦点在不在可编辑控件上**"
                   （见 OwnerIsEditable 注释）：只有"未持有焦点 + 既不是焦点控件
                   也不是持有者可编辑"才认定没有光标（-1，不再试后面的通道），
                   那正是页面/文档级陈旧光标出现的地方 —— 当初把点钉在屏幕上的根因。
                   （两个元素都要看：文本模式有时挂在焦点控件的祖先文档上。） */
                int editable = active ? 0
                                      : (OwnerIsEditable(el) ||
                                         (focus && OwnerIsEditable(focus)));
                /* ★ 焦点元素离屏（IsOffscreen）时 caret 必然不可见：QQ 实测，
                   从编辑器切到 QQ 时 DOM 焦点残留在上次会话的输入框（用户眼中
                   "没有输入框"），闸门见 Edit 就放行，MSAA 紧接着报出残留坐标
                   —— 圆点钉在没有输入框的位置。离屏 ⇒ 不显示，也顺带覆盖
                   最小化窗口的场景。 */
                BOOL focusOff = FALSE;
                if (focus && FAILED(focus->get_CurrentIsOffscreen(&focusOff)))
                    focusOff = FALSE;
                if (focusOff) {
                    ok = -1;
                } else if (active || editable) {
                    ok = RectViaRange(out, range, L"uia_caret") ? 1 : 0;
                } else {
                    ok = -1;   /* 焦点不在文本里 */
                }
                range->Release();
            }
            tp2->Release();
        }
        pat->Release();
    }
    focus->Release();
    el->Release();
    return ok;
}

/* 方法3：UIA TextPattern::GetSelection（向上找文本节点，Chromium 系） */
static int ViaUiaSelection(CaretPos* out) {
    if (!g_uia) return 0;
    IUIAutomationElement* focus = NULL;
    HRESULT hr = g_uia->GetFocusedElement(&focus);
    if (FAILED(hr) || !focus) {
        if (focus) { focus->Release(); focus = NULL; }   /* 失败仍回填非空的防御 */
        HWND fw = ImeFocusedWindow();
        if (fw && SUCCEEDED(g_uia->ElementFromHandle(fw, &focus)) && focus) {
            LogChFail(L"uia_sel", L"GetFocusedElement failed -> fallback hwnd", hr);
        } else {
            if (focus) { focus->Release(); focus = NULL; }
            return 0;   /* uia_caret 通道已记过失败原因，不再重复刷 */
        }
    }
    int depth = -1;
    IUIAutomationElement* el = UiaFindPattern(focus, FALSE, &depth);
    focus->Release();
    if (!el) return 0;
    out->depth = depth;
    int ok = 0;
    IUnknown* pat = NULL;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_TextPatternId, &pat)) && pat) {
        IUIAutomationTextPattern* tp = NULL;
        if (SUCCEEDED(pat->QueryInterface(IID_PPV_ARGS(&tp)))) {
            IUIAutomationTextRangeArray* sel = NULL;
            if (SUCCEEDED(tp->GetSelection(&sel)) && sel) {
                int n = 0;
                if (SUCCEEDED(sel->get_Length(&n)) && n >= 1) {
                    IUIAutomationTextRange* range = NULL;
                    /* 取**最后一个**选区。★ 只认**折叠**的（Start==End=真插入点）：
                       非折叠说明用户在拖选 —— 无光标的程序（QQ 聊天窗实测）这时
                       GetSelection 给的是整个选区，折叠到 End 再取字符框就成了
                       "选区末字的字框"，圆点钉在选区末尾。选中期间光标本来看不见，
                       直接放弃本轮让圆点收起。端点比较失败（provider 不支持）时
                       退回旧行为，避免误杀。 */
                    if (SUCCEEDED(sel->GetElement(n - 1, &range)) && range) {
                        int cmp = 0;
                        if (FAILED(range->CompareEndpoints(
                                TextPatternRangeEndpoint_Start, range,
                                TextPatternRangeEndpoint_End, &cmp)) || cmp == 0) {
                            range->MoveEndpointByRange(TextPatternRangeEndpoint_Start,
                                                       range, TextPatternRangeEndpoint_End);
                            ok = RectViaRange(out, range, L"uia_sel");
                        }
                        range->Release();
                    }
                }
                sel->Release();
            }
            tp->Release();
        }
        pat->Release();
    }
    el->Release();
    return ok;
}

/* 方法4：IME 组合窗口定位（候选框）。**仅在存在组合串时信任**：
   ImmGetCompositionWindow 在有组合（候选框有字）时返回光标附近的组合框位置，
   无组合时却返回一个残留/远离的坐标——这正是"候选框空时点漂到远处"的根因。
   无组合串（GCS_COMPSTR 返回 0）直接放弃该通道，让上层走 GUI/UIA 或隐藏。 */
static int ViaIme(CaretPos* out) {
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    HIMC hImc = ImmGetContext(fg);
    if (!hImc) return 0;
    int ok = 0;
    int bytes = (int)ImmGetCompositionStringW(hImc, GCS_COMPSTR, NULL, 0);
    if (bytes > 0) {
        COMPOSITIONFORM cf;
        ZeroMemory(&cf, sizeof(cf));
        if (ImmGetCompositionWindow(hImc, &cf) && (cf.dwStyle & CFS_POINT)) {
            POINT org = { 0, 0 }, pt = { cf.ptCurrentPos.x, cf.ptCurrentPos.y };
            if (ClientToScreen(fg, &org) && ClientToScreen(fg, &pt)) {
                /* ptCurrentPos 同样是**对方**客户端坐标 → 与 guiinfo 同一套 DPI 换算 */
                UINT dpiW = WindowDpi(fg), dpiM = MonitorDpi(fg);
                if (dpiW && dpiM && dpiW != dpiM) {
                    pt.x = org.x + MulDiv(pt.x - org.x, (int)dpiM, (int)dpiW);
                    pt.y = org.y + MulDiv(pt.y - org.y, (int)dpiM, (int)dpiW);
                }
                out->x = pt.x; out->y = pt.y; out->h = 20;
                ok = 1;
            }
        }
    }
    ImmReleaseContext(fg, hImc);
    return ok;
}

/* 合理性过滤：真光标必定落在前台窗口附近。
   某些通道（尤其 Chromium 的 UIA Selection）会返回与光标无关的坐标
   （焦点元素不对 / 返回的是选区外框）→ 点会跳到"离光标很远"的地方。
   这里要求坐标落在前台窗口屏幕矩形（各向外放一个窗口宽高）之内，
   超出即丢弃，让上层走下一通道或直接隐藏，而不是把点摆到远处。 */
static int CaretPlausible(const CaretPos* cp) {
    HWND fg = GetForegroundWindow();
    if (!fg) return 1;
    RECT r;
    if (!GetWindowRect(fg, &r)) return 1;
    int mw = r.right - r.left;
    int mh = r.bottom - r.top;
    if (mw < 1) mw = GetSystemMetrics(SM_CXSCREEN);
    if (mh < 1) mh = GetSystemMetrics(SM_CYSCREEN);
    return cp->x >= (long)(r.left - mw) && cp->x <= (long)(r.right + mw) &&
           cp->y >= (long)(r.top - mh)  && cp->y <= (long)(r.bottom + mh);
}

static const WCHAR* CaretSrcName(CaretSource s) {
    switch (s) {
    case CARET_GUIINFO:   return L"guiinfo";
    case CARET_MSAA:      return L"msaa";
    case CARET_UIA_CARET: return L"uia_caret";
    case CARET_UIA_SEL:   return L"uia_sel";
    case CARET_IME:       return L"ime";
    case CARET_WPS:       return L"wps";
    case CARET_WPP:       return L"wpp";
    case CARET_ET:        return L"et";
    default:              return L"none";
    }
}

/* 追踪循环每 15ms 跑一次，"没有光标"是常态 → 这类日志必须节流，否则刷屏 */
static void LogNoCaret(const WCHAR* why) {
    static ULONGLONG last = 0;
    static WCHAR lastWhy[64] = L"";
    ULONGLONG now = GetTickCount64();
    if (last && now - last < 1000 && wcscmp(lastWhy, why) == 0) return;
    last = now;
    lstrcpynW(lastWhy, why, 64);
    DbgLog(L"no caret: %s", why);
}

/* 返回 1=拿到坐标，-1=该通道明确报告"当前没有光标"（不再试后面的通道）。
   why 是判"没有光标"的原因，随日志打出来（目前只有 uia_caret 会走这条）。 */
#define TRY_CHANNEL(detector, src, why)                                       \
    do {                                                                      \
        int r = (detector)(out);                                              \
        if (r < 0) {                                                          \
            LogNoCaret(why);                                                  \
            goto done;                                                        \
        }                                                                     \
        if (r > 0) {                                                          \
            if (CaretPlausible(out)) { out->source = (src); goto done; }      \
            DbgLog(L"reject %s caret=(%d,%d,w=%d,h=%d,d=%d)",                 \
                   CaretSrcName(src), out->x, out->y, out->w, out->h, out->depth); \
        }                                                                     \
    } while (0)

/* ---------- 单次探测（**会阻塞**：内部是跨进程 UIA/MSAA 调用） ----------
   只能在查询线程里跑，别在检测线程直接调 —— 这就是超时机制存在的理由。 */
static int CaretProbeOnce(CaretPos* out) {
    EnsureUia();
    out->found = 0;
    out->source = CARET_NONE;
    out->w = 0;
    out->depth = -1;
    /* 全屏（看视频/演示）时一律不显示：此时 MSAA 会报出上一次的陈旧光标位置，
       圆点会钉在画面上挡视线。 */
    if (g_cfg.hideFullscreen && CaretIsForegroundFullscreen()) {
        LogNoCaret(L"fullscreen foreground");
        goto done;
    }
    /* 焦点在上下文菜单上：整条链路短路（见 FocusIsContextMenu 注释） */
    if (FocusIsContextMenu()) {
        LogNoCaret(L"context menu focused");
        goto done;
    }
    /* WPS 系排在最前：它们的 guiinfo/UIA/MSAA/IMM 四条路全不通（见文件头），
       后面那些跑一遍纯属白付跨进程开销，而这条路一次就够。演示那条还必须抢在
       guiinfo 之前 —— 它的 guiinfo 报的是屏幕原点上的 1×1 假光标。 */
    TRY_CHANNEL(WpsCaretText, CARET_WPS, L"wps: no caret rect");
    TRY_CHANNEL(WpsCaretShow, CARET_WPP, L"wpp: no caret rect");
    TRY_CHANNEL(WpsCaretGrid, CARET_ET, L"et: no caret rect");
    TRY_CHANNEL(ViaGuiInfo, CARET_GUIINFO, L"guiinfo: no caret hwnd");
    /* UIA caret 排在 MSAA 之前，职责是**把关**：判定"焦点在不在可编辑控件"
       （不在 → -1，整条链停，MSAA 的陈旧坐标就没机会出来），并只在自己能给
       出"正常字符格"时提供坐标；细条/空/宽块一律返回 0 落到 MSAA 的 1px
       光标条 —— Chromium 系实测 MSAA 比 UIA 的文本范围准。 */
    TRY_CHANNEL(ViaUiaCaretRange, CARET_UIA_CARET,
                L"uia_caret: inactive caret outside an editable control");
    TRY_CHANNEL(ViaMsaa, CARET_MSAA, L"msaa: no caret object");
    TRY_CHANNEL(ViaUiaSelection, CARET_UIA_SEL, L"uia_sel: no caret rect");
    TRY_CHANNEL(ViaIme, CARET_IME, L"ime: no composition window");
done:
    if (out->source != CARET_NONE) { out->found = 1; return 1; }
    out->x = out->y = out->h = 0;
    return 0;
}

/* ================= 查询线程 + 超时 =================
   UIA/MSAA 是跨进程调用，**没有超时参数**：对方线程不泵消息，调用就一直悬着
   （实测前台浏览器卡住时检测线程冻 6 秒，日志心跳直接断档）。解决办法是把
   查询隔离到独立线程，调用方按时间等事件，超时就放弃本轮。

   请求/应答协议（不用互斥量，靠"序号 + 事件"，杜绝互相等待）：
     reqSeq    检测线程递增后 SetEvent(reqEv)，表示"又有一轮要查"
     ackSeq    查询线程查完写回它对应的序号，SetEvent(ackEv)
   检测线程拿到 ackSeq==自己的 reqSeq 才采纳结果；否则视为超时/过期，
   直接返回失败（沿用上一轮显示状态）。过期结果也不会被误用，因为序号对不上。

   ★ worker 卡死自愈：某些程序被挂起（SuspendThread/死锁）时，跨进程调用
   **永不返回** —— worker 卡死一次，之后每轮查询都超时，圆点从此不再显示
   （用户症状："怎么切换中英都不显示，重启才好"）。对策：连续超时
   CARET_RESTART_AFTER 次就**换一代 worker**（新事件对 + 新线程 + 重新
   CoCreateInstance）。旧线程若哪天苏醒，发现代际不符就自己退出（不用
   TerminateThread —— 它持有 COM 状态，硬杀有死锁风险；泄漏一个挂死线程
   和一对事件句柄，低频可接受）。每个 worker 用**自己捕获的**事件句柄，
   避免重建后旧线程误等新事件。 */
#define CARET_DEFAULT_TIMEOUT_MS 150
#define CARET_RESTART_AFTER 20          /* 连续超时这么多次（约3秒）就重建 worker */

static HANDLE          g_reqEv = NULL;    /* 检测线程 -> 查询线程：有新请求 */
static HANDLE          g_ackEv = NULL;    /* 查询线程 -> 检测线程：结果就绪 */
static HANDLE          g_workerTh = NULL;
static volatile LONG   g_reqSeq = 0;
static volatile LONG   g_ackSeq = 0;
static int             g_timeoutMs = CARET_DEFAULT_TIMEOUT_MS;
static CaretPos        g_result;          /* 仅查询线程写，靠序号/事件保证读时已写完 */

static DWORD WINAPI CaretWorkerThread(LPVOID param) {
    LONG mygen = (LONG)(INT_PTR)param;
    HANDLE myReq = g_reqEv, myAck = g_ackEv;   /* 捕获本代的事件对（重建后全局会换新） */
    /* 线程命名：调试器/诊断工具里区分 worker 用 */
    if (GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))
        SetThreadDescription(GetCurrentThread(), L"caret-worker");
    /* 查询线程自己初始化 COM（UIA 客户端必须 MTA）—— 不能沿用检测线程的
       apartment：CoInitializeEx 是按线程计的。 */
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;

    LONG served = 0;
    for (;;) {
        DWORD w = WaitForSingleObject(myReq, 1000);
        if (g_gen != mygen) break;                   /* 换代了：自行退出 */
        if (w != WAIT_OBJECT_0) continue;            /* 超时只是醒来看看 stop */
        LONG seq = InterlockedCompareExchange(&g_reqSeq, 0, 0);
        if (seq == served) continue;                 /* 已经答过这一步 */
        served = seq;
        CaretPos r;
        ZeroMemory(&r, sizeof(r));
        CaretProbeOnce(&r);                          /* 可能永久卡死（自愈机制兜底） */
        g_result = r;                                /* 先写数据，再放事件 */
        InterlockedExchange(&g_ackSeq, served);
        SetEvent(myAck);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    CloseHandle(myReq);                              /* 本代的事件对只有自己在用 */
    CloseHandle(myAck);
    return 0;
}

/* 事件对 + 线程 + 序列的整体（重）建。首次启动与卡死自愈共用。 */
static int CaretSpawnWorker(void) {
    HANDLE re = CreateEventW(NULL, FALSE, FALSE, NULL);   /* 自动重置：一次请求一次唤醒 */
    HANDLE ae = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!re || !ae) { if (re) CloseHandle(re); if (ae) CloseHandle(ae); return 0; }
    g_reqEv = re; g_ackEv = ae;
    InterlockedExchange(&g_reqSeq, 0);
    InterlockedExchange(&g_ackSeq, 0);
    HANDLE th = CreateThread(NULL, 0, CaretWorkerThread,
                             (LPVOID)(INT_PTR)g_gen, 0, NULL);
    if (!th) return 0;
    g_workerTh = th;
    return 1;
}

void CaretWorkerStart(int timeoutMs) {
    if (g_workerTh) return;
    g_timeoutMs = (timeoutMs >= 20 && timeoutMs <= 5000) ? timeoutMs : CARET_DEFAULT_TIMEOUT_MS;
    CaretSpawnWorker();
}

void CaretWorkerStop(void) {
    /* 在检测线程（已退出循环）里调，不涉及并发 */
    if (!g_workerTh) return;
    InterlockedIncrement(&g_gen);            /* 旧线程最多 1 秒内自行退出 */
    /* 旧线程可能正卡在一次跨进程调用里（无限期），句柄 detach 掉 ——
       进程马上要结束了，为它无限等待不值得。 */
    CloseHandle(g_workerTh);
    g_workerTh = NULL;
    /* 事件对归 worker 所有（它退出时关），这里不重复关 */
}

/* 带超时的光标查询：把活儿交给查询线程，最多等 g_timeoutMs。
   返回 1=拿到本轮的坐标（序号对得上保证不是过期结果）。
   返回 0 时看 *timeoutOut：1 = 本轮等超时了（前台程序卡住），0 = 正常地没有光标。
   两者必须分开 —— 前者沿用上一轮显示，后者才收起圆点。

   ★ 必须**循环收应答**：worker 一次 probe 偶尔超过轮询间隔（跨进程调用 200ms+
   很常见）时，它会先 ack 上一轮请求 —— 那个"旧 ack"会立刻唤醒正在等本轮的
   检测线程。若把旧 ack 当失败返回，之后每轮都会被 worker 的旧 ack 唤醒、
   每轮都 miss，而 wait 又总能被唤醒 → 连续超时计数永远不涨 → 自愈永不触发
   → 圆点永久消失（重启才好的那次故障就是它）。所以旧 ack 只能忽略并继续等，
   worker 处理完堆积请求后自然会 ack 到本轮序号。 */
int CaretGetPosEx(CaretPos* out, int* timeoutOut) {
    CaretPos local;
    if (!out) out = &local;
    out->found = 0;
    out->source = CARET_NONE;
    out->x = out->y = out->h = 0;
    out->w = 0;
    out->depth = -1;
    if (timeoutOut) *timeoutOut = 0;
    if (!g_workerTh) CaretSpawnWorker();       /* 上次重建失败：本轮再试 */
    if (!g_workerTh) return 0;

    static int consecTimeouts = 0;             /* 连续超时计数（检测线程独占，无需同步） */

    LONG seq = InterlockedIncrement(&g_reqSeq);
    SetEvent(g_reqEv);
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)g_timeoutMs;
    int got = 0;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;                        /* 总超时 */
        DWORD remain = (DWORD)(deadline - now);
        if (WaitForSingleObject(g_ackEv, remain) != WAIT_OBJECT_0) break;  /* 总超时 */
        if (InterlockedCompareExchange(&g_ackSeq, 0, 0) >= seq) { got = 1; break; }
        /* 旧 ack：worker 正在追之前堆积的请求，继续等它追到本轮 */
    }

    if (!got) {
        /* ★ 连续超时 = worker 很可能永久卡死在跨进程调用里：换代重建，
           否则此后每轮都超时，圆点永远不再显示。 */
        if (++consecTimeouts >= CARET_RESTART_AFTER) {
            consecTimeouts = 0;
            static ULONGLONG lastRe = 0;
            ULONGLONG now = GetTickCount64();
            /* 重建本身限频 5 秒：纯"慢"（非卡死）的前台程序会持续超时，
               不限频会每 3 秒白重建一次。 */
            if (!lastRe || now - lastRe >= 5000) {
                lastRe = now;
                DbgLog(L"caret: worker stuck (%d consecutive timeouts) -- respawning", CARET_RESTART_AFTER);
                InterlockedIncrement(&g_gen);      /* 旧线程醒来见代际不符自行退出 */
                if (g_workerTh) { CloseHandle(g_workerTh); g_workerTh = NULL; }  /* detach */
                /* 旧 UIA 对象与 walker 不 Release（旧 worker 可能还悬在它的调用上），
                   直接弃用换新：若卡死发生在本地代理锁上，复用旧对象会让新 worker
                   跟着卡。 */
                g_uia = NULL;
                g_walkCV = NULL;
                g_walkRV = NULL;
                CaretSpawnWorker();                /* 新事件对 + 新线程 + 新 COM 对象 */
            }
        }
        if (timeoutOut) *timeoutOut = 1;
        static ULONGLONG last = 0;
        ULONGLONG now2 = GetTickCount64();
        if (!last || now2 - last >= 1000) {
            last = now2;
            DbgLog(L"caret: query timeout >%dms (foreground app busy) -- keep last state", g_timeoutMs);
        }
        return 0;
    }
    consecTimeouts = 0;
    if (InterlockedCompareExchange(&g_ackSeq, 0, 0) != seq) return 0;  /* 防御：不应发生 */
    *out = g_result;
    return out->found ? 1 : 0;
}
