//
// Created by liu on 2021/9/22.
//

#include "ArLivePushEvent.h"
// NativePushObserver_JNI.h：自动生成的 JNI 跳板（trampoline）头文件，
// 内含 Java_NativePushObserver_on* 系列函数，负责真正调用 Java 观察者对象上的方法
// （该文件由另一个 agent 单独注释，此处不修改/不重复注释其内部实现）
#include "NativePushObserver_JNI.h"
namespace anyrtc {
    // 构造函数：event 是 Java 层传入的推流观察者对象（局部引用）。
    // 需要把它转换为全局引用才能保存下来供后续异步回调使用
    LivePushEvent::LivePushEvent(jobject event):
            m_jJavaObj(NULL),
            m_jClass(NULL) {
        if (event){
            // 确保当前线程已附着到 JVM，拿到可用的 JNIEnv*
            JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
            // 将传入的局部引用（local ref）提升为全局引用（global ref）。
            // 局部引用只在当前 native 方法调用期间有效，一旦这次 JNI 调用返回，
            // 该 local ref 就可能被 JVM 回收；而 LivePushEvent 对象的生命周期
            // 会跨越整个推流会话，需要在任意时刻回调 Java 对象，
            // 因此必须用 NewGlobalRef 创建一个不会被自动回收、需手动释放的全局引用
            m_jJavaObj = jni->NewGlobalRef(event);
            // 获取该 Java 对象所属的 Class，并同样转换为全局引用保存，
            // 供后续 GetMethodID 等反射操作使用（Class 对象也需要跨调用持久有效）
            m_jClass = static_cast<jclass>(jni->NewGlobalRef(
                    jni->GetObjectClass(m_jJavaObj)));
        }
    }

    // 析构函数：对称地释放构造函数中创建的两个全局引用。
    // 如果不在这里调用 DeleteGlobalRef，这些全局引用会一直占用 JNI 引用表中的槽位，
    // 造成 JNI 全局引用泄漏（该 Java 对象也无法被 JVM 垃圾回收），
    // 因此全局引用的创建（NewGlobalRef）与释放（DeleteGlobalRef）必须成对出现
    LivePushEvent::~LivePushEvent() {
        if (m_jJavaObj){
            JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
            // 释放 Class 的全局引用
            jni->DeleteGlobalRef(m_jClass);
            // 释放观察者对象本身的全局引用
            jni->DeleteGlobalRef(m_jJavaObj);
            m_jClass = NULL;
            m_jJavaObj = NULL;
        }
    }
    // 错误回调实现：把 native 层的错误码/错误信息转发给 Java 层观察者
    void LivePushEvent::onError(int32_t code, const char* msg, void* extraInfo){
        // 确保当前线程（可能是推流引擎内部线程，而非 JVM 创建的线程）已附着到 JVM
        JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
        if (m_jJavaObj != NULL) {
            // JavaParamRef 是一个"不获取所有权、仅作为参数传递"的轻量包装类型，
            // 用已保存的全局引用 m_jJavaObj 构造，作为参数传给下面的自动生成跳板函数，
            // 跳板函数内部据此调用 m_jJavaObj 对应 Java 对象上的方法，
            // 但并不会尝试释放这个引用（其生命周期仍由 m_jJavaObj/析构函数管理）
            const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
            // 调用自动生成的 JNI 跳板函数：
            // NativeToJavaString 把 native 的 C 字符串 msg 转换为 Java 的 jstring 对象；
            // 这里对同一个 msg 转换了两次（分别对应跳板函数签名中的两个字符串参数），
            // 各自生成独立的 local ref 字符串对象
            Java_NativePushObserver_onError(jni, j_context, code, webrtc::NativeToJavaString(jni, msg),
                                            webrtc::NativeToJavaString(jni, msg));
        }
    }

    // 警告回调实现：逻辑与 onError 相同，只是转发到 onWarning 跳板函数
    void LivePushEvent::onWarning(int32_t code, const char* msg, void* extraInfo){
        JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
        if (m_jJavaObj != NULL) {
            const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
            Java_NativePushObserver_onWarning(jni, j_context, code, webrtc::NativeToJavaString(jni, msg),
                                            webrtc::NativeToJavaString(jni, msg));
        }
    }

    // 推流状态更新回调实现：把 ArLivePushStatus 枚举强转为 jint 传给 Java 层
    void LivePushEvent::onPushStatusUpdate(ArLivePushStatus state, const char* msg, void* extraInfo){
        JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
        if (m_jJavaObj != NULL) {
            const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
            Java_NativePushObserver_onPushStatusUpdate(jni, j_context,(jint)state, webrtc::NativeToJavaString(jni, msg),
                                              webrtc::NativeToJavaString(jni, msg));
        }
    }

    // 推流统计信息更新回调实现：把统计结构体各字段逐一强转为 jint 传给 Java 层
    void LivePushEvent::onStatisticsUpdate(ArLivePusherStatistics statistics){
        JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
        if (m_jJavaObj != NULL) {
            const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
            // 依次传递：应用 CPU 占用率、系统 CPU 占用率、视频宽、高、帧率、视频码率、音频码率
            Java_NativePushObserver_onStatisticsUpdate(jni, j_context,(jint)statistics.appCpu,
                                                       (jint)statistics.systemCpu,
                                                       (jint)statistics.width,(jint)statistics.height,(jint)statistics.fps,(jint)statistics.videoBitrate,(jint)statistics.audioBitrate);
        }
    }

    // 截图完成回调：当前是空实现，未转发给 Java 层（可能截图功能尚未接入或由其他路径处理）
    void LivePushEvent::onSnapshotComplete(const char* image, int length, int width, int height, ArLivePixelFormat format){

    }
    // 麦克风采集音量更新回调实现：直接把 volume 强转为 jint 转发给 Java 层
    void LivePushEvent::onMicrophoneVolumeUpdate(int32_t volume) {
        JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
        if (m_jJavaObj != NULL) {
            const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
            Java_NativePushObserver_onMicrophoneVolumeUpdate(jni, j_context,(jint)volume);
        }
    }
}
