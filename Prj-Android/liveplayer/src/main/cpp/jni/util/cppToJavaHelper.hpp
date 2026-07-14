//
//

#ifndef IMAGEUTILS_CPPTOJAVAHELPER_HPP
#define IMAGEUTILS_CPPTOJAVAHELPER_HPP

// jni.h：JNI 基础类型声明（JNIEnv、jobject、jbyteArray 等）
#include <jni.h>
// android/bitmap.h：NDK Bitmap 访问接口，本文件虽未直接调用其中的函数，
// 但 BitmapFormat 枚举语义上与 ANDROID_BITMAP_FORMAT_* 一一对应，故引入此头文件便于关联理解
#include <android/bitmap.h>

// BitmapFormat：BitmapUtil.cpp / BitmapUtil.h 中使用的“源像素格式”枚举，
// 用来在 bitmapToByteArray 的回调（Func callback）里区分当前 Bitmap 到底是哪种像素格式，
// 从而决定是直接内存拷贝（ARGB_8888）还是需要经过 libyuv 转换（RGB_565）。
// 注意这里的命名和数值只在本工具文件的调用范围内使用，和 NDK 的 ANDROID_BITMAP_FORMAT_* 常量不是同一套符号，
// 调用方（BitmapUtil.cpp）负责把 AndroidBitmapInfo.format 映射成这里的枚举值。
enum BitmapFormat {
    ARGB_8888, // 对应 Android 的 ANDROID_BITMAP_FORMAT_RGBA_8888（32 位，每像素 4 字节）
    RGB_565    // 对应 Android 的 ANDROID_BITMAP_FORMAT_RGB_565（16 位，每像素 2 字节）
};

// formatToStr：把 BitmapFormat 枚举转换成可读字符串，主要用于日志打印/调试，方便定位当前处理的是哪种像素格式。
const char *formatToStr(BitmapFormat format) {
    switch (format) {
        case RGB_565:
            return "RGB_565";
        case ARGB_8888:
        default:
            // 默认分支同时覆盖 ARGB_8888 以及理论上不应出现的其它枚举值，统一返回 "ARGB_8888" 兜底。
            return "ARGB_8888";
    }
}

// bitmapToByteArray 的前向声明（模板函数）：
// 真正的通用实现（加锁 -> 调用 callback 转换 -> 解锁 -> 打包成 jbyteArray）在 BitmapUtil.cpp / BitmapUtil.h 中，
// 这里单独声明是为了让本头文件的使用者（以及包含该头文件的其它编译单元）能够看到该模板函数的签名，
// Func 是一个可调用对象（通常是 lambda），签名约定为：
//   int callback(uint8_t **target_data, int width, int height, int *dataSize, uint8_t *pixel, BitmapFormat format)
template<typename Func>
jbyteArray bitmapToByteArray(JNIEnv *env, jobject jbitmap, Func callback);

#endif //IMAGEUTILS_CPPTOJAVAHELPER_HPP
