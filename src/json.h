/* json.h —— 极简 JSON 读写（无依赖，只够配置文件用）
 *
 * 解析：UTF-8 输入 -> Jv 树（字符串转成 UTF-16 存）。
 * 输出：Jw 增长缓冲，逐段拼出带缩进的 JSON。
 *
 * 有意不做的事：数字精度（配置里只有整数）、注释、流式解析。
 * 遇到不认识的键由调用方忽略即可 —— 新版本加的字段老版本读不懂时不至于崩。
 */
#ifndef IME_JSON_H
#define IME_JSON_H

#include <windows.h>

typedef enum { JV_NULL = 0, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } JvType;

typedef struct Jv Jv;
struct Jv {
    JvType  type;
    int     b;        /* JV_BOOL */
    double  n;        /* JV_NUM  */
    WCHAR*  s;        /* JV_STR，NUL 结尾 */
    Jv**    items;    /* JV_ARR / JV_OBJ 的子值 */
    WCHAR** keys;     /* JV_OBJ 的键（与 items 等长） */
    int     count;
};

/* 解析失败返回 NULL。len 是字节数（不含结尾 NUL），传 -1 表示按 strlen 算。 */
Jv*  JsonParse(const char* utf8, int len);
void JsonFree(Jv* v);

Jv*  JsonGet(const Jv* obj, const WCHAR* key);
int  JsonInt(const Jv* obj, const WCHAR* key, int def);
int  JsonBool(const Jv* obj, const WCHAR* key, int def);
const WCHAR* JsonStr(const Jv* obj, const WCHAR* key);

/* ---- 输出 ---- */
typedef struct { char* p; size_t len, cap; int oom; } Jw;
void JwInit(Jw* w);
void JwFree(Jw* w);
void JwRaw(Jw* w, const char* s);
void JwStr(Jw* w, const WCHAR* s);   /* 带引号 + 转义 */
void JwInt(Jw* w, int v);
void JwBool(Jw* w, int v);
void JwKey(Jw* w, const WCHAR* k);   /* 输出 "key": */

#endif
