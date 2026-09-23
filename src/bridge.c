#include "ime_indicator.h"
#include "bridge.h"
#include <tlhelp32.h>   /* 枚举进程找 explorer.exe（借它的普通权限令牌） */
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>

/* ================= 降权桥：管理员进程的"普通权限代理" =================
   问题：**COM 的运行对象表（ROT）按完整性级别隔离**。以管理员运行时，
   GetActiveObject 对普通权限进程注册的对象恒返回 MK_E_UNAVAILABLE，
   WPS 文档开着也拿不到 → WPS 里不显示圆点。本程序常被以管理员启动
   （这样也能读到提权程序的光标），所以这类查询只能交给"普通权限的自己"去跑。
   方案与 CapsEnhance 同源（那边已实测定型，直接沿用）。

   为什么是"子进程建命名管道"：
     · CreateProcessWithTokenW **不支持句柄继承**（没有 bInheritHandles 参数，
       实测子进程拿到的是无效句柄）；
     · CreateProcessAsUser 能继承，但要 SeAssignPrimaryTokenPrivilege，管理员
       令牌里它是"未分配"（不是"未启用"），EnablePrivilege 也失败；
     · 父进程建管道的话对象带高完整性标签，"低写高"被强制完整性控制拦下；
     · 子进程建的管道带普通权限标签 —— 高完整性进程去连、去写是允许的，
       反向才被拦。正好是我们需要的方向。

   线程约定：父进程侧的四个函数（BrEnsure/BrSend/BrRecv/BrDrop）只在一个线程
   上用（本程序里是 caret.c 的查询线程）；BrShutdown 由主线程在退出时调。 */

#pragma comment(lib, "advapi32.lib")   /* OpenProcessToken / DuplicateTokenEx */

#define BR_IDLE_MS     120000  /* 子进程空闲多久自退（没人用时不占内存） */
#define BR_WAIT_MS     120     /* 父进程等一帧应答的上限（管道在本机，很宽裕） */
/* 等子进程把管道建起来的上限。降权启动要借 shell 令牌（LOGON_WITH_PROFILE，
   要加载用户配置），实测偶发要几百毫秒 —— 500ms 会漏，给到 2s。
   注意这段等待**不阻塞**：BrEnsure 每次只试一次 CreateFile，连不上就返回
   FALSE 让本轮放弃，下轮轮询再试（查询线程每 15~50ms 一轮，2s 足够兜住）。
   这么做是因为查询线程被长时间占住会让"光标查询超时"连击、触发 worker 重建。 */
#define BR_SPAWN_MS    2000
#define BR_RETRY_MS    1000    /* 拉起失败后的冷却（挡住每轮都拉进程） */
#define BR_PIPE_BUF    4096    /* 管道内核缓冲（负载很小：几十字节） */
#define BR_FRAME_MAX   65536   /* 单帧上限：防对端给个离谱长度 */

static volatile LONG s_brSeq = 0;    /* 管道名序号：避免与残留的旧子进程撞名 */
static HANDLE        s_brPipe = INVALID_HANDLE_VALUE;
static HANDLE        s_brProc = NULL;
static unsigned long s_brPendingPid = 0; /* 已拉起但还没连上的子进程：管道名参数 */
static unsigned long s_brPendingSeq = 0;
static ULONGLONG     s_brDeadline = 0;   /* 等子进程建管道的截止时刻 */
static ULONGLONG     s_brRetryAt = 0;    /* 拉起失败的冷却到期时刻 */
static volatile LONG s_brChild = 0;      /* 1 = 本进程是桥子进程 */

/* 桥的诊断**无条件**写日志：它一旦失败，父进程那边只剩"桥不可用"一句，
   原因全丢。（两个进程写各自的日志文件，不会互相干扰。） */
static void BrLog(const WCHAR* fmt, ...) {
    if (!g_logging) return;
    WCHAR line[512];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(line, 512, _TRUNCATE, fmt, ap);
    va_end(ap);
    DbgLog(L"%s%s", s_brChild ? L"[桥] " : L"  桥 ", line);
}

/* ---------- 命令行：--bridge <父pid> <序号> ----------
   按"整词"匹配，两个十进制参数都必须存在且能解析（pid 与序号缺一个就没法拼
   管道名，宁可让主程序走普通启动路径）。 */
BOOL BrParseChildSwitch(DWORD* pid, DWORD* seq) {
    const WCHAR* cmd = GetCommandLineW();
    if (!cmd) return FALSE;
    const WCHAR* flag = L"--bridge";
    size_t fl = wcslen(flag);
    for (const WCHAR* p = cmd; (p = wcsstr(p, flag)) != NULL; p += fl) {
        BOOL lb = (p == cmd) || (p[-1] == L' ' || p[-1] == L'\t');
        WCHAR c = p[fl];
        if (!lb || (c != 0 && c != L' ' && c != L'\t')) continue;
        const WCHAR* q = p + fl;
        while (*q == L' ' || *q == L'\t') q++;
        WCHAR* end = NULL;
        DWORD x = (DWORD)wcstoul(q, &end, 10);
        if (end == q) continue;
        while (*end == L' ' || *end == L'\t') end++;
        WCHAR* end2 = NULL;
        DWORD y = (DWORD)wcstoul(end, &end2, 10);
        if (end2 == end) continue;
        *pid = x; *seq = y;
        return TRUE;
    }
    return FALSE;
}

static void BrPipeName(unsigned long pid, unsigned long seq, WCHAR* out, size_t cap) {
    _snwprintf_s(out, cap, _TRUNCATE, L"\\\\.\\pipe\\IMEIndicatorBridge.%lu.%lu", pid, seq);
}

/* ---------- 阻塞式读写全部（对端在同一条管道上，短读要循环补齐） ---------- */
static BOOL BrWriteAll(HANDLE h, const void* buf, DWORD want) {
    const BYTE* p = (const BYTE*)buf;
    DWORD done = 0;
    while (done < want) {
        DWORD w = 0;
        if (!WriteFile(h, p + done, want - done, &w, NULL) || w == 0) return FALSE;
        done += w;
    }
    return TRUE;
}

static BOOL BrReadAll(HANDLE h, void* buf, DWORD want) {
    BYTE* p = (BYTE*)buf;
    DWORD done = 0;
    while (done < want) {
        DWORD got = 0;
        if (!ReadFile(h, p + done, want - done, &got, NULL) || got == 0) return FALSE;
        done += got;
    }
    return TRUE;
}

/* 带截止时间的读满：管道是**阻塞**的，直接 ReadFile 会一直等下去、超时成空话。
   所以先 PeekNamedPipe 看有多少，只读"确实已经到的"那部分。
   返回 FALSE 还有一种含义：管道断了（PeekNamedPipe 失败）。 */
static BOOL BrReadAllBy(HANDLE h, void* buf, DWORD want, ULONGLONG deadline) {
    BYTE* p = (BYTE*)buf;
    DWORD done = 0;
    while (done < want) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return FALSE;
        if (avail == 0) {
            if (GetTickCount64() >= deadline) return FALSE;
            Sleep(2);
            continue;
        }
        DWORD chunk = want - done;
        if (chunk > avail) chunk = avail;
        DWORD got = 0;
        if (!ReadFile(h, p + done, chunk, &got, NULL) || got == 0) return FALSE;
        done += got;
    }
    return TRUE;
}

/* ---------- 拿当前用户 shell 的普通权限主令牌 ----------
   explorer.exe 跑在 Medium IL、属于当前用户、且一定在同一会话里，是"降权到
   普通用户"最省事的令牌来源。拿不到就返回 NULL（那时子进程也是管理员，桥无效）。 */
static HANDLE BrShellToken(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return NULL;
    PROCESSENTRY32W pe; memset(&pe, 0, sizeof(pe)); pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0) continue;
            DWORD sid = 0;                     /* 跳过 session 0 的服务实例 */
            if (ProcessIdToSessionId(pe.th32ProcessID, &sid) && sid != 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!pid) return NULL;

    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return NULL;
    HANDLE tok = NULL, dup = NULL;
    if (OpenProcessToken(hp, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &tok)) {
        if (!DuplicateTokenEx(tok, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation,
                              TokenPrimary, &dup))
            dup = NULL;
    }
    if (tok) CloseHandle(tok);
    CloseHandle(hp);
    return dup;
}

static BOOL BrSpawn(unsigned long pid, unsigned long seq) {
    WCHAR exe[MAX_PATH] = L"";
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return FALSE;
    WCHAR cmd[MAX_PATH + 160];
    _snwprintf_s(cmd, MAX_PATH + 160, _TRUNCATE,
                 L"\"%s\" --bridge %lu %lu%s", exe, pid, seq,
                 g_logging ? L" --log" : L"");

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    BOOL ok = FALSE;
    if (ProcIsElevated()) {
        /* 管理员：必须借 shell 令牌，否则子进程同样是管理员，照样附不上。
           借不到/起不来就**干脆不起** —— 起个同样提权的子进程会让它也去拉
           自己的桥，进程一层层生下去。 */
        HANDLE tok = BrShellToken();
        if (tok) {
            ok = CreateProcessWithTokenW(tok, LOGON_WITH_PROFILE, NULL, cmd,
                                         CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &si, &pi);
            CloseHandle(tok);
        }
        if (!ok) {
            BrLog(L"借 shell 令牌启动降权子进程失败 err=%lu", (unsigned long)GetLastError());
            return FALSE;
        }
    } else {
        /* 本来就是普通权限：直接起，子进程同样是普通权限 */
        ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        if (!ok) BrLog(L"子进程启动失败 err=%lu", (unsigned long)GetLastError());
    }
    CloseHandle(pi.hThread);
    if (s_brProc) CloseHandle(s_brProc);
    s_brProc = pi.hProcess;
    BrLog(L"已拉起子进程 pid=%lu seq=%lu", (unsigned long)pi.dwProcessId, seq);
    return TRUE;
}

/* 单次连接尝试（管道还没建好时 CreateFile 立刻失败返回，不阻塞） */
static BOOL BrTryConnect(unsigned long pid, unsigned long seq) {
    WCHAR name[128];
    BrPipeName(pid, seq, name, 128);
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    s_brPipe = h;
    return TRUE;
}

/* 收摊：关管道、丢进程句柄、进冷却（下次别每轮都拉进程） */
static void BrAbandon(void) {
    if (s_brPipe != INVALID_HANDLE_VALUE) {
        CloseHandle(s_brPipe);                 /* 子进程读到断开会自己退 */
        s_brPipe = INVALID_HANDLE_VALUE;
    }
    if (s_brProc) {
        DWORD code = 0;
        if (GetExitCodeProcess(s_brProc, &code) && code != STILL_ACTIVE)
            BrLog(L"子进程已退出，退出码=%lu", (unsigned long)code);
        else
            TerminateProcess(s_brProc, 0);     /* 活着却没建管道：别留孤儿 */
        CloseHandle(s_brProc);
        s_brProc = NULL;
    }
    s_brRetryAt = GetTickCount64() + BR_RETRY_MS;
}

void BrDrop(void) { BrAbandon(); }

BOOL BrEnsure(void) {
    if (s_brPipe != INVALID_HANDLE_VALUE) return TRUE;
    ULONGLONG now = GetTickCount64();

    if (!s_brProc) {                       /* 还没拉：先拉起（本轮多半连不上） */
        if (now < s_brRetryAt) return FALSE;
        unsigned long pid = (unsigned long)GetCurrentProcessId();
        unsigned long seq = (unsigned long)InterlockedIncrement(&s_brSeq);
        if (!BrSpawn(pid, seq)) { s_brRetryAt = now + BR_RETRY_MS; return FALSE; }
        s_brPendingPid = pid; s_brPendingSeq = seq;
        s_brDeadline = now + BR_SPAWN_MS;
        BrLog(L"等子进程建管道（最多 %u ms）", (unsigned)BR_SPAWN_MS);
        return FALSE;                      /* 下一轮再来连 */
    }
    if (BrTryConnect(s_brPendingPid, s_brPendingSeq)) return TRUE;
    if (now >= s_brDeadline) {
        BrLog(L"子进程超时未建管道");
        BrAbandon();
        return FALSE;
    }
    return FALSE;
}

/* ---------- 串行化：一次"请求+应答"必须独占管道 ----------
   caret.c 的查询线程卡死时会被**换代重建**（CARET_RESTART_AFTER），旧线程可能
   还活着一小会儿；两条线程同时写管道会把帧边界错开，之后每帧都对不上 → 桥
   表现为永久失效。所以"发+收"整体加锁（临界区只在管道已建好后才用得上，
   开销可忽略）。 */
static INIT_ONCE          s_brOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION   s_brCs;

static BOOL CALLBACK BrInitCs(PINIT_ONCE o, PVOID p, PVOID* ctx) {
    (void)o; (void)p; (void)ctx;
    InitializeCriticalSection(&s_brCs);
    return TRUE;
}

/* 帧格式：[LONG len][len 字节负载]。len < 0 = 对端处理失败（-2 = 收工）。
   长度前缀让"一次请求/一次应答"的边界明确，不必约定负载内部布局 ——
   协议内容完全由调用方（wps.c）定义。 */
static BOOL BrSend(const BYTE* buf, DWORD n) {
    if (s_brPipe == INVALID_HANDLE_VALUE) return FALSE;
    LONG len = (LONG)n;
    return BrWriteAll(s_brPipe, &len, sizeof(len)) &&
           (n == 0 || BrWriteAll(s_brPipe, buf, n));
}

static BOOL BrRecv(BYTE** buf, DWORD* n, DWORD timeoutMs) {
    *buf = NULL;
    *n = 0;
    if (s_brPipe == INVALID_HANDLE_VALUE) return FALSE;
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    LONG len = 0;
    if (!BrReadAllBy(s_brPipe, &len, sizeof(len), deadline)) return FALSE;
    if (len < 0) return FALSE;                              /* 对端报告失败 */
    if (len == 0) return TRUE;                              /* 空应答 */
    if (len > BR_FRAME_MAX) { BrLog(L"应答长度异常 %ld", (long)len); return FALSE; }
    BYTE* b = (BYTE*)malloc((size_t)len);
    if (!b) return FALSE;
    if (!BrReadAllBy(s_brPipe, b, (DWORD)len, deadline)) { free(b); return FALSE; }
    *buf = b; *n = (DWORD)len;
    return TRUE;
}

/* 一次完整的"问-答"：发请求、收应答（失败时由调用方 BrDrop）。
   整个来回在锁里做，避免两条线程把帧序搅乱（见上面的串行化说明）。 */
BOOL BrCall(const BYTE* req, DWORD reqLen, BYTE** resp, DWORD* respLen, DWORD timeoutMs) {
    *resp = NULL;
    *respLen = 0;
    InitOnceExecuteOnce(&s_brOnce, BrInitCs, NULL, NULL);
    EnterCriticalSection(&s_brCs);
    BOOL ok = BrSend(req, reqLen) && BrRecv(resp, respLen, timeoutMs);
    LeaveCriticalSection(&s_brCs);
    if (!ok && *resp) { free(*resp); *resp = NULL; }
    return ok;
}

void BrShutdown(void) {
    if (s_brPipe != INVALID_HANDLE_VALUE) {
        LONG q = -2;                       /* 约定的"停止"帧：子进程立刻收工 */
        BrWriteAll(s_brPipe, &q, sizeof(q));
        CloseHandle(s_brPipe);
        s_brPipe = INVALID_HANDLE_VALUE;
    }
    if (s_brProc) { CloseHandle(s_brProc); s_brProc = NULL; }
}

/* ---------- 子进程侧 ---------- */

/* 父进程监视线程：父进程一没，自己立刻退。
   没有它的话，若父进程在"已拉起子进程、还没连上管道"的窗口里死掉，子进程会
   永远阻塞在 ConnectNamedPipe 上变成孤儿（几 MB 常驻）。 */
static DWORD WINAPI BrWatchParent(LPVOID p) {
    HANDLE hp = (HANDLE)p;
    if (hp) {
        WaitForSingleObject(hp, INFINITE);
        ExitProcess(0);
    }
    return 0;
}

static void BrStartParentWatch(unsigned long pid) {
    HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!hp) {
        /* 打不开：父进程已经不在了（或没有权限）—— 直接收工，别留孤儿 */
        ExitProcess(0);
    }
    HANDLE th = CreateThread(NULL, 0, BrWatchParent, hp, 0, NULL);
    if (th) CloseHandle(th);
    else CloseHandle(hp);
}

/* 收一帧请求；idleMs 内没有任何数据就返回 FALSE（空闲自退）。
   PeekNamedPipe 失败同样返回 FALSE —— 那表示父进程已经走了。 */
static BOOL BrChildRecv(HANDLE np, BYTE** buf, DWORD* n, DWORD idleMs) {
    ULONGLONG deadline = GetTickCount64() + idleMs;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(np, NULL, 0, NULL, &avail, NULL)) return FALSE;
        if (avail > 0) break;
        if (GetTickCount64() >= deadline) return FALSE;
        Sleep(50);
    }
    LONG len = 0;
    if (!BrReadAll(np, &len, sizeof(len))) return FALSE;
    if (len == -2) return FALSE;                       /* 停止帧 */
    if (len < 0 || len > BR_FRAME_MAX) return FALSE;
    BYTE* b = NULL;
    if (len > 0) {
        b = (BYTE*)malloc((size_t)len);
        if (!b) return FALSE;
        if (!BrReadAll(np, b, (DWORD)len)) { free(b); return FALSE; }
    }
    *buf = b;
    *n = (DWORD)len;
    return TRUE;
}

int BrServe(unsigned long pid, unsigned long seq, BrHandler handler) {
    InterlockedExchange(&s_brChild, 1);
    BrLog(L"启动 pid=%lu seq=%lu", pid, seq);
    BrStartParentWatch(pid);       /* 注意：父进程已死时它不会返回 */

    WCHAR name[128];
    BrPipeName(pid, seq, name, 128);
    HANDLE np = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                 1, BR_PIPE_BUF, BR_PIPE_BUF, 0, NULL);
    if (np == INVALID_HANDLE_VALUE) {
        BrLog(L"CreateNamedPipe 失败 err=%lu", (unsigned long)GetLastError());
        return 2;
    }
    if (!ConnectNamedPipe(np, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
        BrLog(L"ConnectNamedPipe 失败 err=%lu", (unsigned long)GetLastError());
        CloseHandle(np);
        return 4;
    }
    BrLog(L"已连接，开始服务");

    for (;;) {
        BYTE* req = NULL;
        DWORD reqLen = 0;
        if (!BrChildRecv(np, &req, &reqLen, BR_IDLE_MS)) break;

        BYTE* resp = NULL;
        DWORD respLen = 0;
        BOOL ok = handler ? handler(req, reqLen, &resp, &respLen) : FALSE;
        free(req);

        if (!ok) {
            LONG neg = -1;                        /* 处理失败：父进程丢弃本次 */
            if (!BrWriteAll(np, &neg, sizeof(neg))) { free(resp); break; }
            free(resp);
            continue;
        }
        LONG len = (LONG)respLen;
        BOOL wrote = BrWriteAll(np, &len, sizeof(len)) &&
                     (respLen == 0 || BrWriteAll(np, resp, respLen));
        free(resp);
        if (!wrote) break;
    }
    FlushFileBuffers(np);
    DisconnectNamedPipe(np);
    CloseHandle(np);
    BrLog(L"服务结束");
    return 0;
}
