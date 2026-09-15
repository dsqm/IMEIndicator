#include "ime_status.h"
#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>

/* ================= 调试日志 =================
   托盘「记录日志」开关把 g_logging 置 1 后，检测线程每次循环写一行。
   日志写到 exe 同目录 log\ 子目录，每个进程会话**新建一个**以时间命名的
   文件（log\IMEStatus-YYYYMMDD-HHMMSS.log），一次会话一个文件，
   不会无限追加变长。只有检测线程写文件，托盘切换只翻 g_logging 标志，
   避免跨线程文件句柄竞争。 */

volatile LONG g_logging = 0;

static WCHAR g_dir[MAX_PATH] = L"";
static FILE*  g_fp = NULL;

void DbgInit(void) {
    WCHAR exe[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (exe[i] == L'\\') slash = &exe[i];
    if (slash) {
        *slash = 0;
        _snwprintf_s(g_dir, MAX_PATH, _TRUNCATE, L"%s", exe);
    }
}

/* 打开本次会话的日志文件：<exe目录>\log\IMEStatus-<时间>.log。
   目录不存在就建；文件用 "w"（新建），一次会话一个、不会跨启动追加。 */
static void DebugOpen(void) {
    if (!g_dir[0]) return;
    WCHAR dir[MAX_PATH];
    _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\log", g_dir);
    CreateDirectoryW(dir, NULL);   /* 已存在则忽略 */
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE,
                 L"%s\\IMEStatus-%04u%02u%02u-%02u%02u%02u.log",
                 dir, st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    _wfopen_s(&g_fp, path, L"w, ccs=UTF-8");
}

/* 关闭日志文件（下次 DbgLog 若仍在开启会重开） */
static void DebugClose(void) {
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
}

void DbgLog(const WCHAR* fmt, ...) {
    if (!g_logging) return;
    if (!g_fp) DebugOpen();
    if (!g_fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR head[32];
    _snwprintf_s(head, 32, _TRUNCATE, L"[%02u:%02u:%02u.%03u] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fputws(head, g_fp);
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(g_fp, fmt, ap);
    va_end(ap);
    fputwc(L'\n', g_fp);
    fflush(g_fp);
}

void DbgShutdown(void) {
    DebugClose();
}

/* 托盘关闭日志时释放文件句柄（否则文件一直被占用，无法删除/改名）。
   由检测线程在 g_logging 下降沿调用，保证文件只被一个线程碰。 */
void DbgClose(void) {
    DebugClose();
}