#include "ime_indicator.h"

#include <uiautomationclient.h>
#include <oleauto.h>
#include <oleacc.h>   /* MSAA：AccessibleObjectFromWindow + accLocation */

/* ================= 光标位置检测（多级策略） =================
   1) GUI 线程 caret 矩形（GetGUIThreadInfo，经典 Win32 编辑器）；
   2) UI Automation TextPattern2::GetCaretRange（VS Code 等现代编辑器）；
   3) UI Automation TextPattern::GetSelection（Chromium / Chrome 系）；
   4) IME 组合窗口 ImmGetCompositionWindow（候选框定位）。
   坐标一律换算为屏幕物理像素（进程已声明 DPI 感知）。 */

static IUIAutomation* g_uia = NULL;

static void EnsureUia(void) {
    if (g_uia) return;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* RPC_E_CHANGED_MODE 表示本线程已按别的模式初始化过 COM，照样能用 */
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&g_uia));
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
static void LogChFail(const WCHAR* ch, const WCHAR* why, HRESULT hr) {
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
   每个矩形 4 个元素：左 上 宽 高；单位=屏幕坐标）。某些实现给空数组
   时尝试先 ExpandToEnclosingUnit(Character) 再取。 */
static int UiRect(CaretPos* out, IUIAutomationTextRange* range) {
    SAFEARRAY* arr = NULL;
    if (FAILED(range->GetBoundingRectangles(&arr)) || !arr) return 0;
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
            out->h = (int)d[3];
            ok = 1;
            SafeArrayUnaccessData(arr);
        }
    }
    SafeArrayDestroy(arr);
    return ok;
}

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
        POINT pt = { x, y };
        if (ClientToScreen(gi.hwndCaret, &pt)) {
            out->x = pt.x; out->y = pt.y; out->h = h;
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
   返回的是绝对屏幕坐标（accLocation 约定）；部分控件给 (0,0) 假数据，
   交给合理性过滤丢弃。 */
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
    out->x = (int)x; out->y = (int)y; out->h = (int)h;
    return 1;   /* 合理性由 CaretGetPos 的 TRY_CHANNEL 统一过滤（这样 reject 日志才打得出） */
}

/* 自 GetFocusedElement 向上（NVDA 式）找最近一个实现了指定文本模式的元素：
   Chromium/WinUI 的焦点元素常是深处的子节点，文本模式与光标属于其祖先——
   只在焦点元素上查 GetCaretRange/GetSelection 会拿到别的元素/过期的选区（漂移根因）。 */
static IUIAutomationElement* UiaFindPattern(IUIAutomationElement* start, BOOL want2) {
    IUIAutomationElement* e = start;
    if (e) e->AddRef();
    /* ControlView walker：向上导航（无则退回 RawView；两种都要不到父级就停） */
    IUIAutomationTreeWalker* walker = NULL;
    if (g_uia) g_uia->get_ControlViewWalker(&walker);
    IUIAutomationTreeWalker* rawWalker = NULL;
    if (g_uia) g_uia->get_RawViewWalker(&rawWalker);

    for (int depth = 0; e && depth < 24; depth++) {
        IUnknown* pat = NULL;
        if (SUCCEEDED(e->GetCurrentPattern(want2 ? UIA_TextPattern2Id : UIA_TextPatternId, &pat)) && pat) {
            pat->Release();
            if (walker) walker->Release();
            if (rawWalker) rawWalker->Release();
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
    }
    if (e) e->Release();
    if (walker) walker->Release();
    if (rawWalker) rawWalker->Release();
    return NULL;
}

/* 方法2：UIA TextPattern2::GetCaretRange（向上找文本节点） */
static int ViaUiaCaretRange(CaretPos* out) {
    if (!g_uia) return 0;
    IUIAutomationElement* focus = NULL;
    HRESULT hr = g_uia->GetFocusedElement(&focus);
    if (FAILED(hr) || !focus) {
        /* 兜底：按焦点窗口句柄取元素。GetFocusedElement 依赖对端 UIA provider
           的焦点上报，某些应用（权限差异/沙箱/provider 忙）会拿不到焦点元素，
           但窗口句柄仍然有效 —— Chromium 系应用常见。 */
        HWND fw = ImeFocusedWindow();
        if (fw && SUCCEEDED(g_uia->ElementFromHandle(fw, &focus)) && focus) {
            LogChFail(L"uia_caret", L"GetFocusedElement failed -> fallback hwnd", hr);
        } else {
            LogChFail(L"uia_caret", L"GetFocusedElement & hwnd both failed", hr);
            return 0;
        }
    }
    IUIAutomationElement* el = UiaFindPattern(focus, TRUE);
    focus->Release();
    if (!el) return 0;
    int ok = 0;
    IUnknown* pat = NULL;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_TextPattern2Id, &pat)) && pat) {
        IUIAutomationTextPattern2* tp2 = NULL;
        if (SUCCEEDED(pat->QueryInterface(IID_PPV_ARGS(&tp2)))) {
            BOOL active = FALSE;
            IUIAutomationTextRange* range = NULL;
            if (SUCCEEDED(tp2->GetCaretRange(&active, &range)) && range) {
                /* ★ isActive 就是"当前到底有没有活动光标"。为 FALSE 时不能
                   再往下走 MSAA —— 那会拿回一个过期的坐标把点钉在屏幕上。
                   取到文本模式却说没活动光标，就是明确的"没有光标"。 */
                if (active) {
                    ok = UiRect(out, range);
                    if (!ok && SUCCEEDED(range->ExpandToEnclosingUnit(TextUnit_Character)))
                        ok = UiRect(out, range);
                } else {
                    ok = -1;   /* 明确无光标 */
                }
                range->Release();
            }
            tp2->Release();
        }
        pat->Release();
    }
    el->Release();
    return ok;
}

/* 方法3：UIA TextPattern::GetSelection（向上找文本节点，Chromium 系） */
static int ViaUiaSelection(CaretPos* out) {
    if (!g_uia) return 0;
    IUIAutomationElement* focus = NULL;
    HRESULT hr = g_uia->GetFocusedElement(&focus);
    if (FAILED(hr) || !focus) {
        HWND fw = ImeFocusedWindow();
        if (fw && SUCCEEDED(g_uia->ElementFromHandle(fw, &focus)) && focus) {
            LogChFail(L"uia_sel", L"GetFocusedElement failed -> fallback hwnd", hr);
        } else {
            return 0;   /* uia_caret 通道已记过失败原因，不再重复刷 */
        }
    }
    IUIAutomationElement* el = UiaFindPattern(focus, FALSE);
    focus->Release();
    if (!el) return 0;
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
                    if (SUCCEEDED(sel->GetElement(0, &range))) {
                        ok = UiRect(out, range);
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
            POINT pt = { cf.ptCurrentPos.x, cf.ptCurrentPos.y };
            if (ClientToScreen(fg, &pt)) {
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

/* 返回 1=拿到坐标，-1=该通道明确报告"当前没有光标"（不再试后面的通道） */
#define TRY_CHANNEL(detector, src)                                            \
    do {                                                                      \
        int r = (detector)(out);                                              \
        if (r < 0) {                                                          \
            LogNoCaret(L"uia_caret reports inactive");                        \
            goto done;                                                        \
        }                                                                     \
        if (r > 0) {                                                          \
            if (CaretPlausible(out)) { out->source = (src); goto done; }      \
            DbgLog(L"reject %s caret=(%d,%d,h=%d)", CaretSrcName(src),        \
                   out->x, out->y, out->h);                                   \
        }                                                                     \
    } while (0)

/* ---------- 单次探测（**会阻塞**：内部是跨进程 UIA/MSAA 调用） ----------
   只能在查询线程里跑，别在检测线程直接调 —— 这就是超时机制存在的理由。 */
static int CaretProbeOnce(CaretPos* out) {
    EnsureUia();
    out->found = 0;
    out->source = CARET_NONE;
    /* 全屏（看视频/演示）时一律不显示：此时 MSAA 会报出上一次的陈旧光标位置，
       圆点会钉在画面上挡视线。 */
    if (g_cfg.hideFullscreen && CaretIsForegroundFullscreen()) {
        LogNoCaret(L"fullscreen foreground");
        goto done;
    }
    /* UIA caret 排在 MSAA 之前：它带 isActive，能明确区分"有光标"和
       "只有个过期坐标"；MSAA 拿这个信息，所以只能当兜底。 */
    TRY_CHANNEL(ViaGuiInfo, CARET_GUIINFO);
    TRY_CHANNEL(ViaUiaCaretRange, CARET_UIA_CARET);
    TRY_CHANNEL(ViaMsaa, CARET_MSAA);
    TRY_CHANNEL(ViaUiaSelection, CARET_UIA_SEL);
    TRY_CHANNEL(ViaIme, CARET_IME);
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
static volatile LONG   g_gen = 0;         /* worker 代际：重建 +1，旧代见之即退 */
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
   两者必须分开 —— 前者沿用上一轮显示，后者才收起圆点。 */
int CaretGetPosEx(CaretPos* out, int* timeoutOut) {
    CaretPos local;
    if (!out) out = &local;
    out->found = 0;
    out->source = CARET_NONE;
    out->x = out->y = out->h = 0;
    if (timeoutOut) *timeoutOut = 0;
    if (!g_workerTh) CaretSpawnWorker();       /* 上次重建失败：本轮再试 */
    if (!g_workerTh) return 0;

    static int consecTimeouts = 0;             /* 连续超时计数（检测线程独占，无需同步） */

    LONG seq = InterlockedIncrement(&g_reqSeq);
    SetEvent(g_reqEv);
    if (WaitForSingleObject(g_ackEv, (DWORD)g_timeoutMs) != WAIT_OBJECT_0) {
        /* ★ 连续超时 = worker 很可能永久卡死在跨进程调用里：换代重建，
           否则此后每轮都超时，圆点永远不再显示（重启才能恢复的那种症状）。 */
        if (++consecTimeouts >= CARET_RESTART_AFTER) {
            consecTimeouts = 0;
            static ULONGLONG lastRe = 0;
            ULONGLONG now = GetTickCount64();
            if (!lastRe || now - lastRe >= 5000) {
                lastRe = now;
                DbgLog(L"caret: worker stuck (%d consecutive timeouts) -- respawning", CARET_RESTART_AFTER);
            }
            InterlockedIncrement(&g_gen);      /* 旧线程醒来见代际不符自行退出 */
            if (g_workerTh) { CloseHandle(g_workerTh); g_workerTh = NULL; }  /* detach */
            /* 旧 UIA 对象不 Release（旧 worker 可能还悬在它的调用上），直接弃用换新：
               若卡死发生在本地代理锁上，复用旧对象会让新 worker 跟着卡。 */
            g_uia = NULL;
            CaretSpawnWorker();                /* 新事件对 + 新线程 + 新 COM 对象 */
        }
        if (timeoutOut) *timeoutOut = 1;
        static ULONGLONG last = 0;
        ULONGLONG now = GetTickCount64();
        if (!last || now - last >= 1000) {
            last = now;
            DbgLog(L"caret: query timeout >%dms (foreground app busy) -- keep last state", g_timeoutMs);
        }
        return 0;
    }
    consecTimeouts = 0;
    if (InterlockedCompareExchange(&g_ackSeq, 0, 0) != seq) return 0;  /* 过期结果，不用 */
    *out = g_result;
    return out->found ? 1 : 0;
}
