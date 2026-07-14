//
// Created by liu on 2022/1/14.
//


#include <common.h>
#include "BitmapUtil.h"
#include "cppToJavaHelper.hpp"


// bitmapToRgba：对外的 JNI 入口，把一个 Java 层 Bitmap 转换成 RGBA 字节数组。
// 内部只负责“定义转换策略”（一个 lambda），真正的加锁/取像素/解锁等 NDK 样板代码全部交给
// 模板函数 bitmapToByteArray 去做，二者通过“回调”的方式协作，实现关注点分离。
jbyteArray bitmapToRgba(JNIEnv *env,jobject jbitmap) {
    // 提前判空：Java 层传入 null Bitmap 时直接返回 NULL，避免后续对空指针调用 NDK Bitmap API。
    // 注意 bitmapToByteArray 内部也会做同样的判空，这里属于双重防御，不影响逻辑。
    if (jbitmap == NULL) {
        return NULL;
    }
    // callback：真正的“像素格式相关”的转换逻辑，通过 lambda 传入 bitmapToByteArray 模板函数。
    // 参数说明：
    //   target_data - 输出参数，用于接收本次转换分配出来的目标缓冲区（RGBA，4 字节/像素）
    //   width/height - 图像宽高，用来计算目标缓冲区大小
    //   dataSize     - 输出参数，返回目标缓冲区的字节数
    //   pixel        - 输入参数，Bitmap 被锁定后得到的原始像素指针（源数据，尚未转换）
    //   format       - 由外层 bitmapToByteArray 根据 AndroidBitmapInfo.format 判断出的源像素格式
    // 返回值：0 表示转换成功，非 0 表示失败（外层会据此清理内存并返回 NULL）
    auto callback = [](uint8_t **target_data, int width, int height, int *dataSize, uint8_t *pixel,
                       BitmapFormat format) -> int {
        // 目标格式固定为 RGBA（4 字节/像素），先按宽高计算并分配目标缓冲区。
        // 该内存由本 lambda 用 new[] 分配，其生命周期/释放由外层 bitmapToByteArray 负责（delete[]）。
        *dataSize = width * height * 4;
        *target_data = new uint8_t[*dataSize];
        if (format == ARGB_8888) {
            // 源本身就是 32 位 RGBA_8888（在 libyuv 命名习惯里等同于 ABGR 字节序），
            // 与目标格式完全一致，因此直接整块内存拷贝，无需做任何像素转换。
            memcpy(*target_data, pixel, sizeof(uint8_t) * width * height * 4);
            return 0;
        } else if (format == RGB_565) {
            // 源是 16 位 RGB_565，需要经过颜色空间转换才能得到 32 位 RGBA，
            // 具体转换过程委托给下面的 RGB565ToRGBA（内部调用 libyuv 完成两步转换）。
            return RGB565ToRGBA(pixel, width, height, *target_data);
        } else {
            // 既不是 ARGB_8888 也不是 RGB_565，属于本函数不支持的格式，返回 -1 表示失败。
            return -1;
        }
    };

    // 把上面定义好的转换策略连同 Bitmap 一起交给通用模板函数处理，
    // bitmapToByteArray 负责 AndroidBitmap_getInfo/lockPixels/unlockPixels 的完整生命周期管理。
    return bitmapToByteArray(env, jbitmap, callback);
}

// RGB565ToRGBA：使用 libyuv 把 16 位 RGB565 源数据转换为 32 位 RGBA 目标数据。
// 之所以分两步调用 libyuv，是因为 libyuv 并没有直接提供 "RGB565 -> RGBA" 的一步到位的转换函数，
// 只能先转换到它的中间格式 ARGB，再从 ARGB 转换到目标字节序：
//   第一步 RGB565ToARGB：把 16 位 RGB565 转换为 32 位 ARGB（libyuv 内部标准的小端字节序 B,G,R,A）；
//   第二步 ARGBToABGR：在字节顺序层面把 ARGB 重新排列为 ABGR（即交换 R 通道和 B 通道），
//                       因为下游消费方（Java 侧 Bitmap.Config.ARGB_8888 / RGBA_8888 的内存字节序）
//                       期望的是 R,G,B,A 顺序，与 libyuv 默认的 ARGB(B,G,R,A) 字节序相反，
//                       所以必须再做一次通道交换才能得到调用方期望的 RGBA 数据。
// 这里第二步直接原地转换（src 和 dst 用了同一块 dst_rgba_data 缓冲区），libyuv 的 ARGBToABGR 支持原地操作。
int RGB565ToRGBA(const uint8_t *src_rgb_data, int width, int height, uint8_t *dst_rgba_data) {
    // stride（每行字节数）：RGB565 每像素 2 字节，故为 width * 2；ARGB/RGBA 每像素 4 字节，故为 width * 4。
    if (libyuv::RGB565ToARGB(src_rgb_data, width * 2, dst_rgba_data, width * 4, width, height) !=0) {
        // 第一步转换失败（例如内部参数校验不通过），直接返回 -1，不再执行第二步通道交换。
        return -1;
    }
    // 第二步：ARGB -> ABGR 通道重排，得到调用方期望的 RGBA 字节序。返回值直接透传给上层（0 成功，非 0 失败）。
    return libyuv::ARGBToABGR(dst_rgba_data, width * 4, dst_rgba_data, width * 4, width, height);
}

// bitmapToByteArray：通用模板函数，封装了“操作 Android Bitmap 的 NDK 样板流程”：
//   1) AndroidBitmap_getInfo   获取 Bitmap 的宽高、像素格式等元信息
//   2) AndroidBitmap_lockPixels 锁定像素内存，之后才能安全地读取像素数据指针
//   3) 调用外部传入的 callback，按具体像素格式做转换/拷贝，得到一份新分配的目标缓冲区
//   4) 无论 callback 成功与否，都必须 AndroidBitmap_unlockPixels 解锁，否则会导致 Bitmap
//      的像素内存一直处于锁定状态，后续 Java 层对该 Bitmap 的操作（比如重绘、回收）可能出错或死锁
//   5) 将目标缓冲区包装为 Java 的 jbyteArray 返回给调用方，并释放 native 侧临时分配的内存
template<typename Func>
jbyteArray bitmapToByteArray(JNIEnv *env, jobject jbitmap, Func callback) {
    // 判空保护：Bitmap 为 NULL 时直接返回，不进行任何 NDK Bitmap 调用。
    if (jbitmap == NULL) {
        return NULL;
    }
    AndroidBitmapInfo info;
    uint8_t *pixels;
    // 获取 Bitmap 的宽、高、stride、像素格式（存入 info）。
    // 注意：这里没有检查 AndroidBitmap_getInfo 的返回值（ANDROID_BITMAP_RESULT_SUCCESS 等），
    // 如果传入的不是合法 Bitmap 对象，getInfo 可能失败，info 中的内容将是未初始化/无效的，
    // 但代码并未据此提前返回，而是继续往下执行——这是一处潜在风险点（仅注释说明，不在本次改动中修复）。
    AndroidBitmap_getInfo(env, jbitmap, &info);
    // 锁定 Bitmap 像素内存，之后 pixels 才指向真正可读的像素数据首地址。
    // 同样地，这里也没有检查 AndroidBitmap_lockPixels 的返回值；如果锁定失败，
    // pixels 可能是未定义值，后续 callback 里对 pixels 的读取将是不安全的——同样只做注释标注，不修复。
    AndroidBitmap_lockPixels(env, jbitmap, reinterpret_cast<void **>(&pixels));

    uint8_t *target_data = NULL;
    int dataSize = 0;
    int code = -1;
    // 根据 Bitmap 的实际像素格式，选择对应的 BitmapFormat 枚举值传给 callback，
    // 由 callback（在 bitmapToRgba 中定义）决定具体如何处理这两种格式。
    if (info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) {
        code = callback(&target_data, info.width, info.height, &dataSize, pixels, ARGB_8888);
    } else if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
        code = callback(&target_data, info.width, info.height, &dataSize, pixels, RGB_565);
    }
    // 若 Bitmap 格式既不是 RGBA_8888 也不是 RGB_565，则 code 保持初始值 -1（未调用 callback），
    // 会走下面的失败分支，同样会执行解锁。

    if (code != 0) {
        // 失败路径：即使转换失败，也必须解锁像素内存，否则 Bitmap 会一直处于锁定状态，
        // 这是本函数对“无论成功失败都要成对调用 lock/unlock”原则的体现。
        AndroidBitmap_unlockPixels(env, jbitmap);
        // 如果 callback 内部已经分配了目标缓冲区（例如格式判断失败发生在分配之后），
        // 这里需要释放，避免内存泄漏。
        if (target_data != NULL) {
            delete[] target_data;
        }
        return NULL;
    }
    // 成功路径：把 native 侧的目标缓冲区数据拷贝进新建的 Java byte[]（jbyteArray），
    // 这一步会发生一次 JNI 层面的内存拷贝（native -> JVM 堆）。
    jbyteArray res = env->NewByteArray(dataSize);
    env->SetByteArrayRegion(res, 0, dataSize, reinterpret_cast<const jbyte *>(target_data));
    // 成功路径同样要解锁像素内存，与失败路径保持对称，确保 lock/unlock 成对出现。
    AndroidBitmap_unlockPixels(env, jbitmap);

    // 数据已经拷贝进 jbyteArray，native 侧的临时缓冲区可以释放了。
    if (target_data != NULL) {
        delete[] target_data;
    }

    return res;
}



