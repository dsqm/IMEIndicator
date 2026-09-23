/* bridge.h —— 降权桥：管理员进程的"普通权限代理"（实现在 bridge.c）
   只管运输与生命周期，不管协议内容（首个使用者是 wps.c 的 WPS 光标查询）。 */
#ifndef IME_BRIDGE_H
#define IME_BRIDGE_H

/* 子进程按 "--bridge <父pid> <序号>" 启动（见 bridge.c 的 BrParseChildSwitch）。
   ★ 父进程侧这五个函数**只在一个线程上用**（本程序里是 caret.c 的查询线程）。 */
BOOL BrParseChildSwitch(DWORD* pid, DWORD* seq);
BOOL BrEnsure(void);    /* 桥是否可用；必要时拉起子进程（不阻塞，见 bridge.c） */
void BrDrop(void);      /* 丢弃连接：对端答错/断了时调，下一轮会重新拉 */
BOOL BrCall(const BYTE* req, DWORD reqLen, BYTE** resp, DWORD* respLen,
            DWORD timeoutMs);   /* 一次"问-答"；成功则 *resp 需 free */
void BrShutdown(void);  /* 通知子进程收工（退出前调；子进程也会自己看父进程） */

/* 子进程的处理函数：吃一帧请求，吐一帧应答（返回 FALSE = 让父进程丢弃本次） */
typedef BOOL (*BrHandler)(const BYTE* req, DWORD reqLen, BYTE** resp, DWORD* respLen);
int  BrServe(DWORD pid, DWORD seq, BrHandler handler);   /* 子进程用：收工才返回 */

#endif /* IME_BRIDGE_H */
