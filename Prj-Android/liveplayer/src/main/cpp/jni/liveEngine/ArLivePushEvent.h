//
// Created by liu on 2021/9/22.
//

#ifndef LIVEPLAYER_ARLIVEPUSHEVENT_H
#define LIVEPLAYER_ARLIVEPUSHEVENT_H
// webrtc 内部 jvm.h：提供 GetClass 等 JNI 工具函数
#include "webrtc/sdk/android/src/jni/jvm.h"
// scoped_java_ref.h：提供 ScopedJavaLocalRef/JavaParamRef 等 RAII 包装类型，
// 用于安全地管理 JNI 引用生命周期
#include "webrtc/sdk/android/native_api/jni/scoped_java_ref.h"
// java_types.h：提供 NativeToJavaString/JavaToStdString 等 Java<->native 类型转换工具函数
#include "webrtc/sdk/android/native_api/jni/java_types.h"
// IArLivePusher.hpp：定义推流相关的抽象接口 ArLivePusherObserver 及相关状态/统计结构体
#include "IArLivePusher.hpp"
// helpers_android.h：提供 AttachCurrentThreadIfNeeded 等辅助函数，
// 用于让当前 native 线程正确附着到 JVM
#include <webrtc/modules/utility/include/helpers_android.h>
namespace anyrtc {
    // LivePushEvent：native 层 ArLivePusherObserver 接口的具体实现，
    // 负责把推流器（Pusher）产生的各类回调事件转发给 Java 层的观察者对象。
    // 每个回调方法内部都会调用位于 NativePushObserver_JNI.h 中自动生成的
    // Java_NativePushObserver_on* 跳板（trampoline）函数完成真正的 JNI 调用
    class LivePushEvent : public ArLivePusherObserver {
    public:
        // 构造函数：传入 Java 层观察者对象（jobject），内部会为其创建 JNI 全局引用
        LivePushEvent(jobject event);

        // 析构函数：释放构造函数中创建的 JNI 全局引用
        ~LivePushEvent(void);

    public:
        // 错误回调：推流过程中发生不可恢复错误时触发
        void onError(int32_t code, const char* msg, void* extraInfo);

        // 警告回调：推流过程中发生可恢复的异常情况时触发
        void onWarning(int32_t code, const char* msg, void* extraInfo);

        // 推流状态变化回调（如连接中/已连接/断开等）
        void onPushStatusUpdate(ArLivePushStatus state, const char* msg, void* extraInfo);
        // 麦克风采集音量更新回调
        void onMicrophoneVolumeUpdate(int32_t volume);

        // 推流统计信息更新回调（CPU/码率/分辨率/帧率等）
        void onStatisticsUpdate(ArLivePusherStatistics statistics);
        // 截图完成回调（当前 .cpp 中为空实现，未转发给 Java 层）
        void onSnapshotComplete(const char* image, int length, int width, int height, ArLivePixelFormat format);
    protected:
        // 持有 Java 层观察者对象的全局引用（JNI Global Reference）。
        // 之所以需要 NewGlobalRef，是因为构造函数收到的 jobject 只是一个局部引用（local ref），
        // 其生命周期仅在创建它的那次 JNI 调用范围内有效；而这个观察者对象需要在
        // 后续任意时刻（推流过程中随时可能触发的回调）被使用，因此必须转为全局引用
        // 才能跨越多次 JNI 调用、长期持有而不被 JVM 回收或失效
        jobject m_jJavaObj;
        // 持有 Java 观察者对象所属 Class 的全局引用，用于 GetMethodID 等反射操作
        jclass m_jClass;
    };
}
#endif //LIVEPLAYER_ARLIVEPUSHEVENT_H
