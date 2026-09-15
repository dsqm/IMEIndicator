#include "ime_status.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ================= 配置模块（IMEStatus.ini，与 exe 同目录） =================
   INI 风格，UTF-8 编码；`;` 开头为注释，空行忽略，[段名] 切段。
   [General]   键值项（Key = Value）
   [Colors]    三/四状态颜色（#RRGGBB 或 #RRGGBBAA）+ 全局透明度
   [Overlay]   圆点尺寸与光标偏移、检测间隔
   [Ignore]    程序黑名单：前台是这些进程时完全不显示（规避游戏卡顿）
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
    long long n=0; int any=0;
    for (const char* p=v; *p; p++) {
        if (*p<'0'||*p>'9') break;
        n=n*10+(*p-'0'); any=1;
        if (n>100000000LL) return def;
    }
    if (!any) return def;
    if (n<lo) n=lo; if (n>hi) n=hi;
    return (int)n;
}

/* #RRGGBB 或 #RRGGBBAA -> 0x00RRGGBB + 返回 alpha(0..255) via *aout。
   解析失败返回默认 rgb，alpha 用默认 alpha。 */
static int HexV(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return 0;
}
static DWORD ParseColorA(const char* v, DWORD defRgb, DWORD defAlpha, DWORD* aout) {
    DWORD rgb=defRgb, alpha=defAlpha;
    if (v) {
        const char* p=v;
        while (*p==' '||*p=='\t') p++;
        if (*p=='#') p++;
        int len=0; while (p[len]>='0'&&p[len]<='9'||(p[len]>='A'&&p[len]<='F')||(p[len]>='a'&&p[len]<='f')) len++;
        if (len>=6) {
            rgb = ((DWORD)HexV(p[0])<<20)|((DWORD)HexV(p[1])<<16)|
                  ((DWORD)HexV(p[2])<<12)|((DWORD)HexV(p[3])<<8)|
                  ((DWORD)HexV(p[4])<<4)|((DWORD)HexV(p[5]));
            if (len>=8) alpha = (DWORD)(HexV(p[6])<<4)|(DWORD)HexV(p[7]);
        }
    }
    if (aout) *aout=alpha;
    return rgb;
}

/* ---------- 默认值 ---------- */
#define DEF_EN_RGB  0xFF8C00   /* 橘                                     */
#define DEF_CAPS_RGB 0xFF0000  /* 红                                     */
#define DEF_KBDEN_RGB 0x8B00FF /* 紫                                     */
#define DEF_ALPHA   190

static void SrcOfPath(WCHAR* out, size_t cap) {
    WCHAR exe[MAX_PATH]; DWORD n=GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR* slash=NULL;
    for (DWORD i=0;i<n;i++) if (exe[i]==L'\\') slash=&exe[i];
    if (slash) *slash=0;
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\IMEStatus.ini", exe);
}

#define ISAME(s,w) (_stricmp(s,w)==0)

void CfgLoad(ImeCfg* c) {
    WCHAR path[MAX_PATH]; SrcOfPath(path, MAX_PATH);
    int defAl=DEF_ALPHA;
    DWORD en=DEF_EN_RGB, caps=DEF_CAPS_RGB, kbden=DEF_KBDEN_RGB, cn=DEF_EN_RGB;
    c->size=8; c->offsetX=2; c->offsetY=4; c->pollMs=100; c->trackMs=15; c->showEn=1;

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
            "ShowWhenEnglish = 1     ; 英文态是否显示圆点（1 显示 / 0 隐藏）\n"
            ";\n"
            "[Colors]\n"
            "; 颜色格式：#RRGGBB 或 #RRGGBBAA（AA=十六进制不透明度，留空用下面 Alpha）\n"
            "En   = #FF8C00   ; 英文（默认橘色）\n"
            "Caps = #FF0000   ; 大写键 Caps Lock（默认红色）\n"
            "KbdEn= #8B00FF   ; 英文键盘布局（默认紫色）\n"
            "Cn   = #FF8C00   ; 中文输入（默认与英文同色；如需区分中文请改这里）\n"
            "Alpha= 190       ; 圆点全局不透明度 0..255（0=全透）\n"
            ";\n"
            "[Overlay]\n"
            "Size    = 8      ; 圆点直径（像素）\n"
            "OffsetX = 2      ; 相对光标左缘的水平偏移（+ 右）\n"
            "OffsetY = 4      ; 相对光标底缘的垂直偏移（+ 下）\n"
            ";\n"
            "[Ignore]\n"
            "; 前台程序命中名单时完全不显示（规避某些游戏/程序卡顿）。\n"
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
                                    else if (ISAME(kb,"showwhenenglish")) c->showEn = (vb[0]=='1');
                                } else if (sec==1 && eq) {
                                    *eq=0;
                                    WCHAR k[CFG_NAME_MAX], v[128];
                                    _snwprintf_s(k,CFG_NAME_MAX,_TRUNCATE,L"%s",line);
                                    _snwprintf_s(v,128,_TRUNCATE,L"%s",eq+1);
                                    TrimW(k); TrimW(v);
                                    char kb[CFG_NAME_MAX*4], vb[512];
                                    WideCharToMultiByte(CP_UTF8,0,k,-1,kb,(int)sizeof(kb),0,0);
                                    WideCharToMultiByte(CP_UTF8,0,v,-1,vb,(int)sizeof(vb),0,0);
                                    DWORD al=defAl;
                                    if (ISAME(kb,"en"))   en  =ParseColorA(vb,en,defAl,&al);
                                    else if (ISAME(kb,"caps")) caps=ParseColorA(vb,caps,defAl,&al);
                                    else if (ISAME(kb,"kbden"))kbden=ParseColorA(vb,kbden,defAl,&al);
                                    else if (ISAME(kb,"cn"))  cn  =ParseColorA(vb,cn,defAl,&al);
                                    else if (ISAME(kb,"alpha")) defAl=ParseIntA(vb,190,0,255);
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