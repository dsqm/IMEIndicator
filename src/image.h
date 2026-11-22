/* image.h —— 自定义图片形状：解码、缓存、等比缩放
 *
 * 悬浮窗与设置预览共用同一份实现，保证「预览看到的 = 实际看到的」。
 * 图片放在 <exe 目录>\shapes\ 下，配置里只存文件名。
 */
#ifndef IME_IMAGE_H
#define IME_IMAGE_H

#include <windows.h>

/* 导入上限：图标场景 512×512 就够，这里留足余量，同时挡住
   「一张巨图把内存/磁盘吃光」的情况。解码时还会再查一次尺寸，
   手动丢进 shapes\ 的超限文件同样会被拒绝。 */
#define IMG_MAX_FILE_BYTES (1024u * 1024u)        /* 单文件 1 MB */
#define IMG_MAX_EDGE       4096u                  /* 单边像素 */
#define IMG_MAX_PIXELS     16000000u              /* 总像素 1600 万（与 README 承诺一致） */

/* 一张已缩放的图片：预乘 BGRA，自上而下，w*h*4 字节。
   指针归模块所有，下一次 ImgGet 可能让它失效 —— 调用方要立刻用掉。 */
typedef struct {
    int         w, h;
    const BYTE* bgra;
} ImgView;

/* 取 shapes\<fileName> 按**长边** targetPx 等比缩放后的位图。
   解码结果与缩放结果都缓存：同一文件同一尺寸只算一次，每帧只做一次查表。
   返回 1 成功；0 失败，err 里写原因（可传 NULL）。 */
int  ImgGet(const WCHAR* fileName, int targetPx, ImgView* out,
            WCHAR* err, size_t errCap);

/* 把 view 按整体不透明度 alpha 拷进 dst（dst 必须是 view->w × view->h 的预乘 BGRA）。 */
void ImgBlit(BYTE* dst, const ImgView* v, int alpha);

/* 把 srcPath 复制进 shapes\，重名自动加序号；成功时 outName 是最终文件名。
   内容完全相同的文件直接复用已有名字，不产生副本。
   复制前先校验：文件不超过 IMG_MAX_FILE_BYTES、能解码、尺寸不超过
   IMG_MAX_EDGE / IMG_MAX_PIXELS。返回 1 成功；0 失败，err 里写原因（可传 NULL）。 */
int  ImgImport(const WCHAR* srcPath, WCHAR* outName, size_t outCap,
               WCHAR* err, size_t errCap);

/* 释放缓存（退出时调；不调也无妨，进程退出会回收）。 */
void ImgShutdown(void);

#endif
