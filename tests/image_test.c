/* image_test.c —— 图片形状：WIC 解码、等比缩放、导入去重
 *
 * 用 WIC 现场编码一张已知像素的 PNG，再走 ImgGet 解回来比对 ——
 * 不依赖仓库里带二进制测试素材，也不碰真实配置（全部在 bin\test\ 下）。
 */
#include "../src/image.h"
#include "../src/ime_indicator.h"
#include <wincodec.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "windowscodecs.lib")

static int g_fail = 0, g_pass = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; wprintf(L"  [FAIL] %s (line %d)\n", msg, __LINE__); } \
} while (0)

#define CHECK_INT(a, b, msg) do { \
    int _a = (int)(a), _b = (int)(b); \
    if (_a == _b) { g_pass++; } \
    else { g_fail++; wprintf(L"  [FAIL] %s: got %d, want %d (line %d)\n", msg, _a, _b, __LINE__); } \
} while (0)

/* ---------- 用 WIC 写一张 PNG ---------- */
static int WritePng(const WCHAR* path, int w, int h, const BYTE* bgra) {
    IWICImagingFactory*    fac = NULL;
    IWICStream*            stream = NULL;
    IWICBitmapEncoder*     enc = NULL;
    IWICBitmapFrameEncode* frame = NULL;
    IPropertyBag2*         props = NULL;
    HRESULT hr;
    int ok = 0;

    hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          IID_IWICImagingFactory, (void**)&fac);
    if (FAILED(hr)) goto done;
    if (FAILED(fac->CreateStream(&stream))) goto done;
    if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) goto done;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, NULL, &enc))) goto done;
    if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) goto done;
    if (FAILED(frame->Initialize(props))) goto done;
    if (FAILED(frame->SetSize(w, h))) goto done;
    {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt))) goto done;
    }
    if (FAILED(frame->WritePixels(h, w * 4, w * h * 4, (BYTE*)bgra))) goto done;
    if (FAILED(frame->Commit())) goto done;
    if (FAILED(enc->Commit())) goto done;
    ok = 1;

done:
    if (props)  props->Release();
    if (frame)  frame->Release();
    if (enc)    enc->Release();
    if (stream) stream->Release();
    if (fac)    fac->Release();
    return ok;
}

static void Fill(BYTE* buf, int w, int h, BYTE b, BYTE g, BYTE r, BYTE a) {
    for (int i = 0; i < w * h; i++) {
        buf[i * 4 + 0] = b;
        buf[i * 4 + 1] = g;
        buf[i * 4 + 2] = r;
        buf[i * 4 + 3] = a;
    }
}

static int PixelIs(const ImgView* v, int x, int y, int b, int g, int r, int a) {
    const BYTE* p = v->bgra + ((size_t)y * v->w + x) * 4;
    return p[0] == b && p[1] == g && p[2] == r && p[3] == a;
}

int main(void) {
    wprintf(L"=== image_test ===\n");

    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WCHAR dir[MAX_PATH], shapes[MAX_PATH], alt[MAX_PATH];
    CfgDir(dir, MAX_PATH);
    _snwprintf_s(shapes, MAX_PATH, _TRUNCATE, L"%s\\shapes", dir);
    _snwprintf_s(alt, MAX_PATH, _TRUNCATE, L"%s\\alt", dir);
    CreateDirectoryW(shapes, NULL);
    CreateDirectoryW(alt, NULL);

    /* 4×2：左两列红、右两列蓝 */
    BYTE src[4 * 2 * 4];
    Fill(src, 4, 2, 0, 0, 255, 255);
    for (int y = 0; y < 2; y++)
        for (int x = 2; x < 4; x++) {
            BYTE* p = src + (y * 4 + x) * 4;
            p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255;
        }

    WCHAR srcPath[MAX_PATH];
    _snwprintf_s(srcPath, MAX_PATH, _TRUNCATE, L"%s\\redblue.png", dir);
    CHECK(WritePng(srcPath, 4, 2, src), L"测试 PNG 写入成功");

    /* ---------- 1. 导入 ---------- */
    wprintf(L"[1] 导入与去重\n");
    WCHAR name[CFG_NAME_MAX], name2[CFG_NAME_MAX];
    CHECK(ImgImport(srcPath, name, CFG_NAME_MAX), L"首次导入成功");
    CHECK(wcscmp(name, L"redblue.png") == 0, L"首次导入保留原文件名");
    CHECK(ImgImport(srcPath, name2, CFG_NAME_MAX), L"重复导入成功");
    CHECK(wcscmp(name2, name) == 0, L"内容相同不产生副本");

    /* 同名不同内容 -> 加序号 */
    BYTE altPx[2 * 2 * 4];
    Fill(altPx, 2, 2, 0, 255, 0, 255);
    WCHAR altPath[MAX_PATH];
    _snwprintf_s(altPath, MAX_PATH, _TRUNCATE, L"%s\\redblue.png", alt);
    CHECK(WritePng(altPath, 2, 2, altPx), L"同名测试 PNG 写入成功");
    WCHAR name3[CFG_NAME_MAX];
    CHECK(ImgImport(altPath, name3, CFG_NAME_MAX), L"同名不同内容导入成功");
    CHECK(wcscmp(name3, L"redblue_2.png") == 0, L"重名自动加序号");

    /* ---------- 2. 解码与等比缩放 ---------- */
    wprintf(L"[2] 解码与等比缩放\n");
    ImgView v;
    WCHAR err[128];

    CHECK(ImgGet(L"redblue.png", 4, &v, err, 128), L"按长边 4 解码成功");
    CHECK_INT(v.w, 4, L"宽 = 长边");
    CHECK_INT(v.h, 2, L"高按原比例");
    CHECK(PixelIs(&v, 0, 0, 0, 0, 255, 255), L"左半是红");
    CHECK(PixelIs(&v, 3, 0, 255, 0, 0, 255), L"右半是蓝");

    CHECK(ImgGet(L"redblue.png", 2, &v, err, 128), L"按长边 2 解码成功");
    CHECK_INT(v.w, 2, L"缩小后宽 = 2");
    CHECK_INT(v.h, 1, L"缩小后高 = 1");
    CHECK(PixelIs(&v, 0, 0, 0, 0, 255, 255), L"缩小后左像素仍是红");
    CHECK(PixelIs(&v, 1, 0, 255, 0, 0, 255), L"缩小后右像素仍是蓝");

    CHECK(ImgGet(L"redblue.png", 8, &v, err, 128), L"按长边 8 解码成功");
    CHECK_INT(v.w, 8, L"放大后宽 = 8");
    CHECK_INT(v.h, 4, L"放大后高 = 4");
    CHECK(PixelIs(&v, 0, 0, 0, 0, 255, 255), L"放大后左边缘是红");
    CHECK(PixelIs(&v, 7, 3, 255, 0, 0, 255), L"放大后右边缘是蓝");

    CHECK(ImgGet(L"redblue_2.png", 2, &v, err, 128), L"第二张图解码成功");
    CHECK_INT(v.w, 2, L"第二张图宽 = 2");
    CHECK_INT(v.h, 2, L"第二张图高 = 2");
    CHECK(PixelIs(&v, 0, 0, 0, 255, 0, 255), L"第二张图是绿");

    /* ---------- 3. 失败路径 ---------- */
    wprintf(L"[3] 失败路径\n");
    CHECK(!ImgGet(L"missing.png", 4, &v, err, 128), L"文件不存在时返回 0");
    CHECK(err[0] != 0, L"失败时给出原因");
    CHECK(!ImgGet(L"..\\redblue.png", 4, &v, err, 128), L"带路径分隔符的名字被拒绝");
    CHECK(!ImgGet(L"", 4, &v, err, 128), L"空名字被拒绝");

    CHECK(!CfgImageNameValid(L"a\\b.png"), L"反斜杠不合法");
    CHECK(!CfgImageNameValid(L"a/b.png"), L"斜杠不合法");
    CHECK(!CfgImageNameValid(L"C:x.png"), L"盘符不合法");
    CHECK(!CfgImageNameValid(L".."), L".. 不合法");
    CHECK(CfgImageNameValid(L"icon.png"), L"普通文件名合法");

    /* ---------- 4. 整体透明度 ---------- */
    wprintf(L"[4] 整体透明度\n");
    CHECK(ImgGet(L"redblue.png", 4, &v, err, 128), L"取图成功");
    BYTE dst[4 * 2 * 4];
    memset(dst, 0xEE, sizeof(dst));
    ImgBlit(dst, &v, 128);
    CHECK(dst[0] == 0 && dst[1] == 0 && dst[2] == 128 && dst[3] == 128,
          L"alpha=128 时预乘通道减半");

    ImgShutdown();
    if (SUCCEEDED(co)) CoUninitialize();

    wprintf(L"\n%d passed, %d failed\n", g_pass, g_fail);
    if (g_fail) { wprintf(L"=== FAILED ===\n"); return 1; }
    wprintf(L"=== OK ===\n");
    return 0;
}
