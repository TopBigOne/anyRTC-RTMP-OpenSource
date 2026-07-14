//
// Created by liu on 2022/1/14.
//

#ifndef LIVEPLAYER_BITMAPUTIL_H
#define LIVEPLAYER_BITMAPUTIL_H

// jni.h：JNI 基础类型（JNIEnv、jobject、jbyteArray 等）的声明
#include <jni.h>
// libyuv.h：Google 开源的图像格式转换/缩放库，这里用到其中的 RGB565ToARGB、ARGBToABGR 两个像素格式转换函数
#include <libyuv.h>
// android/bitmap.h：NDK 提供的 Bitmap 访问接口（AndroidBitmapInfo、AndroidBitmap_getInfo/lockPixels/unlockPixels），
// 用于在 Native 层直接读取 Java 层 android.graphics.Bitmap 的像素数据，避免通过 JNI 数组拷贝
#include <android/bitmap.h>

namespace anyrtc {

    // ImageFormat：本 SDK 内部统一使用的图像格式枚举，
    // 用于描述采集/推流管线（例如虚拟摄像头功能，把一张静态图片当作视频帧推流）中图像数据的像素排布方式
    typedef enum ImageFormat {
        NONE = 0,
        NV21 = 1,       // Android Camera 常见的 YUV420SP 格式（YCrCb 顺序）
        I420 = 2,       // YUV420 Planar 格式，libyuv/WebRTC 内部常用的标准格式
        RGB565 = 3,     // 16 位色深的 RGB 格式，对应 Bitmap 的 ANDROID_BITMAP_FORMAT_RGB_565
        RGB24 = 4,    // ie. BGR888          // 24 位色深，注意实际字节顺序是 BGR
        ARGB_8888 = 5 // in libyuv is ABGR format  // 对应 Bitmap 的 ANDROID_BITMAP_FORMAT_RGBA_8888，
                      // 但在 libyuv 的命名习惯里，同样的内存字节序被称为 ABGR（关注的是字节在内存中的排列顺序而不是位域名称）
    } ImageFormat;

    // 将外部传入的整型格式值（例如从 Java 层传下来的常量）映射为上面的 ImageFormat 枚举
    ImageFormat getFormat(int formatValue);
}

// bitmapToRgba：JNI 导出函数，输入一个 Java 层的 android.graphics.Bitmap 对象（jbitmap），
// 输出一个 jbyteArray（Java byte[]），内容是该 Bitmap 转换后的 RGBA 原始像素数据，
// 供 native 推流/虚拟摄像头管线使用。具体实现见 BitmapUtil.cpp。
JNIEXPORT jbyteArray bitmapToRgba(JNIEnv *env, jobject jbitmap);

// RGB565ToRGBA：把 RGB_565（16 位）格式的像素数据转换为 RGBA（32 位，实际内存序为 ABGR，与 Java 端 RGBA_8888 的
// 字节序保持一致）格式。src_rgb_data 是源像素指针，width/height 是图像宽高，dst_rgba_data 是目标缓冲区（调用方分配好足够大小）。
// 返回 0 表示成功，非 0 表示 libyuv 转换失败。
int RGB565ToRGBA(const uint8_t *src_rgb_data, int width, int height, uint8_t *dst_rgba_data);

// bitmapToByteArray：通用的模板辅助函数，封装了「锁定 Bitmap 像素 -> 调用回调做格式相关的转换/拷贝 -> 解锁 Bitmap ->
// 释放临时缓冲区 -> 封装为 jbyteArray 返回」这一整套通用流程。
// 通过模板参数 Func（通常是一个 lambda）把「具体如何把源像素转换成目标数据」这部分与「JNI/NDK 的样板代码」解耦，
// 这样不同的调用方（比如 bitmapToRgba）只需要提供各自的转换逻辑即可复用同一套加锁/解锁/内存管理代码。
template<typename Func>
jbyteArray bitmapToByteArray(JNIEnv *env, jobject jbitmap, Func callback);


#endif //LIVEPLAYER_BITMAPUTIL_H
