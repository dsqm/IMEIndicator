#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================= 极简 JSON =================
   只服务配置文件：对象/数组/字符串/数字/布尔/null。
   字符串在解析时直接转成 UTF-16（配置里有中文进程名、图片名），
   输出时再转回 UTF-8 并转义。 */

typedef struct {
    const char* p;
    const char* end;
    int depth;
} Jp;

#define JSON_MAX_DEPTH 32

static void SkipWs(Jp* j) {
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') j->p++;
        else break;
    }
}

static Jv* NewJv(JvType t) {
    Jv* v = (Jv*)calloc(1, sizeof(Jv));
    if (v) v->type = t;
    return v;
}

void JsonFree(Jv* v) {
    if (!v) return;
    if (v->s) free(v->s);
    for (int i = 0; i < v->count; i++) {
        if (v->items) JsonFree(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v);
}

/* 追加一个子值（对象还要追加键）。失败返回 0。 */
static int Push(Jv* parent, Jv* child, WCHAR* key) {
    Jv** ni = (Jv**)realloc(parent->items, sizeof(Jv*) * (size_t)(parent->count + 1));
    if (!ni) return 0;
    parent->items = ni;
    if (parent->type == JV_OBJ) {
        WCHAR** nk = (WCHAR**)realloc(parent->keys, sizeof(WCHAR*) * (size_t)(parent->count + 1));
        if (!nk) return 0;
        parent->keys = nk;
        parent->keys[parent->count] = key;
    }
    parent->items[parent->count] = child;
    parent->count++;
    return 1;
}

/* 解析一个 JSON 字符串字面量（j->p 指向开引号），返回 UTF-16 串。 */
static WCHAR* ParseString(Jp* j) {
    if (j->p >= j->end || *j->p != '"') return NULL;
    j->p++;
    /* 先量出 UTF-8 字节数（处理转义），再一次性转换 */
    const char* start = j->p;
    size_t cap = 64, n = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\') {
            if (j->p >= j->end) { free(buf); return NULL; }
            char e = *j->p++;
            switch (e) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                /* \uXXXX -> UTF-8；代理对（D800-DBFF + DC00-DFFF）合并 */
                if (j->end - j->p < 4) { free(buf); return NULL; }
                unsigned cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = j->p[k];
                    unsigned d;
                    if (h >= '0' && h <= '9') d = (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
                    else { free(buf); return NULL; }
                    cp = cp * 16 + d;
                }
                j->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && j->end - j->p >= 6 &&
                    j->p[0] == '\\' && j->p[1] == 'u') {
                    unsigned lo = 0;
                    int ok = 1;
                    for (int k = 0; k < 4; k++) {
                        char h = j->p[2 + k];
                        unsigned d;
                        if (h >= '0' && h <= '9') d = (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
                        else { ok = 0; break; }
                        lo = lo * 16 + d;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        j->p += 6;
                    }
                }
                /* 编码成 UTF-8 */
                char u8[4]; int un = 0;
                if (cp < 0x80) { u8[un++] = (char)cp; }
                else if (cp < 0x800) {
                    u8[un++] = (char)(0xC0 | (cp >> 6));
                    u8[un++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    u8[un++] = (char)(0xE0 | (cp >> 12));
                    u8[un++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    u8[un++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    u8[un++] = (char)(0xF0 | (cp >> 18));
                    u8[un++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    u8[un++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    u8[un++] = (char)(0x80 | (cp & 0x3F));
                }
                for (int k = 0; k < un; k++) {
                    if (n + 1 >= cap) {
                        cap *= 2;
                        char* nb = (char*)realloc(buf, cap);
                        if (!nb) { free(buf); return NULL; }
                        buf = nb;
                    }
                    buf[n++] = u8[k];
                }
                continue;
            }
            default: free(buf); return NULL;
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[n++] = c;
    }
    if (j->p >= j->end) { free(buf); return NULL; }   /* 没闭合 */
    j->p++;   /* 跳过收尾引号 */
    (void)start;

    int wl = MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, NULL, 0);
    WCHAR* out = (WCHAR*)malloc(((size_t)wl + 1) * sizeof(WCHAR));
    if (out) {
        if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, out, wl);
        out[wl] = 0;
    }
    free(buf);
    return out;
}

static Jv* ParseValue(Jp* j);

static Jv* ParseObject(Jp* j) {
    Jv* o = NewJv(JV_OBJ);
    if (!o) return NULL;
    j->p++;   /* { */
    SkipWs(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return o; }
    for (;;) {
        SkipWs(j);
        WCHAR* key = ParseString(j);
        if (!key) { JsonFree(o); return NULL; }
        SkipWs(j);
        if (j->p >= j->end || *j->p != ':') { free(key); JsonFree(o); return NULL; }
        j->p++;
        Jv* v = ParseValue(j);
        if (!v) { free(key); JsonFree(o); return NULL; }
        if (!Push(o, v, key)) { free(key); JsonFree(v); JsonFree(o); return NULL; }
        SkipWs(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == '}') { j->p++; return o; }
        JsonFree(o); return NULL;
    }
}

static Jv* ParseArray(Jp* j) {
    Jv* a = NewJv(JV_ARR);
    if (!a) return NULL;
    j->p++;   /* [ */
    SkipWs(j);
    if (j->p < j->end && *j->p == ']') { j->p++; return a; }
    for (;;) {
        Jv* v = ParseValue(j);
        if (!v) { JsonFree(a); return NULL; }
        if (!Push(a, v, NULL)) { JsonFree(v); JsonFree(a); return NULL; }
        SkipWs(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == ']') { j->p++; return a; }
        JsonFree(a); return NULL;
    }
}

static Jv* ParseValue(Jp* j) {
    if (j->depth >= JSON_MAX_DEPTH) return NULL;
    SkipWs(j);
    if (j->p >= j->end) return NULL;
    char c = *j->p;
    if (c == '{') { j->depth++; Jv* v = ParseObject(j); j->depth--; return v; }
    if (c == '[') { j->depth++; Jv* v = ParseArray(j); j->depth--; return v; }
    if (c == '"') {
        WCHAR* s = ParseString(j);
        if (!s) return NULL;
        Jv* v = NewJv(JV_STR);
        if (!v) { free(s); return NULL; }
        v->s = s;
        return v;
    }
    if (!strncmp(j->p, "true", 4))  { j->p += 4; Jv* v = NewJv(JV_BOOL); if (v) v->b = 1; return v; }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; Jv* v = NewJv(JV_BOOL); if (v) v->b = 0; return v; }
    if (!strncmp(j->p, "null", 4))  { j->p += 4; return NewJv(JV_NULL); }
    /* 数字 */
    const char* s = j->p;
    if (*j->p == '-' || *j->p == '+') j->p++;
    int any = 0;
    while (j->p < j->end && ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' ||
                             *j->p == 'e' || *j->p == 'E' || *j->p == '-' || *j->p == '+')) {
        j->p++; any = 1;
    }
    if (!any) return NULL;
    char tmp[64];
    size_t n = (size_t)(j->p - s);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, s, n); tmp[n] = 0;
    Jv* v = NewJv(JV_NUM);
    if (v) v->n = atof(tmp);
    return v;
}

Jv* JsonParse(const char* utf8, int len) {
    if (!utf8) return NULL;
    if (len < 0) len = (int)strlen(utf8);
    /* 跳过 UTF-8 BOM */
    if (len >= 3 && (unsigned char)utf8[0] == 0xEF &&
        (unsigned char)utf8[1] == 0xBB && (unsigned char)utf8[2] == 0xBF) {
        utf8 += 3; len -= 3;
    }
    Jp j;
    j.p = utf8;
    j.end = utf8 + len;
    j.depth = 0;
    Jv* v = ParseValue(&j);
    if (!v) return NULL;
    SkipWs(&j);
    if (j.p != j.end) { JsonFree(v); return NULL; }   /* 尾部有垃圾 */
    return v;
}

Jv* JsonGet(const Jv* obj, const WCHAR* key) {
    if (!obj || obj->type != JV_OBJ || !key) return NULL;
    for (int i = 0; i < obj->count; i++)
        if (obj->keys[i] && _wcsicmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

int JsonInt(const Jv* obj, const WCHAR* key, int def) {
    Jv* v = JsonGet(obj, key);
    if (!v) return def;
    if (v->type == JV_NUM) return (int)v->n;
    if (v->type == JV_BOOL) return v->b;
    return def;
}

int JsonBool(const Jv* obj, const WCHAR* key, int def) {
    Jv* v = JsonGet(obj, key);
    if (!v) return def;
    if (v->type == JV_BOOL) return v->b;
    if (v->type == JV_NUM) return v->n != 0;
    return def;
}

const WCHAR* JsonStr(const Jv* obj, const WCHAR* key) {
    Jv* v = JsonGet(obj, key);
    return (v && v->type == JV_STR) ? v->s : NULL;
}

/* ================= 输出 ================= */

void JwInit(Jw* w) { w->p = NULL; w->len = w->cap = 0; w->oom = 0; }
void JwFree(Jw* w) { free(w->p); JwInit(w); }

static void JwGrow(Jw* w, size_t need) {
    if (w->oom) return;
    if (w->len + need + 1 <= w->cap) return;
    size_t nc = w->cap ? w->cap : 512;
    while (nc < w->len + need + 1) nc *= 2;
    char* np = (char*)realloc(w->p, nc);
    if (!np) { w->oom = 1; return; }
    w->p = np; w->cap = nc;
}

void JwRaw(Jw* w, const char* s) {
    size_t n = strlen(s);
    JwGrow(w, n);
    if (w->oom) return;
    memcpy(w->p + w->len, s, n);
    w->len += n;
    w->p[w->len] = 0;
}

void JwStr(Jw* w, const WCHAR* s) {
    JwRaw(w, "\"");
    if (s) {
        int bl = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
        if (bl > 1) {
            char* b = (char*)malloc((size_t)bl);
            if (b) {
                WideCharToMultiByte(CP_UTF8, 0, s, -1, b, bl, NULL, NULL);
                for (int i = 0; i < bl - 1; i++) {
                    unsigned char c = (unsigned char)b[i];
                    char esc[8];
                    switch (c) {
                    case '"':  JwRaw(w, "\\\""); break;
                    case '\\': JwRaw(w, "\\\\"); break;
                    case '\n': JwRaw(w, "\\n");  break;
                    case '\r': JwRaw(w, "\\r");  break;
                    case '\t': JwRaw(w, "\\t");  break;
                    default:
                        if (c < 0x20) {
                            _snprintf_s(esc, sizeof(esc), _TRUNCATE, "\\u%04x", c);
                            JwRaw(w, esc);
                        } else {
                            JwGrow(w, 1);
                            if (!w->oom) { w->p[w->len++] = (char)c; w->p[w->len] = 0; }
                        }
                    }
                }
                free(b);
            }
        }
    }
    JwRaw(w, "\"");
}

void JwInt(Jw* w, int v) {
    char b[24];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%d", v);
    JwRaw(w, b);
}

void JwBool(Jw* w, int v) { JwRaw(w, v ? "true" : "false"); }

void JwKey(Jw* w, const WCHAR* k) {
    JwStr(w, k);
    JwRaw(w, ": ");
}
