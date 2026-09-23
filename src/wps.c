/* ======================================================================
   IMEIndicator —— WPS 系（文字 / 演示 / 表格）光标 COM 通道（自 caret.c 拆出）
   Copyright (C) 2026  qm

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
   ------------------------------------------------------------------ */

/* WPS 系三条光标通道的独立实现。对外的只有 wps.h 里那三个函数；探测顺序与
   通用通道（guiinfo/UIA/MSAA/IME）的衔接见 caret.c 的 CaretProbeOnce。
   坐标一律换算为屏幕物理像素（进程已声明 DPI 感知）；这些调用都是**跨进程**
   且没有超时参数 → 只能在 caret.c 的查询线程里跑。 */

#include "ime_indicator.h"
#include "wps.h"
#include "bridge.h"     /* 管理员 + WPS 普通权限时，COM 查询交给降权桥（见下） */

#include <oleauto.h>

/* ================= 方法0：WPS 系（Word / PowerPoint / Excel 对象模型 COM） =================
   WPS 文字、演示、表格是同一个病：外壳上完整实现了自家 Office 的对象模型，
   但**光标对标准接口全不可见**，通用四通道对它们毫无办法。

   文字（进程 wps.exe、窗口类 OpusApp、文档控件 _WwG）本机实测：
     · GetGUIThreadInfo   → hwndCaret = NULL（不用 Win32 光标，与 MS Word 同源）
     · MSAA OBJID_CARET   → S_FALSE（没有 caret 对象）
     · UIA TextPattern(2) → 整窗子树 0 个（它的 provider 是 Qt 系 KxWpsView）
     · ImmGetContext      → NULL（走 TSF 不走 IMM）
   取插入点：
     Application.Selection.Range                          插入点（选区折叠时即光标）
     Application.ActiveWindow.GetPoint(l,t,w,h, Range)     该 Range 的屏幕像素矩形
   实测矩形横竖都跟得住（三个不同行的 y 分别 460/496/532，同列不同行的 x 一致），
   H 是行高、W=0（折叠插入点），语义与 guiinfo 通道的 rcCaret 一致。
   ★ 但 GetPoint 给的是**折叠 Range 所属的那个块**的框，不一定就是光标所在的行：
     实测一个用软换行排成 3 行的段，光标停在末行行首时拿到 (769,496,0,165)
     —— 769 是插入点 x（对），496/165 却是**整段**的顶与高（165 ≈ 3 行），
     圆点于是落在段落左下角而不是光标旁。同一次里另一拍又报到 (1303,460,0,36)
     这种正常的单行框，说明它取决于 Range 落在哪种块上。
     单行段落（最常见）没问题；多行段落待改：思路与演示那条一致 —— 拿插入点
     位置构造 1 个字符的 Range 再 GetPoint，取那一行/那一个字的紧框。
   ★ 非折叠选区没有插入点：GetPoint 给的是选区包围盒，圆点会跳到文本块的
     左下角（实测全选 sel=[0,51] → rect=(769,420,548,76)，看着就是"位置全错"）。
     所以先读 Range.Start/End，不相等时沿用上一次的插入点坐标（见 WpsCaretText）。

   演示（进程 wpp.exe、窗口类 PP12FrameClass）本机实测：
     · GetGUIThreadInfo   → hwndCaret = mdiClass，但 rcCaret = (0,0,1,1)
       —— 是**屏幕原点上的 1×1 假矩形**（MSAA 照抄同一份）。所以通用通道即使
       "命中"，圆点也钉在屏幕左上角，比不显示还糟，必须抢在它们之前掐断。
     · UIA                → 焦点链（KxWppView → KxWppSlidePane → …）整条没有 TextPattern
   取插入点（都是幻灯片点坐标，用 ActiveWindow.PointsToScreenPixelsX/Y 换屏幕像素）：
     Application.ActiveWindow.Selection  Type 必须是 3(ppSelectionText)
       ★ 非编辑态 Selection.TextRange 给的是**整个文本框**的包围盒（实测选中形状时
         Bound=(448,179,63,93)）→ 放行的话圆点跑到文本块边上，所以 Type 是硬闸门。
     ★ 折叠范围的 Selection.TextRange.BoundLeft **不是插入点 x**，是**段落文本区
       左缘**（实测：居中标题里插入点在段落右端，BoundLeft 仍是 136.9 = 幻灯片左边
       缘 129 + 默认内缩 7.2pt；而同一时刻整段文本的紧框是 L=411.4 W=156.6，
       右缘 568.1）。所以必须换成逐字符框：TextRange.Parent（= 文本框的 TextFrame）
       → .TextRange 拿整段范围 → .Characters(p, 1) 拿插入点所在字符的紧框：
       插入点在这字符**之前** → 取它的左缘；插入点在整段末尾（p 超出字符数）→
       取末字符的右缘。逐字符实测 char[1..4] = 411.4 / 454.8 / 474.8 / 521.4，
       与字形宽度对得上。上/下方向本来就是对的（行顶 + 行高），直接用字符框的。
     ★ 名字必须**在哪个对象上就解析在哪**：Selection.TextRange 的 DISPID 是 2010、
       TextFrame.TextRange 是 2004，不通用。拿 2010 去 Invoke TextFrame（TextRange
       的 Parent）会返回 hr=S_OK 但结果不是对象 —— 按 FAILED 判会以为成功，
       实际是"整条通道静默失效"（本次踩过）。
     ★ 还没敲字的空文本框没有字符框：Length=0，这是**正常的**，不能当失败掐断链路
       （否则空框里圆点整个消失）。此时插入点=段落左缘（BoundLeft），行高取
       TextRange.Font.Size（点）。实测 60pt → 92px，与有字时字符框高 93.6 一致。
     ★ 顺序也是坑：Selection.TextRange 只在**文本编辑态**存在（非编辑态 Invoke 直接
       抛异常），而"是否在编辑态"只能问 Selection.Type —— 所以 Type 闸门必须排在
       解析 Stage 2 之前（见 PpNav / PpResolve 为什么拆成两段）。
     ★ 换算实测就是线性映射 screen = 原点 + 点 × 比例，X/Y 同一个比例：
       PTSX(0)=815、PTSY(0)=284、1.53px·pt⁻¹（Bound 136 → 815+136×1.53=1023，
       与 PTSX(136) 实测值相同）。所以标定一次就够，之后每个 Bound 都在本地换算。
       ★ 比例别用 PTSY(1)-PTSY(0)：结果是整数，会把 1.53 截成 1（实测行高变 60px）。
     ★ 开销：逐字符框那条链要 11 次跨进程 Invoke（≈5~8ms），而"插入点序号 + 窗口
       位置都没变"时几何一定没变 —— 于是先只花 3 次（Type / TextRange / Start）
       确认序号，没变就直接用缓存的屏幕坐标（实测 2ms，与文字通道同价），
       变了才走全量。

   InputTip 走的是另一条路：往目标进程注入 WH_CALLWNDPROC 钩子、用
   SendMessageTimeout 把取光标的代码放进对方线程里跑（见其 var.ahk 的
   getCaretPosFromHook / modeNameList 里的 "HOOK"）。那要自带一段机器码 plus
   OpenProcess + CreateRemoteThread，这里用 COM 自动化换掉了注入。

   ★ 附着只能用 GetActiveObject（ROT），**不能用 CoCreateInstance**：后者会另起
     一个没有文档的 WPS 进程（实测其 ActiveDocument 直接抛异常）读不到用户窗口
     里那篇文档。ROT 按完整性级别隔离 → 本进程是管理员、WPS 是普通权限时永远
     拿不到（MK_E_UNAVAILABLE），那种情况退化为"WPS 里不显示"。

   ★ 节流：Selection.Range / TextRange 都是**快照**，必须每次新建（复用同一份矩形
     不会跟着光标动）。按"静止减速、动了跟紧"控制查询节奏（WPS_IDLE_GAP_MS /
     WPS_BURST_MS），不硬扛 15ms 的追踪节奏。GetIDsOfNames 同样是跨进程调用 →
     名字只解析一次，DISPID 缓存住。 */
#define WPS_IDLE_GAP_MS  50    /* 静止时的最小查询间隔：约 2ms/次 → 单核 ~4% */
#define WPS_BURST_MS     500   /* 发现光标移动后按追踪节奏跟这么久（打字会一直续上）*/
#define WPS_RETRY_MS     1000  /* 附着/查询失败后的退避：CLSIDFromProgID+ROT 约 7ms */
#define PP_CALIB_MS      1000  /* 演示：点→像素 标定值的有效期（缩放/移窗都会改变它）*/
#define PP_GEO_MS        500   /* 演示：插入点几何缓存的有效期（兜住缩放/滚动这类
                                  既不换插入点序号、也不移窗口的变化）             */

#define WPS_DOC_CLASS    L"_WwG"   /* 文字的文档编辑控件（焦点在它身上才算在编辑）*/
#define PP_SEL_TEXT      3         /* ppSelectionText：只有它才是"在编辑文本"    */

/* IID_IDispatch 只在 initguid.h 那个 TU 里才有定义（否则要靠 uuid.lib）。自带
   一份常量，值固定、与链接顺序无关。 */
static const IID kIID_Dispatch =
    {0x00020400, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

/* 一条 WPS 系通道的全部状态（文字、演示各一份）。两者的差异只在"附着后怎么取
   矩形"，剩下的进程判定 / 附着 / 节流 / 缓存完全一致 —— 所以共用这套骨架。 */
typedef struct {
    const WCHAR* progid;    /* Application 的 ProgID                      */
    const WCHAR* frame;     /* 顶层窗口类名（先于进程名判，本地调用极便宜）  */
    const WCHAR* proc;      /* 进程名                                     */
    const WCHAR* tag;       /* 日志前缀                                   */
    IDispatch*   app;       /* Application                                */
    IDispatch*   sel;       /* Selection                                  */
    IDispatch*   win;       /* ActiveWindow                               */
    ULONGLONG    retryAt;   /* 附着失败退避到期时刻                        */
    ULONGLONG    nextAt;    /* 节流：最早的下一次真查时刻                   */
    ULONGLONG    burstUntil;/* 跟紧窗口结束时刻                            */
    int          cached, cx, cy, ch;  /* 上一次的矩形（节流期内复用）       */
    LONG         gen;       /* 缓存属于哪一代 worker                       */
} WpsChan;

static WpsChan g_wr = { L"KWPS.Application", L"OpusApp",       L"wps.exe", L"wps" };
static WpsChan g_pp = { L"KWPP.Application", L"PP12FrameClass", L"wpp.exe", L"wpp" };
static WpsChan g_et = { L"KET.Application",  L"XLMAIN",        L"et.exe",  L"et" };

/* 前台是不是这条通道的窗口。**必须按进程名判**：真 MS Word 的窗口类同样是
   OpusApp，而 Word 有 UIA 文本、轮不到这条通道。类名先判（本地调用、极便宜），
   命中才去查进程名（要 OpenProcess）。fgOut 命中时回填前台窗口句柄。 */
static int WpsIsForeground(const WpsChan* c, HWND* fgOut) {
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    WCHAR cls[32];
    if (!GetClassNameW(fg, cls, 32) || wcscmp(cls, c->frame) != 0) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return 0;
    int hit = 0;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        WCHAR full[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, full, &n)) {
            WCHAR* name = full;
            for (WCHAR* q = full; *q; q++) if (*q == L'\\') name = q + 1;
            hit = (_wcsicmp(name, c->proc) == 0);
        }
        CloseHandle(p);
    }
    if (hit && fgOut) *fgOut = fg;
    return hit;
}

/* 名字 -> DISPID（跨进程调用，只在附着时走一次） */
static BOOL WpsDispId(IDispatch* o, const WCHAR* name, DISPID* id) {
    LPOLESTR nm = (LPOLESTR)name;      /* GetIDsOfNames 只读这个字符串 */
    return SUCCEEDED(o->GetIDsOfNames(IID_NULL, &nm, 1, LOCALE_USER_DEFAULT, id));
}

/* 取一个返回对象的属性（引用归调用方 Release）。异常信息吃掉不打印：
   这里失败是常态（没开文档 / 窗口切走了），细节靠调用点的 LogChFail 留痕。 */
static IDispatch* WpsPropDisp(IDispatch* o, DISPID id) {
    DISPPARAMS dp;
    ZeroMemory(&dp, sizeof(dp));
    VARIANT r; VariantInit(&r);
    EXCEPINFO ei; ZeroMemory(&ei, sizeof(ei));
    UINT ae = 0;
    HRESULT hr = o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                           DISPATCH_PROPERTYGET, &dp, &r, &ei, &ae);
    if (ei.bstrDescription) SysFreeString(ei.bstrDescription);
    if (ei.bstrSource) SysFreeString(ei.bstrSource);
    if (FAILED(hr) || r.vt != VT_DISPATCH || !r.pdispVal) { VariantClear(&r); return NULL; }
    return r.pdispVal;
}

/* 取一个数值属性（I4/I2/R4/R8 都认：点值是浮点，字号/序号会随版本给整数）。 */
static BOOL WpsGetNum(IDispatch* o, DISPID id, double* v) {
    if (!id) return FALSE;
    DISPPARAMS dp;
    ZeroMemory(&dp, sizeof(dp));
    VARIANT r; VariantInit(&r);
    EXCEPINFO ei; ZeroMemory(&ei, sizeof(ei));
    UINT ae = 0;
    HRESULT hr = o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                           DISPATCH_PROPERTYGET, &dp, &r, &ei, &ae);
    if (ei.bstrDescription) SysFreeString(ei.bstrDescription);
    if (ei.bstrSource) SysFreeString(ei.bstrSource);
    BOOL ok = SUCCEEDED(hr) &&
              (r.vt == VT_I4 || r.vt == VT_I2 || r.vt == VT_R4 || r.vt == VT_R8);
    if (ok) *v = (r.vt == VT_R4) ? (double)r.fltVal : (r.vt == VT_R8) ? r.dblVal
                                                                    : (double)r.lVal;
    VariantClear(&r);
    return ok;
}

/* Window.GetPoint(左, 上, 宽, 高, Range)：4 个 [out] long + 1 个 Range。
   ★ DISPPARAMS 的实参是**倒序**存放的：最后一个形参在最前面，所以这里 a[4]
   对应第 1 个形参、a[0] 对应第 5 个。写反了 WPS 直接报类型不匹配。 */
static BOOL WpsGetPoint(IDispatch* win, DISPID id, IDispatch* range,
                        long* L, long* T, long* W, long* H) {
    VARIANT a[5];
    for (int i = 0; i < 5; i++) VariantInit(&a[i]);
    a[4].vt = VT_BYREF | VT_I4; a[4].plVal = L;
    a[3].vt = VT_BYREF | VT_I4; a[3].plVal = T;
    a[2].vt = VT_BYREF | VT_I4; a[2].plVal = W;
    a[1].vt = VT_BYREF | VT_I4; a[1].plVal = H;
    a[0].vt = VT_DISPATCH;      a[0].pdispVal = range;
    DISPPARAMS dp;
    ZeroMemory(&dp, sizeof(dp));
    dp.rgvarg = a;
    dp.cArgs = 5;
    VARIANT res; VariantInit(&res);
    EXCEPINFO ei; ZeroMemory(&ei, sizeof(ei));
    UINT ae = 0;
    HRESULT hr = win->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                             DISPATCH_METHOD, &dp, &res, &ei, &ae);
    VariantClear(&res);
    if (ei.bstrDescription) SysFreeString(ei.bstrDescription);
    if (ei.bstrSource) SysFreeString(ei.bstrSource);
    return SUCCEEDED(hr);
}

/* PointsToScreenPixelsX/Y(点) -> 屏幕像素。实参一个 R4（点值通常是小数）。 */
static BOOL WpsPts(IDispatch* win, DISPID id, double pt, long* px) {
    if (!id) return FALSE;
    VARIANT a[1];
    VariantInit(&a[0]);
    a[0].vt = VT_R4; a[0].fltVal = (float)pt;
    DISPPARAMS dp;
    ZeroMemory(&dp, sizeof(dp));
    dp.rgvarg = a;
    dp.cArgs = 1;
    VARIANT r; VariantInit(&r);
    EXCEPINFO ei; ZeroMemory(&ei, sizeof(ei));
    UINT ae = 0;
    HRESULT hr = win->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                             DISPATCH_METHOD, &dp, &r, &ei, &ae);
    if (ei.bstrDescription) SysFreeString(ei.bstrDescription);
    if (ei.bstrSource) SysFreeString(ei.bstrSource);
    BOOL ok = SUCCEEDED(hr) && (r.vt == VT_I4 || r.vt == VT_R4 || r.vt == VT_R8);
    if (ok) *px = (r.vt == VT_R4) ? (long)r.fltVal : (r.vt == VT_R8) ? (long)r.dblVal
                                                                   : r.lVal;
    VariantClear(&r);
    return ok;
}

/* 本轮报废：丢掉代理（**只弃用不 Release**，理由同 g_uia）、退避、本轮不显示 */
static void WpsFail(WpsChan* c, ULONGLONG now) {
    c->app = NULL; c->sel = NULL; c->win = NULL; c->cached = 0;
    c->retryAt = now + WPS_RETRY_MS;
    c->nextAt  = now + WPS_IDLE_GAP_MS;
}

/* 附着到正在运行的 WPS。成功返回 1；失败要退避，否则每轮都白付
   CLSIDFromProgID + GetActiveObject（实测约 7ms）。 */
static int WpsAttach(WpsChan* c) {
    ULONGLONG now = GetTickCount64();
    if (c->app && c->sel && c->win) return 1;
    if (now < c->retryAt) return 0;
    /* 换代（worker 卡死被重建）后上一代的 COM 代理可能还悬着别人的调用 →
       只弃用不 Release，直接换新的：与上面 g_uia 的处理同理。 */
    if (c->gen != g_gen) {
        c->app = NULL; c->sel = NULL; c->win = NULL; c->cached = 0;
        c->gen = g_gen;
    }
    if (!c->app) {
        CLSID clsid;
        if (FAILED(CLSIDFromProgID(c->progid, &clsid))) {
            LogChFail(c->tag, L"CLSIDFromProgID failed (COM not registered?)", 0);
            WpsFail(c, now);
            return 0;
        }
        IUnknown* unk = NULL;
        HRESULT hr = GetActiveObject(clsid, NULL, &unk);   /* ROT：不新建实例 */
        if (FAILED(hr) || !unk) {
            if (unk) unk->Release();
            LogChFail(c->tag,
                      hr == (HRESULT)MK_E_UNAVAILABLE
                          ? L"not in ROT (no document? / we are elevated)"
                          : L"GetActiveObject failed",
                      hr);
            WpsFail(c, now);
            return 0;
        }
        IDispatch* app = NULL;
        hr = unk->QueryInterface(kIID_Dispatch, (void**)&app);
        unk->Release();
        if (FAILED(hr) || !app) {
            LogChFail(c->tag, L"QueryInterface(IDispatch) failed", hr);
            WpsFail(c, now);
            return 0;
        }
        c->app = app;
    }
    return 1;
}

/* ---------- 降权桥：管理员进程取不到 ROT 时的退路 ----------
   ROT 按完整性级别隔离 → 本进程提权时永远附不上普通权限的 WPS（见文件头）。
   这时把整条查询交给"普通权限的自己"（bridge.c 拉起的子进程）：那边跑的就是
   下面这三个函数，COM 照常工作，坐标经管道送回来。协议只有
   "通道号 -> 结果 + 矩形"（结果沿用本地的 1 / 0 / -1 三态）。 */
#define WPS_CH_WR  1
#define WPS_CH_PP  2
#define WPS_CH_ET  3
#define WPS_BR_WAIT_MS 120   /* 等桥一帧应答的上限（子进程查询实测 2~8ms） */

static int WpsBridgeQuery(int ch, CaretPos* out) {
    if (!BrEnsure()) return -1;                 /* 桥还没起来（首轮正在拉进程） */
    LONG req = (LONG)ch;
    BYTE* buf = NULL;
    DWORD n = 0;
    if (!BrCall((const BYTE*)&req, sizeof(req), &buf, &n, WPS_BR_WAIT_MS)) {
        BrDrop();
        return -1;
    }
    if (n < sizeof(LONG) * 5) { free(buf); BrDrop(); return -1; }   /* 协议不符 */
    LONG* r = (LONG*)buf;
    int rc = (int)r[0];
    if (rc == 1) {
        out->x = (int)r[1]; out->y = (int)r[2];
        out->h = (int)r[3]; out->w = (int)r[4];
    }
    free(buf);
    return rc;
}

/* 附着失败时的退路。★ 只在"根本没附上"且本进程提权时才转桥：附着成功说明
   COM 是通的（同一完整性级别），失败属于没文档/没光标之类，桥也帮不上；
   本进程没提权时更是本地就能查（子进程一样是普通权限，白绕一趟）。 */
static int WpsBridgeFallback(WpsChan* c, int ch, CaretPos* out) {
    if (c->app || !ProcIsElevated()) return -1;
    return WpsBridgeQuery(ch, out);
}

/* 桥子进程的请求处理（main.c 的 --bridge 入口接它）：跑本地 COM 查询，把
   "结果 + 矩形"回给父进程。子进程是普通权限 → 这里不会再转到桥。 */
BOOL WpsBridgeHandler(const BYTE* req, DWORD reqLen, BYTE** resp, DWORD* respLen) {
    LONG ch = 0;
    if (reqLen >= sizeof(LONG)) memcpy(&ch, req, sizeof(ch));
    CaretPos cp;
    ZeroMemory(&cp, sizeof(cp));
    int r = 0;
    if (ch == WPS_CH_WR)      r = WpsCaretText(&cp);
    else if (ch == WPS_CH_PP) r = WpsCaretShow(&cp);
    else if (ch == WPS_CH_ET) r = WpsCaretGrid(&cp);
    LONG o[5];
    o[0] = (LONG)r; o[1] = (LONG)cp.x; o[2] = (LONG)cp.y;
    o[3] = (LONG)cp.h; o[4] = (LONG)cp.w;
    *resp = (BYTE*)malloc(sizeof(o));
    if (!*resp) return FALSE;
    memcpy(*resp, o, sizeof(o));
    *respLen = (DWORD)sizeof(o);
    return TRUE;
}

/* 节流期内复用上次结果。返回 1=已给出结论（out 可能已填好，也可能是"没光标"），
   0=不节流、调用方该去真查。 */
static int WpsThrottled(WpsChan* c, CaretPos* out) {
    if (GetTickCount64() >= c->nextAt) return 0;
    if (!c->cached) return -1;
    out->x = c->cx; out->y = c->cy; out->h = c->ch; out->w = 0;
    return 1;
}

/* 记账：动了就进跟紧窗口，没动就退回静止间隔 */
static void WpsStore(WpsChan* c, long x, long y, long h) {
    ULONGLONG now = GetTickCount64();
    if (!c->cached || x != c->cx || y != c->cy || h != c->ch)
        c->burstUntil = now + WPS_BURST_MS;
    c->cached = 1;
    c->cx = (int)x; c->cy = (int)y; c->ch = (int)h;
    c->nextAt = now + ((now < c->burstUntil) ? 0 : WPS_IDLE_GAP_MS);
}

/* ---------- 方法0a：WPS 文字 ---------- */
static DISPID g_wrIdRange = 0;      /* Selection.Range  */
static DISPID g_wrIdGetPoint = 0;   /* Window.GetPoint */
static DISPID g_wrIdStart = 0;      /* Range.Start（判"折叠=插入点"）*/
static DISPID g_wrIdEnd = 0;        /* Range.End                        */

/* 附着后补齐文字专属的代理与 DISPID。1=可以取矩形了。 */
static int WrNav(WpsChan* c) {
    if (!c->sel) {
        DISPID id = 0;
        if (!WpsDispId(c->app, L"Selection", &id)) goto fail;
        c->sel = WpsPropDisp(c->app, id);
        if (!c->sel) goto fail;
    }
    if (!c->win) {
        DISPID id = 0;
        if (!WpsDispId(c->app, L"ActiveWindow", &id)) goto fail;
        c->win = WpsPropDisp(c->app, id);
        if (!c->win) goto fail;
    }
    /* 名字 -> DISPID 也只解析一次（"Range"/"GetPoint" 都不会是 DISPID 0，
       那个值留给默认成员，所以用 0 当"没解析过"是安全的） */
    if (!g_wrIdRange    && !WpsDispId(c->sel, L"Range", &g_wrIdRange)) goto fail;
    if (!g_wrIdGetPoint && !WpsDispId(c->win, L"GetPoint", &g_wrIdGetPoint)) goto fail;
    return 1;
fail:
    WpsFail(c, GetTickCount64());
    return 0;
}

/* 方法0a：WPS 文书的插入点矩形。返回 1=拿到，-1=确认"WPS 里没有光标"
   （其余通道对它毫无办法，见文件头，所以直接掐断链路省掉 19 层 UIA 空爬），
   0=前台不是 WPS 文字，与本通道无关。 */
int WpsCaretText(CaretPos* out) {
    if (!g_cfg.wpsCom) return 0;
    HWND fg = NULL;
    if (!WpsIsForeground(&g_wr, &fg)) return 0;
    /* 焦点不在文档编辑区（点了功能区、菜单、任务窗格）：Selection 还停在旧位置，
       GetPoint 给出的是**上一个光标**的矩形 —— 那正是"点钉在屏幕上"。文档控件的
       类名固定是 _WwG，本地一查就知道。 */
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    GUITHREADINFO gi;
    ZeroMemory(&gi, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (!tid || !GetGUIThreadInfo(tid, &gi)) {
        LogChFail(g_wr.tag, L"GetGUIThreadInfo failed", 0);
        return -1;
    }
    WCHAR fc[32];
    if (!gi.hwndFocus || !GetClassNameW(gi.hwndFocus, fc, 32) ||
        wcscmp(fc, WPS_DOC_CLASS) != 0) {
        LogChFail(g_wr.tag, L"focus not in document", 0);
        return -1;
    }
    int thr = WpsThrottled(&g_wr, out);
    if (thr) return thr;
    if (!WpsAttach(&g_wr) || !WrNav(&g_wr)) return WpsBridgeFallback(&g_wr, WPS_CH_WR, out);
    IDispatch* rng = WpsPropDisp(g_wr.sel, g_wrIdRange);
    if (!rng) {
        LogChFail(g_wr.tag, L"Selection.Range failed", 0);
        WpsFail(&g_wr, GetTickCount64());
        return -1;
    }
    /* ★ 非折叠选区（拖选 / Ctrl+A）没有插入点：GetPoint 给的是**选区包围盒**，
       圆点会跳到文本块的左下角。实测全选时 sel=[0,51] → rect=(769,420,548,76)，
       看着就像"位置完全错了"。选区期间沿用上一次的插入点坐标（稳定不跳），
       从来没查到过坐标就本轮不显示。Start/End 在 Range 上解析（与 GetPoint 的
       ActiveWindow 不是同一个接口，DISPID 不能互相挪用）。 */
    if (!g_wrIdStart && !WpsDispId(rng, L"Start", &g_wrIdStart)) g_wrIdStart = 0;
    if (!g_wrIdEnd && !WpsDispId(rng, L"End", &g_wrIdEnd)) g_wrIdEnd = 0;
    double selStart = 0, selEnd = 0;
    if (g_wrIdStart && g_wrIdEnd &&
        WpsGetNum(rng, g_wrIdStart, &selStart) && WpsGetNum(rng, g_wrIdEnd, &selEnd) &&
        selStart != selEnd) {
        rng->Release();
        if (g_wr.cached) {
            out->x = g_wr.cx; out->y = g_wr.cy; out->h = g_wr.ch; out->w = 0;
            return 1;
        }
        return -1;
    }
    long L = -1, T = -1, W = -1, H = -1;
    BOOL ok = WpsGetPoint(g_wr.win, g_wrIdGetPoint, rng, &L, &T, &W, &H);
    rng->Release();
    if (!ok) {
        /* 缓存失效（WPS 重启 / 换了文档窗口 / 没打开文档）→ 丢掉下次重取 */
        LogChFail(g_wr.tag, L"ActiveWindow.GetPoint failed", 0);
        WpsFail(&g_wr, GetTickCount64());
        return -1;
    }
    WpsStore(&g_wr, L, T, H);
    out->x = (int)L; out->y = (int)T; out->h = (int)H; out->w = (int)W;
    return 1;
}

/* ---------- 方法0b：WPS 演示 ---------- */
static DISPID g_ppIdSel = 0;                   /* Window.Selection              */
static DISPID g_ppIdType = 0, g_ppIdTR = 0;    /* Selection.Type / TextRange    */
static DISPID g_ppIdStart = 0;                 /* TextRange.Start（插入点序号）   */
static DISPID g_ppIdParent = 0;                /* TextRange.Parent（= TextFrame） */
static DISPID g_ppIdTfTR = 0;                  /* ★ TextFrame 自己的 TextRange   */
static DISPID g_ppIdLen = 0, g_ppIdChars = 0;  /* TextRange.Length / Characters  */
static DISPID g_ppIdBL = 0, g_ppIdBW = 0;      /* 字符框：左缘 / 宽               */
static DISPID g_ppIdBT = 0, g_ppIdBH = 0;      /* 字符框：上缘 / 高（= 行高）      */
static DISPID g_ppIdSize = 0;                  /* Font.Size（点，空文本框当行高用）*/
static DISPID g_ppIdFont = 0;                  /* TextRange.Font（Size 挂在它身上）*/
static DISPID g_ppIdPtsX = 0, g_ppIdPtsY = 0;  /* Window.PointsToScreenPixelsX/Y */
static long   g_ppOx = 0, g_ppOy = 0;          /* 标定：幻灯片左上角的屏幕像素     */
static double g_ppScaleX = 0, g_ppScaleY = 0;  /* 标定：点 -> 像素                */
static ULONGLONG g_ppCalibAt = 0;              /* 标定的到期时刻                  */
static RECT   g_ppCalibRect;                   /* 标定时的窗口矩形（移窗即失效）   */
static int    g_ppGeoOk = 0, g_ppGeoIdx = -1;  /* 几何缓存：有效 / 对应的插入点序号 */
static int    g_ppGeoX = 0, g_ppGeoY = 0, g_ppGeoH = 0;
static ULONGLONG g_ppGeoUntil = 0;             /* 几何缓存到期时刻                 */
static int    g_ppGeoRectOk = 0;
static RECT   g_ppGeoRect;                     /* 几何缓存对应的窗口矩形            */

/* 两个矩形是否相同（判断窗口有没有动） */
static int RectSame(const RECT* a, const RECT* b) {
    return a->left == b->left && a->top == b->top &&
           a->right == b->right && a->bottom == b->bottom;
}

/* Characters(下标, 1)：实参倒序（a[1]=下标、a[0]=长度），返回该字符的紧框范围 */
static IDispatch* WpsCharRange(IDispatch* all, long idx) {
    VARIANT a[2];
    VariantInit(&a[0]); VariantInit(&a[1]);
    a[1].vt = VT_I4; a[1].lVal = idx;
    a[0].vt = VT_I4; a[0].lVal = 1;
    DISPPARAMS dp;
    ZeroMemory(&dp, sizeof(dp));
    dp.rgvarg = a;
    dp.cArgs = 2;
    VARIANT r; VariantInit(&r);
    EXCEPINFO ei; ZeroMemory(&ei, sizeof(ei));
    UINT ae = 0;
    HRESULT hr = all->Invoke(g_ppIdChars, IID_NULL, LOCALE_USER_DEFAULT,
                             DISPATCH_METHOD, &dp, &r, &ei, &ae);
    if (ei.bstrDescription) SysFreeString(ei.bstrDescription);
    if (ei.bstrSource) SysFreeString(ei.bstrSource);
    if (FAILED(hr) || r.vt != VT_DISPATCH || !r.pdispVal) { VariantClear(&r); return NULL; }
    return r.pdispVal;
}

/* 标定点→像素的线性映射：原点各取一次 PTS(0)，比例用 1000 点的跨度算。
   ★ 别用 PTS(1)-PTS(0)：整数结果会把 1.53 截成 1（实测行高从 92px 变 60px）。 */
static int PpCalib(WpsChan* c, HWND fg) {
    long x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (!WpsPts(c->win, g_ppIdPtsX, 0, &x0) ||
        !WpsPts(c->win, g_ppIdPtsY, 0, &y0) ||
        !WpsPts(c->win, g_ppIdPtsX, 1000, &x1) ||
        !WpsPts(c->win, g_ppIdPtsY, 1000, &y1)) {
        LogChFail(c->tag, L"PointsToScreenPixelsX/Y failed", 0);
        WpsFail(c, GetTickCount64());
        return 0;
    }
    double sx = (double)(x1 - x0) / 1000.0, sy = (double)(y1 - y0) / 1000.0;
    if (sx < 0.1 || sy < 0.1) {          /* 比例不可能这么小：取到的是坏值 */
        LogChFail(c->tag, L"PointsToScreenPixels returned a bogus scale", 0);
        WpsFail(c, GetTickCount64());
        return 0;
    }
    g_ppOx = x0; g_ppOy = y0; g_ppScaleX = sx; g_ppScaleY = sy;
    if (!GetWindowRect(fg, &g_ppCalibRect)) ZeroMemory(&g_ppCalibRect, sizeof(g_ppCalibRect));
    g_ppCalibAt = GetTickCount64();
    return 1;
}

/* 第一阶段：Window.Selection 与 Selection.Type 的 DISPID。
   ★ 必须与第二阶段分开：Selection.TextRange 只在**文本编辑态**才拿得到
   （不在编辑态时 Invoke 直接抛异常），而"是不是文本编辑态"恰恰要靠这里
   拿到的 Type 去问 —— 顺序反了就是"永远失败且查不出原因"。
   实测这段失败的成因（本次踩过）：在非编辑态解析 TextRange 失败 → 整条链
   静默掐断，日志只剩一句 "wpp: no caret rect"。 */
static int PpNav(WpsChan* c) {
    if (!c->win) {
        DISPID id = 0;
        if (!WpsDispId(c->app, L"ActiveWindow", &id)) {
            LogChFail(c->tag, L"no member Application.ActiveWindow", 0);
            goto fail;
        }
        c->win = WpsPropDisp(c->app, id);
        if (!c->win) { LogChFail(c->tag, L"Application.ActiveWindow failed", 0); goto fail; }
    }
    /* 演示的 Selection 挂在**窗口**上（文字挂在 Application 上） */
    if (!g_ppIdSel && !WpsDispId(c->win, L"Selection", &g_ppIdSel)) {
        LogChFail(c->tag, L"no member Window.Selection", 0);
        goto fail;
    }
    if (!c->sel) {
        c->sel = WpsPropDisp(c->win, g_ppIdSel);
        if (!c->sel) { LogChFail(c->tag, L"Window.Selection failed", 0); goto fail; }
    }
    if (!g_ppIdType && !WpsDispId(c->sel, L"Type", &g_ppIdType)) {
        LogChFail(c->tag, L"no member Selection.Type", 0);
        goto fail;
    }
    return 1;
fail:
    WpsFail(c, GetTickCount64());
    return 0;
}

/* 第二阶段：文本编辑态下才存在的那些名字。借一串临时对象把名字解析完，
   之后所有 DISPID 都非 0，这段只跑一次。
   ★ TextFrame.TextRange 必须**在 TextFrame 上重新解析名字**，不能复用
   Selection.TextRange 的 DISPID：实测两者不同（Selection 是 2010、TextFrame
   是 2004），拿 2010 去 Invoke TextFrame 会返回 hr=S_OK 但结果不是对象
   （不是 FAILED，所以按 hr 判会以为成功），这里踩过。 */
static int PpResolve(WpsChan* c) {
    if (g_ppIdStart && g_ppIdTfTR && g_ppIdLen && g_ppIdChars &&
        g_ppIdBL && g_ppIdBW && g_ppIdBT && g_ppIdBH) return 1;
    DISPID idTR = g_ppIdTR;
    if (!idTR && !WpsDispId(c->sel, L"TextRange", &idTR)) {
        LogChFail(c->tag, L"no member Selection.TextRange", 0);
        goto fail;
    }
    g_ppIdTR = idTR;
    IDispatch* tr = WpsPropDisp(c->sel, idTR);
    if (!tr) {   /* 非编辑态会走到这里 */
        LogChFail(c->tag, L"Selection.TextRange failed (not editing text?)", 0);
        goto fail;
    }
    DISPID idStart = 0, idParent = 0;
    if (!WpsDispId(tr, L"Start", &idStart) || !WpsDispId(tr, L"Parent", &idParent)) {
        tr->Release();
        LogChFail(c->tag, L"no member TextRange.Start/.Parent", 0);
        goto fail;
    }
    g_ppIdStart = idStart;
    g_ppIdParent = idParent;
    IDispatch* par = WpsPropDisp(tr, idParent);
    tr->Release();
    if (!par) { LogChFail(c->tag, L"TextRange.Parent failed", 0); goto fail; }
    DISPID idTfTR = 0;
    if (!WpsDispId(par, L"TextRange", &idTfTR)) {
        par->Release();
        LogChFail(c->tag, L"no member TextFrame.TextRange", 0);
        goto fail;
    }
    g_ppIdTfTR = idTfTR;
    IDispatch* all = WpsPropDisp(par, idTfTR);
    par->Release();
    if (!all) { LogChFail(c->tag, L"TextFrame.TextRange failed", 0); goto fail; }
    DISPID idLen = 0, idChars = 0, a = 0, b = 0, cc = 0, d = 0;
    BOOL okb = WpsDispId(all, L"Length", &idLen) && WpsDispId(all, L"Characters", &idChars)
            && WpsDispId(all, L"BoundLeft", &a) && WpsDispId(all, L"BoundWidth", &b)
            && WpsDispId(all, L"BoundTop", &cc) && WpsDispId(all, L"BoundHeight", &d);
    if (!okb) {
        all->Release();
        LogChFail(c->tag, L"no member TextRange.Length/Characters/Bound*", 0);
        goto fail;
    }
    g_ppIdLen = idLen; g_ppIdChars = idChars;
    g_ppIdBL = a; g_ppIdBW = b; g_ppIdBT = cc; g_ppIdBH = d;
    /* 空文本框没有字符框，行高只能拿字号顶（见 PpGeometry） */
    if (!g_ppIdSize) {
        if (WpsDispId(all, L"Font", &g_ppIdFont)) {
            IDispatch* f = WpsPropDisp(all, g_ppIdFont);
            if (f) { WpsDispId(f, L"Size", &g_ppIdSize); f->Release(); }
        }   /* 解析不到也不致命：只有空文本框才用得上 */
    }
    all->Release();
    if (!g_ppIdPtsX && !WpsDispId(c->win, L"PointsToScreenPixelsX", &g_ppIdPtsX)) {
        LogChFail(c->tag, L"no member Window.PointsToScreenPixelsX", 0);
        goto fail;
    }
    if (!g_ppIdPtsY && !WpsDispId(c->win, L"PointsToScreenPixelsY", &g_ppIdPtsY)) {
        LogChFail(c->tag, L"no member Window.PointsToScreenPixelsY", 0);
        goto fail;
    }
    return 1;
fail:
    WpsFail(c, GetTickCount64());
    return 0;
}

/* 全量：插入点序号 -> 屏幕像素。结果写进 g_ppGeo*。1=成功。 */
static int PpGeometry(WpsChan* c, HWND fg, double pos, const RECT* winRect) {
    ULONGLONG now = GetTickCount64();
    IDispatch* tr = WpsPropDisp(c->sel, g_ppIdTR);
    if (!tr) { LogChFail(c->tag, L"Selection.TextRange failed", 0); WpsFail(c, now); return 0; }
    IDispatch* par = WpsPropDisp(tr, g_ppIdParent);
    tr->Release();
    IDispatch* all = par ? WpsPropDisp(par, g_ppIdTfTR) : NULL;
    if (par) par->Release();
    if (!all) { LogChFail(c->tag, L"TextRange.Parent.TextRange failed", 0); WpsFail(c, now); return 0; }
    double n = 0;
    if (!WpsGetNum(all, g_ppIdLen, &n)) {
        all->Release();
        LogChFail(c->tag, L"text frame Length unavailable", 0);
        WpsFail(c, now);
        return 0;
    }
    long total = (long)n;
    /* 空文本框（还没敲字 / 占位符）没有字符框可测 —— 但插入点就在段落左缘，
       行高拿字号顶（实测 60pt → 92px，与有字时的字符框高 93.6 一致）。
       不能在这里 return -1：那会把整条链掐断，表现成"空文本框里圆点整个消失"。 */
    double bl = 0, bw = 0, bt = 0, bh = 0;
    BOOL ok = FALSE;
    if (total < 1) {
        ok = WpsGetNum(all, g_ppIdBL, &bl) && WpsGetNum(all, g_ppIdBT, &bt);
        if (ok) {
            /* ★ Size 的 DISPID 是在 Font 对象上解析出来的，必须回 Font 上取值；
               拿它去 Invoke TextRange 只会白白失败（这里踩过）。 */
            double sz = 0;
            if (g_ppIdSize && g_ppIdFont) {
                IDispatch* f = WpsPropDisp(all, g_ppIdFont);
                if (f) { WpsGetNum(f, g_ppIdSize, &sz); f->Release(); }
            }
            bh = (sz > 0) ? sz : 0;
            ok = bh > 0;
        }
        bw = 0;
    } else {
        long idx = (long)pos;
        if (idx < 1) idx = 1;
        if (idx > total) idx = total;
        IDispatch* ch = WpsCharRange(all, idx);
        if (ch) {
            ok = WpsGetNum(ch, g_ppIdBL, &bl) && WpsGetNum(ch, g_ppIdBW, &bw) &&
                 WpsGetNum(ch, g_ppIdBT, &bt) && WpsGetNum(ch, g_ppIdBH, &bh);
            ch->Release();
        }
    }
    all->Release();
    if (!ok) {
        /* 两条分支分开报：空框那条只要 BoundLeft/Top + Font.Size 三样，
           有字那条要整组字符框 —— 混用一句话会让排查时分不清是哪条死的。 */
        LogChFail(c->tag, total < 1 ? L"empty frame: BoundLeft/Top or Font.Size failed"
                                    : L"character Bound* failed", 0);
        WpsFail(c, now);
        return 0;
    }

    RECT rc;
    ZeroMemory(&rc, sizeof(rc));
    BOOL haveRect = (winRect != NULL);
    if (haveRect) rc = *winRect;
    if (!g_ppScaleX || now >= g_ppCalibAt || !RectSame(&rc, &g_ppCalibRect)) {
        if (!PpCalib(c, fg)) return 0;
    }
    /* 插入点在这字符**之前** → 取左缘；插入点在整段末尾（序号超出字符数）→
       取末字符的右缘 */
    double ptX = ((long)pos > total) ? (bl + bw) : bl;
    g_ppGeoX = (int)(g_ppOx + (long)(ptX * g_ppScaleX));
    g_ppGeoY = (int)(g_ppOy + (long)(bt * g_ppScaleY));
    g_ppGeoH = (int)(bh * g_ppScaleY);
    g_ppGeoOk = 1;
    g_ppGeoIdx = (long)pos;
    g_ppGeoUntil = now + PP_GEO_MS;
    g_ppGeoRectOk = haveRect;
    if (haveRect) g_ppGeoRect = rc;
    return 1;
}

/* 方法0b：WPS 演示的插入点。返回 1=拿到（屏幕像素），
   -1=不是"正在编辑文本"（选形状/无选区/放映）→ 掐断链路，别让 guiinfo 那个
   屏幕原点上的 1×1 假光标得逞；0=前台不是 WPS 演示。 */
int WpsCaretShow(CaretPos* out) {
    if (!g_cfg.wpsCom) return 0;
    HWND fg = NULL;
    if (!WpsIsForeground(&g_pp, &fg)) return 0;
    /* 演示没有"文档控件类名"可判（编辑与否焦点都是 mdiClass），
       编辑态由 Selection.Type 把关，见下面。 */
    int thr = WpsThrottled(&g_pp, out);
    if (thr) return thr;
    if (!WpsAttach(&g_pp) || !PpNav(&g_pp)) return WpsBridgeFallback(&g_pp, WPS_CH_PP, out);

    ULONGLONG now = GetTickCount64();
    double ty = -1;
    if (!WpsGetNum(g_pp.sel, g_ppIdType, &ty) || (long)ty != PP_SEL_TEXT) {
        LogChFail(g_pp.tag, L"Selection.Type is not ppSelectionText", 0);
        return -1;
    }
    /* Type 闸门过了才是"在编辑文本"，Selection.TextRange 这时才拿得到 */
    if (!PpResolve(&g_pp)) return -1;
    IDispatch* tr = WpsPropDisp(g_pp.sel, g_ppIdTR);
    if (!tr) {
        LogChFail(g_pp.tag, L"Selection.TextRange failed", 0);
        WpsFail(&g_pp, now);
        return -1;
    }
    double pos = -1;
    BOOL havePos = WpsGetNum(tr, g_ppIdStart, &pos);
    tr->Release();
    if (!havePos) {
        LogChFail(g_pp.tag, L"TextRange.Start failed", 0);
        WpsFail(&g_pp, now);
        return -1;
    }
    /* 插入点序号没变 + 窗口没动 + 缓存没过期 → 几何一定没变：省掉全量那条
      11 次 Invoke 的链，只剩上面这 3 次 */
    RECT rc;
    BOOL haveRect = GetWindowRect(fg, &rc) != 0;
    if (g_ppGeoOk && (long)pos == g_ppGeoIdx && now < g_ppGeoUntil &&
        haveRect && g_ppGeoRectOk && RectSame(&rc, &g_ppGeoRect)) {
        out->x = g_ppGeoX; out->y = g_ppGeoY; out->h = g_ppGeoH; out->w = 0;
        WpsStore(&g_pp, g_ppGeoX, g_ppGeoY, g_ppGeoH);
        return 1;
    }
    if (!PpGeometry(&g_pp, fg, pos, haveRect ? &rc : NULL)) return -1;
    out->x = g_ppGeoX; out->y = g_ppGeoY; out->h = g_ppGeoH; out->w = 0;
    WpsStore(&g_pp, g_ppGeoX, g_ppGeoY, g_ppGeoH);
    return 1;
}

/* ---------- 方法0c：WPS 表格 ---------- */
/* 表格（进程 et.exe、顶层窗口类 XLMAIN、工作簿窗口 EXCEL6——都是仿 MS Excel 的）
   本机实测（caretprobe，选区态与单元格编辑态各来了一遍）：
     · GetGUIThreadInfo   → hwndCaret = NULL
     · MSAA OBJID_CARET   → S_FALSE
     · UIA                → 焦点链（KxetEditBoxWindow → KxEtMdiArea → …）全无 TextPattern
   取位置（Excel 对象模型，点坐标 → 屏幕像素）：
     Application.ActiveCell       当前格（选区移动跟着变；格内编辑态停在编辑的格）
       .Left/.Top/.Width/.Height  点，相对**列 A 左缘 / 行 1 顶缘**（不是视口！
                                  滚动改变的是 PTS(0)，两者配合才得屏幕坐标）
     ActiveWindow.PointsToScreenPixelsX/Y(0)   网格原点（列 A 左缘）的屏幕像素
     ActiveWindow.Zoom            显示比例（百分比）
   ★ 换算比例**不能用 PTS(1000)-PTS(0)**：WPS 表格把它按 1:1 处理（实测返回
     103/1103，差正好 1000），完全没算 DPI 与 1pt=4/3px。真实比例实测 =
     2.333px/pt（175% DPI × 100% 缩放），即
       scale = GetDpiForWindow(前台)/96 × Zoom/100 × 4/3
     验算 K11：103 + 480×2.333 = 1223，截图量得 1225；L12 同样像素级对齐。
   ★ 单元格没有"插入点"概念：圆点放在当前格左缘、行高取格高（格内编辑态时
     光标就在格左缘，正好重合；方向键移格时圆点跟格走）。 */

static DISPID g_etIdCell = 0;                   /* Application.ActiveCell        */
static DISPID g_etIdL = 0, g_etIdT = 0;         /* Range.Left / Top（点）          */
static DISPID g_etIdH = 0;                      /* Range.Height（行高）            */
static DISPID g_etIdZoom = 0;                   /* Window.Zoom（百分比）           */
static DISPID g_etIdPtsX = 0, g_etIdPtsY = 0;   /* Window.PointsToScreenPixelsX/Y */

/* 方法0c：WPS 表格当前格的位置。返回 1=拿到（屏幕像素），-1=在表格里但拿不到
   （没有 ActiveCell 之类）→ 掐断链路；0=前台不是 WPS 表格。 */
int WpsCaretGrid(CaretPos* out) {
    if (!g_cfg.wpsCom) return 0;
    HWND fg = NULL;
    if (!WpsIsForeground(&g_et, &fg)) return 0;
    int thr = WpsThrottled(&g_et, out);
    if (thr) return thr;
    if (!WpsAttach(&g_et)) return WpsBridgeFallback(&g_et, WPS_CH_ET, out);

    ULONGLONG now = GetTickCount64();
    if (!g_et.win) {
        DISPID id = 0;
        if (!WpsDispId(g_et.app, L"ActiveWindow", &id)) {
            LogChFail(g_et.tag, L"no member Application.ActiveWindow", 0);
            WpsFail(&g_et, now);
            return -1;
        }
        g_et.win = WpsPropDisp(g_et.app, id);
        if (!g_et.win) {
            LogChFail(g_et.tag, L"Application.ActiveWindow failed", 0);
            WpsFail(&g_et, now);
            return -1;
        }
    }
    /* 一次性解析全部 DISPID（GetIDsOfNames 是跨进程调用，只做一遍） */
    if (!g_etIdCell) {
        int ok = WpsDispId(g_et.app, L"ActiveCell", &g_etIdCell);
        IDispatch* c0 = ok ? WpsPropDisp(g_et.app, g_etIdCell) : NULL;
        if (c0) {
            ok = WpsDispId(c0, L"Left", &g_etIdL) && WpsDispId(c0, L"Top", &g_etIdT) &&
                 WpsDispId(c0, L"Height", &g_etIdH);
            c0->Release();
        }
        if (ok) ok = WpsDispId(g_et.win, L"Zoom", &g_etIdZoom) &&
                     WpsDispId(g_et.win, L"PointsToScreenPixelsX", &g_etIdPtsX) &&
                     WpsDispId(g_et.win, L"PointsToScreenPixelsY", &g_etIdPtsY);
        if (!ok) {
            LogChFail(g_et.tag, L"ActiveCell/Zoom/PointsToScreenPixels resolve failed", 0);
            WpsFail(&g_et, now);
            return -1;
        }
    }
    IDispatch* cell = WpsPropDisp(g_et.app, g_etIdCell);
    if (!cell) {
        LogChFail(g_et.tag, L"Application.ActiveCell failed", 0);
        WpsFail(&g_et, now);
        return -1;
    }
    double L = 0, T = 0, H = 0, zoom = 0;
    BOOL ok = WpsGetNum(cell, g_etIdL, &L) && WpsGetNum(cell, g_etIdT, &T) &&
              WpsGetNum(cell, g_etIdH, &H);
    cell->Release();
    if (!ok) {
        LogChFail(g_et.tag, L"ActiveCell Left/Top/Height failed", 0);
        WpsFail(&g_et, now);
        return -1;
    }
    if (!WpsGetNum(g_et.win, g_etIdZoom, &zoom) || zoom <= 0) zoom = 100;
    long x0 = 0, y0 = 0;
    if (!WpsPts(g_et.win, g_etIdPtsX, 0, &x0) || !WpsPts(g_et.win, g_etIdPtsY, 0, &y0)) {
        LogChFail(g_et.tag, L"PointsToScreenPixelsX/Y(0) failed", 0);
        WpsFail(&g_et, now);
        return -1;
    }
    /* 比例 = DPI/96 × Zoom/100 × 4/3（见上：PTS 的跨度是 1:1，不能用） */
    UINT dpi = WindowDpi(fg);
    if (!dpi) dpi = MonitorDpi(fg);
    if (!dpi) dpi = 96;
    double scale = (double)dpi / 96.0 * (zoom / 100.0) * 4.0 / 3.0;
    long x = x0 + (long)(L * scale);
    long y = y0 + (long)(T * scale);
    long h = (long)(H * scale);
    WpsStore(&g_et, x, y, h);
    out->x = x; out->y = y; out->h = h; out->w = 0;
    return 1;
}
