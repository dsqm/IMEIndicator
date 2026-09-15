#include "ime_status.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ================= 配置模块（IMEStatus.ini，与 exe 同目录） =================
   INI 风格，UTF-8 编码；`;` 开头为注释，空行忽略，[段名] 切段。
   [General]   键值项（Key = Value）
   [Colors]    三/四状态颜色（#RRGGBB 或 #RRGGBBAA）+ 全局透明度
   [Overlay]   圆点尺寸与光标偏移、检测间隔
   [Ignore]    程序黑名单：前台是这些进程时跳过中英状态检测（规避游戏卡顿）
   文件不存在时写一份带注释的模板。颜色值只认前导 #RRGGBB 解析，
   解析失败回退默认。 */

#define CFG_MAX_ENTRIES 128
#define CFG_NAME_MAX    64

static WCHAR g_ignore[CFG_MAX_ENTRIES][CFG_NAME_MAX];
static int   g_ignoreCount = 0;

/* ---------- 小工具 ---------- */

static void TrimW(WCHAR* s) {
    size_t l = wcslen(s);
    while (l > 0 && (s[l-1]==L' '||s[l-1]==L'\t'||s[l-1]==L'\r')) s[--l]=0;
    WCHAR* p = s;
    while (*p==L' '||*p==L'\t') p++;
    if (p!=s) memmove(s, p, (wcslen(p)+1)*sizeof(WCHAR));
}

static void StripExeW(WCHAR* s) {
    size_t l = wcslen(s);
    if (l>=4 && _wcsicmp(s+l-4, L".exe")==0) s[l-4]=0;
}

static int ParseIntA(const char* v, int def, int lo, int hi) {
    if (!v||!v[0]) return def;
    const char* p=v;
    int neg=0;
    if (*p=='-') { neg=1; p++; }
    else if (*p=='+') p++;
    long long n=0; int any=0;
    for (; *p; p++) {
        if (*p<'0'||*p>'9') break;
        n=n*10+(*p-'0'); any=1;
        if (n>100000000LL) return def;
    }
    if (!any) return def;
    if (neg) n=-n;
    if (n<lo) n=lo; if (n>hi) n=hi;
    return (int)n;
}

/* 只认 #RRGGBB：透明度一律走 Alpha，颜色值不携带 AA。 */
static int HexV(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return 0;
}
/* 通用规则：色值写 0（可带行内注释）= 该状态不显示圆点。
   ★ 用 IME_COLOR_NONE 哨兵而不是 0 —— 0 是**黑色**，两者不能混。 */
static DWORD ParseColor6(const char* v, DWORD defRgb) {
    if (!v) return defRgb;
    while (*v==' '||*v=='\t') v++;
    if (v[0]=='0' && (v[1]==0 || v[1]==' ' || v[1]=='\t' || v[1]==';'))
        return IME_COLOR_NONE;
    if (*v!='#') return defRgb;
    v++;
    int len=0;
    while (v[len]>='0'&&v[len]<='9'||(v[len]>='A'&&v[len]<='F')||(v[len]>='a'&&v[len]<='f')) len++;
    if (len<6) return defRgb;
    return ((DWORD)HexV(v[0])<<20)|((DWORD)HexV(v[1])<<16)|
           ((DWORD)HexV(v[2])<<12)|((DWORD)HexV(v[3])<<8)|
           ((DWORD)HexV(v[4])<<4)|(DWORD)HexV(v[5]);
}

/* ---------- 默认值 ---------- */
#define DEF_EN_RGB   0xFF0000  /* 红                                     */
#define DEF_CAPS_RGB 0x0080FF  /* 蓝                                     */
#define DEF_KBDEN_RGB 0x8B00FF /* 紫                                     */
#define DEF_JP_RGB   0x000000  /* 黑                                     */
#define DEF_KR_RGB   0x000000  /* 黑                                     */
#define DEF_ALPHA   255

static void SrcOfPath(WCHAR* out, size_t cap) {
    WCHAR exe[MAX_PATH]; DWORD n=GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash=NULL;
    for (DWORD i=0;i<n;i++) if (exe[i]==L'\\') slash=&exe[i];
    if (slash) *slash=0;
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\IMEStatus.ini", exe);
}

#define ISAME(s,w) (_stricmp(s,w)==0)

/* 配置文件完整路径（托盘「打开配置」用） */
void CfgPath(WCHAR* out, size_t cap) { SrcOfPath(out, cap); }

void CfgLoad(ImeCfg* c) {
    WCHAR path[MAX_PATH]; SrcOfPath(path, MAX_PATH);
    int defAl=DEF_ALPHA;
    DWORD en=DEF_EN_RGB, caps=DEF_CAPS_RGB, kbden=DEF_KBDEN_RGB;
    DWORD jp=DEF_JP_RGB, kr=DEF_KR_RGB;
    DWORD cn=IME_COLOR_NONE;   /* Cn 默认不显示：中文态无圆点 */
    c->size=9; c->offsetX=2; c->offsetY=4; c->pollMs=100; c->trackMs=15;
    c->imeStrategy=0;

    FILE* f=NULL;
    if (_wfopen_s(&f, path, L"rb")!=0 || !f) {
        /* 文件不存在：写模板 */
        static const char kTpl[] =
            "; IMEStatus 配置文件（UTF-8，与 exe 同目录）\n"
            "; `;` 开头为注释（空行忽略）。改动后重启程序生效。\n"
            "[General]\n"
            ";\n"
            "PollIntervalMs = 100    ; 状态(中英/大写/黑名单)检测间隔(ms)\n"
            "TrackIntervalMs = 15    ; 光标坐标追踪间隔(ms)\n"
            "; 中英判定策略：0=自动学习(推荐) 1=只看 IME open 状态 2=只看转换模式\n"
            "; 自动学习会观察切换时哪个信号在变；个别输入法识别不准时可手动指定。\n"
            "ImeStrategy = 0\n"
            ";\n"
            "[Colors]\n"
            "; 颜色格式固定 #RRGGBB（6 位十六进制，必须带 #）；透明度一律用下面的 Alpha\n"
            "; 通用规则：色值写 0 表示该状态不显示圆点（0 不等于黑色，黑色写 #000000）\n"
            "En   = #FF0000   ; 英文（默认红色）\n"
            "Caps = #0080FF   ; 大写键 Caps Lock（默认蓝色）\n"
            "KbdEn= #8B00FF   ; 英文键盘布局（默认紫色）\n"
            "Jp   = #000000   ; 日文输入法（默认黑色）\n"
            "Kr   = #000000   ; 韩文输入法（默认黑色）\n"
            "Cn   = 0         ; 中文输入（0=不显示；设颜色值则中文态显示该色）\n"
            "Alpha= 255       ; 圆点全局不透明度 0..255（0=全透）\n"
            ";\n"
            "[Overlay]\n"
            "Size    = 9      ; 圆点直径（像素）\n"
            "OffsetX = 2      ; 相对光标左缘的水平偏移（+ 右）\n"
            "OffsetY = 4      ; 相对光标底缘的垂直偏移（+ 下）\n"
            ";\n"
            "[Ignore]\n"
            "; 前台程序命中名单时，跳过中英状态检测（检测本身可能卡顿，而非圆点显示）。\n"
            "; 每行一个进程名：完全匹配、大小写不敏感、.exe 后缀可写可不写。\n"
            "; 例：\n"
            "; somegame.exe\n";
        FILE* w=NULL;
        if (_wfopen_s(&w, path, L"wb")==0 && w) {
            /* 写 BOM + 模板，避免记事本按 ANSI 误读中文注释 */
            fputs("\xEF\xBB\xBF", w);
            fwrite(kTpl,1,sizeof(kTpl)-1,w);
            fclose(w);
        }
    } else {
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        if (sz>0 && sz<=64*1024) {
            char* raw=(char*)malloc((size_t)sz+1);
            if (raw) {
                size_t rd=fread(raw,1,(size_t)sz,f); raw[rd]=0;
                const char* p=raw;
                if (rd>=3 && (unsigned char)p[0]==0xEF&&(unsigned char)p[1]==0xBB&&(unsigned char)p[2]==0xBF) p+=3;
                int srcLen=(int)(rd-(size_t)(p-raw));
                int wl=MultiByteToWideChar(CP_UTF8,0,p,srcLen,NULL,0);
                if (wl>0) {
                    WCHAR* wide=(WCHAR*)malloc(((size_t)wl+1)*sizeof(WCHAR));
                    if (wide) {
                        MultiByteToWideChar(CP_UTF8,0,p,srcLen,wide,wl); wide[wl]=0;
                        int sec=-1; /* 0 General,1 Colors,2 Overlay,3 Ignore */
                        WCHAR* line=wide;
                        while (line) {
                            WCHAR* nl=wcschr(line,L'\n'); if(nl)*nl=0;
                            TrimW(line);
                            if (line[0]==L';'||!line[0]) { /* ignore */ }
                            else if (line[0]==L'[') {
                                WCHAR name[CFG_NAME_MAX];
                                _snwprintf_s(name,CFG_NAME_MAX,_TRUNCATE,L"%s",line+1);
                                WCHAR* rb=wcschr(name,L']'); if(rb)*rb=0; TrimW(name);
                                if      (_wcsicmp(name,L"general")==0) sec=0;
                                else if (_wcsicmp(name,L"colors")==0)  sec=1;
                                else if (_wcsicmp(name,L"overlay")==0) sec=2;
                                else if (_wcsicmp(name,L"ignore")==0)  sec=3;
                                else                                   sec=-1;
                            } else {
                                WCHAR* eq=wcschr(line,L'=');
                                if (sec==0 && eq) {
                                    *eq=0;
                                    WCHAR k[CFG_NAME_MAX], v[128];
                                    _snwprintf_s(k,CFG_NAME_MAX,_TRUNCATE,L"%s",line);
                                    _snwprintf_s(v,128,_TRUNCATE,L"%s",eq+1);
                                    TrimW(k); TrimW(v);
                                    char kb[CFG_NAME_MAX*4], vb[512];
                                    WideCharToMultiByte(CP_UTF8,0,k,-1,kb,(int)sizeof(kb),0,0);
                                    WideCharToMultiByte(CP_UTF8,0,v,-1,vb,(int)sizeof(vb),0,0);
                                    if (ISAME(kb,"pollintervalms"))   c->pollMs=ParseIntA(vb,100,5,60000);
                                    else if (ISAME(kb,"trackintervalms")) c->trackMs=ParseIntA(vb,15,5,200);
                                    else if (ISAME(kb,"imestrategy")) c->imeStrategy=ParseIntA(vb,0,0,2);
                                } else if (sec==1 && eq) {
                                    *eq=0;
                                    WCHAR k[CFG_NAME_MAX], v[128];
                                    _snwprintf_s(k,CFG_NAME_MAX,_TRUNCATE,L"%s",line);
                                    _snwprintf_s(v,128,_TRUNCATE,L"%s",eq+1);
                                    TrimW(k); TrimW(v);
                                    char kb[CFG_NAME_MAX*4], vb[512];
                                    WideCharToMultiByte(CP_UTF8,0,k,-1,kb,(int)sizeof(kb),0,0);
                                    WideCharToMultiByte(CP_UTF8,0,v,-1,vb,(int)sizeof(vb),0,0);
                                    if (ISAME(kb,"en"))    en   =ParseColor6(vb,en);
                                    else if (ISAME(kb,"caps"))  caps =ParseColor6(vb,caps);
                                    else if (ISAME(kb,"kbden")) kbden=ParseColor6(vb,kbden);
                                    else if (ISAME(kb,"cn"))    cn   =ParseColor6(vb,cn);
                                    else if (ISAME(kb,"jp"))    jp   =ParseColor6(vb,jp);
                                    else if (ISAME(kb,"kr"))    kr   =ParseColor6(vb,kr);
                                    else if (ISAME(kb,"alpha")) defAl=ParseIntA(vb,255,0,255);
                                } else if (sec==2 && eq) {
                                    *eq=0;
                                    WCHAR k[CFG_NAME_MAX], v[128];
                                    _snwprintf_s(k,CFG_NAME_MAX,_TRUNCATE,L"%s",line);
                                    _snwprintf_s(v,128,_TRUNCATE,L"%s",eq+1);
                                    TrimW(k); TrimW(v);
                                    char kb[CFG_NAME_MAX*4], vb[512];
                                    WideCharToMultiByte(CP_UTF8,0,k,-1,kb,(int)sizeof(kb),0,0);
                                    WideCharToMultiByte(CP_UTF8,0,v,-1,vb,(int)sizeof(vb),0,0);
                                    if (ISAME(kb,"size"))    c->size=ParseIntA(vb,8,1,64);
                                    else if (ISAME(kb,"offsetx")) c->offsetX=ParseIntA(vb,2,-512,512);
                                    else if (ISAME(kb,"offsety")) c->offsetY=ParseIntA(vb,4,-512,512);
                                } else if (sec==3 && !eq) {
                                    if (g_ignoreCount<CFG_MAX_ENTRIES) {
                                        WCHAR tmp[CFG_NAME_MAX];
                                        _snwprintf_s(tmp,CFG_NAME_MAX,_TRUNCATE,L"%s",line);
                                        TrimW(tmp); StripExeW(tmp);
                                        if (tmp[0]) { wcscpy_s(g_ignore[g_ignoreCount],CFG_NAME_MAX,tmp); g_ignoreCount++; }
                                    }
                                }
                            }
                            line = nl? nl+1 : NULL;
                        }
                        free(wide);
                    }
                }
                free(raw);
            }
        }
        fclose(f);
    }
    c->enrgb=en; c->capsrgb=caps; c->kbdEnrgb=kbden; c->cnrgb=cn;
    c->jprgb=jp; c->krrgb=kr;
    c->dotAlpha=defAl;
}

/* ---------- 前台进程名 ---------- */
static int GetFocusProcessName(HWND focus, WCHAR* out, size_t cap) {
    if (!focus||!out||cap==0) return 0;
    out[0]=0;
    DWORD pid=0; GetWindowThreadProcessId(focus,&pid);
    if (!pid) return 0;
    HANDLE hp=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!hp) return 0;
    WCHAR exe[MAX_PATH]; DWORD sz=MAX_PATH;
    BOOL ok=QueryFullProcessImageNameW(hp,0,exe,&sz);
    CloseHandle(hp);
    if (!ok||!sz) return 0;
    WCHAR* fname=exe;
    for (WCHAR* q=exe;*q;q++) if (*q==L'\\') fname=q+1;
    _snwprintf_s(out,cap,_TRUNCATE,L"%s",fname);
    return out[0]!=0;
}

/* 前台（焦点）程序是否命中 [Ignore] 黑名单 */
int CfgBlockedForeground(void) {
    if (g_ignoreCount==0) return 0;
    extern HWND ImeFocusedWindow(void);
    HWND w=ImeFocusedWindow();
    if (!w) return 0;
    WCHAR name[MAX_PATH];
    if (!GetFocusProcessName(w,name,MAX_PATH)) return 0;
    WCHAR n[CFG_NAME_MAX];
    _snwprintf_s(n,CFG_NAME_MAX,_TRUNCATE,L"%s",name);
    StripExeW(n);
    for (int i=0;i<g_ignoreCount;i++)
        if (_wcsicmp(n,g_ignore[i])==0) return 1;
    return 0;
}