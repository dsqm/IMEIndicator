/* image.c —— 自定义图片形状：WIC 解码 + 缓存 + 等比缩放
 *
 * 为什么用 WIC：系统自带，零外部依赖，PNG/ICO/BMP/JPEG 都能解。
 *
 * ★ WIC 的 ICO 解码器按「最小帧优先」返回：直接 GetFrame(0) 会把 256×256 的
 *   图标解成 16×16。必须遍历 GetFrameCount，按面积自己挑最大帧。
 * ★ 解码一次、缓存源位图；缩放结果也缓存。尺寸不变时每帧只做一次查表，
 *   追踪循环 15ms 一拍也不会产生解码开销。
 * ★ 源图长边上限 IMG_MAX_SRC：再大的图先让 WIC 缩到这个尺寸再转格式，
 *   避免一张 4K 图吃掉几十 MB 内存、解码几十毫秒。
 * ★ 缩放用面积平均（box filter）：预乘 alpha 是线性的，直接平均就是正确的
 *   合成结果；缩小大比例时也不会像双线性那样丢细节。
 */
#include "image.h"
#include "ime_indicator.h"
#include <wincodec.h>
#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#pragma comment(lib, "windowscodecs.lib")

#define IMG_MAX_SRC 512
#define IMG_CACHE   6

typedef struct {
    WCHAR     name[CFG_NAME_MAX];
    int       srcW, srcH;
    BYTE*     src;      /* 预乘 BGRA，长边 <= IMG_MAX_SRC */
    int       outW, outH;
    BYTE*     out;      /* 按 outFor 缩放后的结果 */
    int       outFor;   /* 缩放用的长边；0 = 还没缩过 */
    ULONGLONG used;
} Entry;

static Entry     g_e[IMG_CACHE];
static int       g_n = 0;
static ULONGLONG g_tick = 0;

static void SetErr(WCHAR* err, size_t cap, const WCHAR* fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(err, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void ShapesDir(WCHAR* out, size_t cap) {
    WCHAR dir[MAX_PATH];
    CfgDir(dir, MAX_PATH);
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\shapes", dir);
}

/* ---------- 解码 ---------- */

static int Decode(const WCHAR* path, int* ow, int* oh, BYTE** obits,
                  WCHAR* err, size_t cap) {
    IWICImagingFactory*    fac = NULL;
    IWICBitmapDecoder*     dec = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICBitmapScaler*      scaler = NULL;
    IWICFormatConverter*   conv = NULL;
    IWICBitmapSource*      src = NULL;
    BYTE* buf = NULL;
    UINT  count = 0, best = 0, bw = 0, bh = 0, tw = 0, th = 0;
    UINT  stride = 0, size = 0;
    int   ok = 0;
    HRESULT hr;

    *obits = NULL;

    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          IID_IWICImagingFactory, (void**)&fac);
    if (FAILED(hr)) {
        SetErr(err, cap, L"WIC 初始化失败 (0x%08X)", (unsigned)hr);
        return 0;
    }

    hr = fac->CreateDecoderFromFilename(path, NULL, GENERIC_READ,
                                        WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) {
        SetErr(err, cap, L"无法打开图片 (0x%08X)", (unsigned)hr);
        goto done;
    }

    if (FAILED(dec->GetFrameCount(&count)) || count == 0) {
        SetErr(err, cap, L"图片里没有可用的帧");
        goto done;
    }

    /* ICO 的多帧是不同尺寸：挑面积最大的那帧 */
    for (UINT i = 0; i < count; i++) {
        IWICBitmapFrameDecode* f = NULL;
        if (SUCCEEDED(dec->GetFrame(i, &f))) {
            UINT w = 0, h = 0;
            if (SUCCEEDED(f->GetSize(&w, &h)) &&
                (ULONGLONG)w * h > (ULONGLONG)bw * bh) {
                bw = w; bh = h; best = i;
            }
            f->Release();
        }
    }
    if (bw == 0 || bh == 0) {
        SetErr(err, cap, L"图片尺寸无效");
        goto done;
    }
    if (FAILED(dec->GetFrame(best, &frame))) {
        SetErr(err, cap, L"读取图片帧失败");
        goto done;
    }

    tw = bw; th = bh;
    if (bw > IMG_MAX_SRC || bh > IMG_MAX_SRC) {
        double s = (double)IMG_MAX_SRC / (double)(bw > bh ? bw : bh);
        tw = (UINT)(bw * s + 0.5); if (tw < 1) tw = 1;
        th = (UINT)(bh * s + 0.5); if (th < 1) th = 1;
    }

    src = frame;
    if (tw != bw || th != bh) {
        if (FAILED(fac->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant))) {
            SetErr(err, cap, L"图片缩放失败");
            goto done;
        }
        src = scaler;
    }

    if (FAILED(fac->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapDitherTypeNone, NULL, 0.0,
                                WICBitmapPaletteTypeCustom))) {
        SetErr(err, cap, L"图片格式转换失败");
        goto done;
    }

    stride = tw * 4;
    size = stride * th;
    buf = (BYTE*)malloc(size);
    if (!buf) {
        SetErr(err, cap, L"内存不足");
        goto done;
    }
    if (FAILED(conv->CopyPixels(NULL, stride, size, buf))) {
        SetErr(err, cap, L"读取像素失败");
        goto done;
    }

    *ow = (int)tw;
    *oh = (int)th;
    *obits = buf;
    buf = NULL;
    ok = 1;

done:
    free(buf);
    if (conv)   conv->Release();
    if (scaler) scaler->Release();
    if (frame)  frame->Release();
    if (dec)    dec->Release();
    if (fac)    fac->Release();
    return ok;
}

/* ---------- 缩放（面积平均） ---------- */

static void Scale(const BYTE* src, int sw, int sh, BYTE* dst, int dw, int dh) {
    const double rx = (double)sw / (double)dw;
    const double ry = (double)sh / (double)dh;

    for (int y = 0; y < dh; y++) {
        double y0 = y * ry, y1 = (y + 1) * ry;
        int iy0 = (int)floor(y0), iy1 = (int)ceil(y1);
        if (iy0 < 0) iy0 = 0;
        if (iy1 > sh) iy1 = sh;

        for (int x = 0; x < dw; x++) {
            double x0 = x * rx, x1 = (x + 1) * rx;
            int ix0 = (int)floor(x0), ix1 = (int)ceil(x1);
            if (ix0 < 0) ix0 = 0;
            if (ix1 > sw) ix1 = sw;

            double acc[4] = { 0, 0, 0, 0 };
            double wsum = 0;
            for (int iy = iy0; iy < iy1; iy++) {
                double wy = (y1 < iy + 1.0 ? y1 : iy + 1.0) -
                            (y0 > (double)iy ? y0 : (double)iy);
                if (wy <= 0) continue;
                for (int ix = ix0; ix < ix1; ix++) {
                    double wx = (x1 < ix + 1.0 ? x1 : ix + 1.0) -
                                (x0 > (double)ix ? x0 : (double)ix);
                    if (wx <= 0) continue;
                    double w = wx * wy;
                    const BYTE* p = src + ((size_t)iy * sw + ix) * 4;
                    acc[0] += p[0] * w;
                    acc[1] += p[1] * w;
                    acc[2] += p[2] * w;
                    acc[3] += p[3] * w;
                    wsum += w;
                }
            }

            BYTE* q = dst + ((size_t)y * dw + x) * 4;
            if (wsum <= 0) {
                q[0] = q[1] = q[2] = q[3] = 0;
                continue;
            }
            for (int k = 0; k < 4; k++) {
                double v = acc[k] / wsum;
                q[k] = (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5));
            }
        }
    }
}

/* ---------- 缓存 ---------- */

static Entry* FindEntry(const WCHAR* name) {
    for (int i = 0; i < g_n; i++)
        if (_wcsicmp(g_e[i].name, name) == 0) return &g_e[i];
    return NULL;
}

static void FreeEntry(Entry* e) {
    free(e->src);
    free(e->out);
    ZeroMemory(e, sizeof(*e));
}

static Entry* NewEntry(const WCHAR* name) {
    Entry* e;
    if (g_n < IMG_CACHE) {
        e = &g_e[g_n++];
    } else {
        e = &g_e[0];
        for (int i = 1; i < IMG_CACHE; i++)
            if (g_e[i].used < e->used) e = &g_e[i];
        FreeEntry(e);
    }
    lstrcpynW(e->name, name, CFG_NAME_MAX);
    return e;
}

static Entry* LoadEntry(const WCHAR* name, WCHAR* err, size_t cap) {
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    ShapesDir(dir, sizeof(dir) / sizeof(dir[0]));
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, name);

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        SetErr(err, cap, L"找不到 shapes\\%s", name);
        return NULL;
    }

    int w = 0, h = 0;
    BYTE* bits = NULL;
    if (!Decode(path, &w, &h, &bits, err, cap)) return NULL;

    Entry* e = NewEntry(name);
    e->srcW = w;
    e->srcH = h;
    e->src = bits;
    e->used = ++g_tick;
    return e;
}

/* ---------- 对外接口 ---------- */

int ImgGet(const WCHAR* name, int targetPx, ImgView* out, WCHAR* err, size_t cap) {
    if (err && cap) err[0] = 0;
    if (!out) return 0;
    if (!name || !name[0]) {
        SetErr(err, cap, L"未选择图片");
        return 0;
    }
    if (!CfgImageNameValid(name)) {
        SetErr(err, cap, L"图片名不合法");
        return 0;
    }
    if (targetPx < 1) targetPx = 1;
    if (targetPx > IMG_MAX_SRC) targetPx = IMG_MAX_SRC;

    Entry* e = FindEntry(name);
    if (!e) {
        e = LoadEntry(name, err, cap);
        if (!e) return 0;
    }
    e->used = ++g_tick;

    if (e->outFor != targetPx || !e->out) {
        int longEdge = e->srcW > e->srcH ? e->srcW : e->srcH;
        int dw, dh;
        if (e->srcW >= e->srcH) {
            dw = targetPx;
            dh = (int)((long long)e->srcH * targetPx / longEdge + 0.5);
        } else {
            dh = targetPx;
            dw = (int)((long long)e->srcW * targetPx / longEdge + 0.5);
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;

        BYTE* buf = (BYTE*)malloc((size_t)dw * dh * 4);
        if (!buf) {
            SetErr(err, cap, L"内存不足");
            return 0;
        }
        Scale(e->src, e->srcW, e->srcH, buf, dw, dh);
        free(e->out);
        e->out = buf;
        e->outW = dw;
        e->outH = dh;
        e->outFor = targetPx;
    }

    out->w = e->outW;
    out->h = e->outH;
    out->bgra = e->out;
    return 1;
}

void ImgBlit(BYTE* dst, const ImgView* v, int alpha) {
    if (!dst || !v || !v->bgra) return;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;

    size_t n = (size_t)v->w * v->h * 4;
    if (alpha == 255) {
        memcpy(dst, v->bgra, n);
        return;
    }
    for (size_t i = 0; i < n; i++)
        dst[i] = (BYTE)((v->bgra[i] * alpha + 127) / 255);
}

/* ---------- 导入 ---------- */

/* 拆成「不含扩展名的主名」+「含点的扩展名」 */
static void SplitName(const WCHAR* base, WCHAR* stem, size_t stemCap,
                      WCHAR* ext, size_t extCap) {
    const WCHAR* dot = NULL;
    for (const WCHAR* p = base; *p; p++) if (*p == L'.') dot = p;
    if (!dot || dot == base) {
        lstrcpynW(stem, base, (int)stemCap);
        ext[0] = 0;
        return;
    }
    size_t n = (size_t)(dot - base);
    if (n >= stemCap) n = stemCap - 1;
    memcpy(stem, base, n * sizeof(WCHAR));
    stem[n] = 0;
    lstrcpynW(ext, dot, (int)extCap);
}

/* 两个文件内容是否完全相同（先比大小，再分块比） */
static int SameFile(const WCHAR* a, const WCHAR* b) {
    HANDLE ha = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (ha == INVALID_HANDLE_VALUE) return 0;
    HANDLE hb = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hb == INVALID_HANDLE_VALUE) { CloseHandle(ha); return 0; }

    int same = 0;
    LARGE_INTEGER sa, sb;
    if (GetFileSizeEx(ha, &sa) && GetFileSizeEx(hb, &sb) &&
        sa.QuadPart == sb.QuadPart) {
        static BYTE ba[65536], bb[65536];
        same = 1;
        for (;;) {
            DWORD ra = 0, rb = 0;
            if (!ReadFile(ha, ba, sizeof(ba), &ra, NULL) ||
                !ReadFile(hb, bb, sizeof(bb), &rb, NULL) ||
                ra != rb || memcmp(ba, bb, ra) != 0) {
                same = 0;
                break;
            }
            if (ra == 0) break;
        }
    }
    CloseHandle(ha);
    CloseHandle(hb);
    return same;
}

int ImgImport(const WCHAR* srcPath, WCHAR* outName, size_t outCap) {
    if (!srcPath || !outName || outCap == 0) return 0;

    WCHAR dir[MAX_PATH];
    ShapesDir(dir, sizeof(dir) / sizeof(dir[0]));
    CreateDirectoryW(dir, NULL);          /* 已存在时返回 0，忽略 */

    const WCHAR* base = srcPath;
    for (const WCHAR* p = srcPath; *p; p++)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    if (!base[0]) return 0;

    WCHAR stem[CFG_NAME_MAX], ext[32];
    SplitName(base, stem, CFG_NAME_MAX, ext, 32);
    if (!stem[0]) return 0;

    for (int n = 1; n < 1000; n++) {
        WCHAR cand[CFG_NAME_MAX];
        if (n == 1)
            _snwprintf_s(cand, CFG_NAME_MAX, _TRUNCATE, L"%s%s", stem, ext);
        else
            _snwprintf_s(cand, CFG_NAME_MAX, _TRUNCATE, L"%s_%d%s", stem, n, ext);

        WCHAR dst[MAX_PATH];
        _snwprintf_s(dst, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, cand);

        if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES) {
            if (!CopyFileW(srcPath, dst, FALSE)) return 0;
            lstrcpynW(outName, cand, (int)outCap);
            return 1;
        }
        if (SameFile(srcPath, dst)) {      /* 同一张图重复导入：复用，不堆副本 */
            lstrcpynW(outName, cand, (int)outCap);
            return 1;
        }
    }
    return 0;
}

void ImgShutdown(void) {
    for (int i = 0; i < g_n; i++) FreeEntry(&g_e[i]);
    g_n = 0;
}
