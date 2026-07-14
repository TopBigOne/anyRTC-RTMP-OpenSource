//
// Created by liu on 2021/9/22.
//

#include "ArLivePlayEvent.h"
// NativePlayObserver_JNI.h：自动生成的 JNI 跳板（trampoline）头文件，
// 内含 Java_NativePlayObserver_on* 系列函数，用于把大部分简单回调转发给 Java 层
#include "NativePlayObserver_JNI.h"

using namespace anyrtc;

// 构造函数：event 是 Java 层传入的播放观察者对象（局部引用），
// 需要转换为全局引用长期持有，供后续任意线程/任意时刻触发的播放回调使用
LivePlayEvent::LivePlayEvent(jobject event):
        m_jJavaObj(NULL),
        m_jClass(NULL){
    if (event){
        // 确保当前线程已附着到 JVM，拿到可用的 JNIEnv*
        JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
        // 将局部引用提升为全局引用：局部引用仅在当前 JNI 调用范围内有效，
        // 而播放器的生命周期贯穿整个播放会话，需要跨越多次 JNI 调用持有该对象，
        // 因此必须使用 NewGlobalRef 创建一个需手动释放、但不会被自动回收的全局引用
        m_jJavaObj = jni->NewGlobalRef(event);
        // 获取该对象的 Class 并同样转换为全局引用，供 onRenderVideoFrame /
        // onReceiveSeiMessage 中手写的 GetMethodID 反射调用使用
        m_jClass = static_cast<jclass>(jni->NewGlobalRef(
                jni->GetObjectClass(m_jJavaObj)));
    }


}

// 析构函数：对称释放构造函数中创建的两个全局引用，避免 JNI 全局引用泄漏
// （全局引用不像局部引用那样会随栈帧自动释放，必须显式 DeleteGlobalRef，
// 否则对应的 Java 对象会一直被 JNI 全局引用表钉住，无法被垃圾回收）
LivePlayEvent::~LivePlayEvent(){
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


// 错误回调实现：转发错误码和错误信息给 Java 层
void LivePlayEvent::onError(IArLivePlayer* player, int32_t code, const char* msg, void* extraInfo){

    // 确保当前线程（可能是播放引擎内部解码/网络线程）已附着到 JVM
    JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
    if (m_jJavaObj != NULL) {
        // JavaParamRef 用已持有的全局引用 m_jJavaObj 构造，作为"仅传参、不持有所有权"的
        // 轻量包装，传给下面自动生成的跳板函数使用
        const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
        // NativeToJavaString 把 C 字符串 msg 转换为 Java 字符串（jstring）。
        // 此处对 msg 转换了两次，生成两个独立的 local ref 字符串对象分别传给跳板函数的
        // 两个字符串形参
        Java_NativePlayObserver_onError(jni, j_context, code, webrtc::NativeToJavaString(jni, msg),
                                        webrtc::NativeToJavaString(jni, msg));
    }
}

// 警告回调实现：逻辑与 onError 相同，转发到 onWarning 跳板函数
void LivePlayEvent::onWarning(IArLivePlayer* player, int32_t code, const char* msg, void* extraInfo){

    JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
    if (m_jJavaObj != NULL) {
        const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
        Java_NativePlayObserver_onWarning(jni, j_context, code, webrtc::NativeToJavaString(jni, msg),
                                        webrtc::NativeToJavaString(jni, msg));
    }
}

// 视频播放状态更新回调实现：状态和原因直接透传（枚举可隐式转换为 jint），
// extraInfo 若非空则视为 C 字符串并转换为 jstring，否则传 NULL
void LivePlayEvent::onVideoPlayStatusUpdate(IArLivePlayer* player, ArLivePlayStatus status, ArLiveStatusChangeReason reason, void* extraInfo){

    JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
    if (m_jJavaObj != NULL) {
        const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
        Java_NativePlayObserver_onVideoPlayStatusUpdate(jni, j_context, status, reason,extraInfo==NULL?NULL:
                                        webrtc::NativeToJavaString(jni, (char*)extraInfo));
    }


}

// 音频播放状态更新回调实现：逻辑与 onVideoPlayStatusUpdate 相同，针对音频通道
void LivePlayEvent::onAudioPlayStatusUpdate(IArLivePlayer* player, ArLivePlayStatus status, ArLiveStatusChangeReason reason, void* extraInfo){


    JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
    if (m_jJavaObj != NULL) {
        const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
        Java_NativePlayObserver_onAudioPlayStatusUpdate(jni, j_context, status, reason,extraInfo==NULL?NULL:
                                                                                       webrtc::NativeToJavaString(jni, (char*)extraInfo));
    }

}

// 播放音量更新回调实现：直接把 volume 转发给 Java 层
void LivePlayEvent::onPlayoutVolumeUpdate(IArLivePlayer* player, int32_t volume){


    JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
    if (m_jJavaObj != NULL) {
        const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
        Java_NativePlayObserver_onPlayoutVolumeUpdate(jni, j_context, volume);
    }


}

// 播放统计信息更新回调实现：把统计结构体各字段（应用CPU/系统CPU/宽/高/帧率/视频码率/
// 音频码率）依次传给 Java 层跳板函数
void LivePlayEvent::onStatisticsUpdate(IArLivePlayer* player, ArLivePlayerStatistics statistics){

    JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
    if (m_jJavaObj != NULL) {
        const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
        Java_NativePlayObserver_onStatisticsUpdate(jni, j_context, statistics.appCpu,statistics.systemCpu,statistics.width,statistics.height,statistics.fps,statistics.videoBitrate,statistics.audioBitrate);
    }

}

// 截图完成回调：当前是空实现，未转发给 Java 层
void LivePlayEvent::onSnapshotComplete(IArLivePlayer* player, const char* image, int length, int width, int height, ArLivePixelFormat format){

}

// 渲染视频帧回调：解码后的每一帧视频数据都会走到这里，需要把 native 侧的裸指针帧数据
// 转换为 Java 层可用的对象后再调用 Java 方法（onRenderVideoFrame），
// 因此没有使用自动生成的跳板函数，而是手写 GetMethodID + CallVoidMethod
void LivePlayEvent::onRenderVideoFrame(IArLivePlayer* player, const ArLiveVideoFrame* videoFrame){
    if (m_jJavaObj != NULL && m_jClass !=NULL) {

        // 确保当前线程（通常是解码/渲染线程，不一定是 Java 创建的线程）已附着到 JVM
        JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
        {

            // 反射查找 Java 观察者类上的 onRenderVideoFrame 方法。
            // JNI 签名 "(II[BLjava/nio/ByteBuffer;III)V" 逐段解读：
            //   I               -> 第1个 int 参数：pixelFormat（像素格式）
            //   I               -> 第2个 int 参数：bufferType（缓冲区类型）
            //   [B              -> 第3个参数：byte[]（视频帧数据的字节数组形式）
            //   Ljava/nio/ByteBuffer; -> 第4个参数：java.nio.ByteBuffer 对象（直接缓冲区形式）
            //   I I I           -> 第5、6、7个 int 参数：width、height、rotation
            //   V               -> 返回值 void
            // 也就是说这里同时提供了字节数组和直接 ByteBuffer 两种形式的帧数据，
            // 具体使用哪种由 Java 层根据 bufferType 决定
            jmethodID j_callJavaMId = jni->GetMethodID(m_jClass, "onRenderVideoFrame",
                                                               "(II[BLjava/nio/ByteBuffer;III)V");
                // 创建一个长度为 videoFrame->length 的 Java byte[]（此时内容未初始化）
                jbyteArray dataArray = jni->NewByteArray(videoFrame->length);
                // 用 videoFrame->data 指向的 native 内存直接构造一个 java.nio.ByteBuffer，
                // 不发生数据拷贝，Java 层可以直接读取这块 native 内存
                // （要求 native 内存在 Java 使用期间保持有效）
                jobject _buf = jni->NewDirectByteBuffer(videoFrame->data, videoFrame->length);
                // 把 native 侧的帧数据拷贝进刚创建的 byte[]（这一步与上面的 DirectByteBuffer
                // 是两份独立的数据副本/视图，供 Java 层按需选用）
                jni->SetByteArrayRegion(dataArray, 0, videoFrame->length,
                                        reinterpret_cast<const jbyte *>(videoFrame->data));

                // 调用 Java 层 onRenderVideoFrame 方法，把像素格式、缓冲区类型、字节数组、
                // 直接缓冲区、宽高、旋转角度都传过去
                jni->CallVoidMethod(m_jJavaObj,j_callJavaMId,videoFrame->pixelFormat,videoFrame->bufferType,dataArray,_buf,videoFrame->width,videoFrame->height,videoFrame->rotation);
                // 这里的写法是：先调用 GetByteArrayElements 获取一份 dataArray 的指针，
                // 再立即调用 ReleaseByteArrayElements 释放它（模式为 0，表示如果是拷贝模式则
                // 把内容拷回数组并释放缓冲区）。
                // 注意：这一组 Get/Release 只是为了触发某些 JVM 实现下的"提交更改"语义，
                // 但由于前面从未通过这里获取的指针写入过任何数据，
                // 且此处新 Get 到的指针与调用之前完全无关，这样"现取现放"的写法
                // 相比直接保存 SetByteArrayRegion 之前的指针要显得多余、浪费一次
                // Get/Release 调用开销；这是原有代码的既有写法，这里只做说明不做修改
                jni->ReleaseByteArrayElements(dataArray, jni->GetByteArrayElements(dataArray, 0),
                                              0);
                // 释放 NewDirectByteBuffer 产生的局部引用（DirectByteBuffer 对象本身，
                // 不影响它所包装的 native 内存）
                jni->DeleteLocalRef(_buf);
                // 注意：这里没有对 dataArray 调用 DeleteLocalRef，
                // 该 local ref 会在当前 native 方法返回、JVM 清理当前 JNI 调用帧时被自动回收
        }
    }
}

// 接收到 SEI（Supplemental Enhancement Information，补充增强信息）消息回调：
// SEI 数据通常随视频帧一起下发，用于传递自定义的应用层数据（如时间戳、自定义标记等），
// 同样因为需要传递二进制数据，手写 GetMethodID + CallVoidMethod 而非自动生成跳板函数
void LivePlayEvent::onReceiveSeiMessage(IArLivePlayer* player, int payloadType, const uint8_t* data, uint32_t dataSize){


    if (m_jJavaObj != NULL && m_jClass !=NULL) {

        // 确保当前线程已附着到 JVM
        JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
        {
//
            // 反射查找 onReceiveSeiMessage 方法。
            // JNI 签名 "(I[B)V" 表示：第1个参数是 int（payloadType，SEI 负载类型），
            // 第2个参数是 byte[]（SEI 原始数据），返回 void
            jmethodID j_callJavaMId = jni->GetMethodID(m_jClass, "onReceiveSeiMessage",
                                                       "(I[B)V");
            // 创建长度为 dataSize 的 Java byte[]
            jbyteArray dataArray = jni->NewByteArray(dataSize);
            // 把 native 侧 SEI 数据拷贝进这个 byte[]
            jni->SetByteArrayRegion(dataArray, 0, dataSize,
                                    reinterpret_cast<const jbyte *>(data));

            // 调用 Java 层方法，传入负载类型和数据数组
            jni->CallVoidMethod(m_jJavaObj,j_callJavaMId,(jint)payloadType,dataArray);
            // 同 onRenderVideoFrame 中的写法：先 GetByteArrayElements 再立即
            // ReleaseByteArrayElements，效果上只是走了一遍"获取指针又释放"的流程，
            // 并没有真正用这个指针读写任何数据，相比直接跳过这一步显得冗余，
            // 此处仅指出这一点，不改动代码行为
            jni->ReleaseByteArrayElements(dataArray, jni->GetByteArrayElements(dataArray, 0),
                                          0);
            // 显式释放 dataArray 这个局部引用（提前告知 JVM 可以回收，
            // 不必等到方法返回时才自动清理）
            jni->DeleteLocalRef(dataArray);
        }
    }
}

// 点播（VOD）播放进度回调实现：把总时长、当前播放时间、缓冲时长转发给 Java 层
void LivePlayEvent::onVodPlaybackProcess(IArLivePlayer *player, int nAllDuration,
                                         int nCurrentPlaybackTime, int nBufferDuration) {

    JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
    if (m_jJavaObj != NULL) {
        const base::android::JavaParamRef<jobject> j_context(m_jJavaObj);
        Java_NativePlayObserver_onVodPlaybackProcess(jni, j_context, nAllDuration,nCurrentPlaybackTime,nBufferDuration);
    }
}
