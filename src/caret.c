#include "ime_status.h"

#include <uiautomationclient.h>
#include <oleauto.h>

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
    if (n >= 4) {
        double* d = NULL;
        if (SUCCEEDED(SafeArrayAccessData(arr, (void**)&d))) {
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

/* 方法2：UIA TextPattern2::GetCaretRange（焦点元素） */
static int ViaUiaCaretRange(CaretPos* out) {
    if (!g_uia) return 0;
    IUIAutomationElement* focus = NULL;
    if (FAILED(g_uia->GetFocusedElement(&focus)) || !focus) return 0;
    IUnknown* pat = NULL;
    BOOL ok = FALSE;
    if (SUCCEEDED(focus->GetCurrentPattern(UIA_TextPattern2Id, &pat)) && pat) {
        IUIAutomationTextPattern2* tp2 = NULL;
        if (SUCCEEDED(pat->QueryInterface(IID_PPV_ARGS(&tp2)))) {
            BOOL active = FALSE;
            IUIAutomationTextRange* range = NULL;
            if (SUCCEEDED(tp2->GetCaretRange(&active, &range)) && range) {
                ok = UiRect(out, range);
                if (!ok && SUCCEEDED(range->ExpandToEnclosingUnit(TextUnit_Character)))
                    ok = UiRect(out, range);
                range->Release();
            }
            tp2->Release();
        }
        pat->Release();
    }
    focus->Release();
    return ok ? 1 : 0;
}

/* 方法3：UIA TextPattern::GetSelection（Chromium 系） */
static int ViaUiaSelection(CaretPos* out) {
    if (!g_uia) return 0;
    IUIAutomationElement* focus = NULL;
    if (FAILED(g_uia->GetFocusedElement(&focus)) || !focus) return 0;
    IUnknown* pat = NULL;
    int ok = 0;
    if (SUCCEEDED(focus->GetCurrentPattern(UIA_TextPatternId, &pat)) && pat) {
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
    focus->Release();
    return ok;
}

/* 方法4：IME 组合窗口定位（候选框） */
static int ViaIme(CaretPos* out) {
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    HIMC hImc = ImmGetContext(fg);
    if (!hImc) return 0;
    COMPOSITIONFORM cf;
    ZeroMemory(&cf, sizeof(cf));
    int ok = 0;
    if (ImmGetCompositionWindow(hImc, &cf) && (cf.dwStyle & CFS_POINT)) {
        POINT pt = { cf.ptCurrentPos.x, cf.ptCurrentPos.y };
        if (ClientToScreen(fg, &pt)) {
            out->x = pt.x; out->y = pt.y; out->h = 20;
            ok = 1;
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
    case CARET_UIA_CARET: return L"uia_caret";
    case CARET_UIA_SEL:   return L"uia_sel";
    case CARET_IME:       return L"ime";
    default:              return L"none";
    }
}

#define TRY_CHANNEL(detector, src)                                            \
    do {                                                                      \
        if (detector(out)) {                                                  \
            if (CaretPlausible(out)) { out->source = (src); goto done; }      \
            DbgLog(L"reject %s caret=(%d,%d,h=%d)", CaretSrcName(src),        \
                   out->x, out->y, out->h);                                   \
        }                                                                     \
    } while (0)

int CaretGetPos(CaretPos* out) {
    EnsureUia();
    out->found = 0;
    out->source = CARET_NONE;
    TRY_CHANNEL(ViaGuiInfo, CARET_GUIINFO);
    TRY_CHANNEL(ViaUiaCaretRange, CARET_UIA_CARET);
    TRY_CHANNEL(ViaUiaSelection, CARET_UIA_SEL);
    TRY_CHANNEL(ViaIme, CARET_IME);
done:
    if (out->source != CARET_NONE) { out->found = 1; return 1; }
    out->x = out->y = out->h = 0;
    return 0;
}