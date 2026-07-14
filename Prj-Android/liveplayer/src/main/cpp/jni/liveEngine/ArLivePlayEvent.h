//
// Created by liu on 2021/9/22.
//

#ifndef LIVEPLAYER_ARLIVEPLAYEVENT_H
#define LIVEPLAYER_ARLIVEPLAYEVENT_H
// IArLivePlayer.hpp：定义播放相关抽象接口 ArLivePlayerObserver 及相关状态/统计/视频帧结构体
#include "IArLivePlayer.hpp"
// helpers_android.h：提供 AttachCurrentThreadIfNeeded 等辅助函数
#include <webrtc/modules/utility/include/helpers_android.h>
// webrtc 内部 jvm.h：提供 GetClass 等 JNI 工具函数
#include "webrtc/sdk/android/src/jni/jvm.h"
// scoped_java_ref.h：提供 ScopedJavaLocalRef/JavaParamRef 等 RAII 包装类型
#include "webrtc/sdk/android/native_api/jni/scoped_java_ref.h"
// java_types.h：提供 NativeToJavaString 等 Java<->native 字符串转换工具函数
#include "webrtc/sdk/android/native_api/jni/java_types.h"

namespace anyrtc{

// LivePlayEvent：native 层 ArLivePlayerObserver 接口的具体实现，
// 负责把播放器（Player）产生的各类回调事件转发给 Java 层的观察者对象。
// 大部分回调通过 NativePlayObserver_JNI.h 中自动生成的 Java_NativePlayObserver_on*
// 跳板函数转发；onRenderVideoFrame 和 onReceiveSeiMessage 因为需要传递二进制数据（视频帧/SEI数据），
// 改为手写 GetMethodID + CallVoidMethod 的方式，手动构造 jbyteArray/ByteBuffer 参数
class LivePlayEvent : public ArLivePlayerObserver{

public:
    // 构造函数：传入 Java 层播放观察者对象（jobject），内部创建 JNI 全局引用保存
    LivePlayEvent(jobject event);
    // 析构函数：释放构造函数中创建的 JNI 全局引用
    ~LivePlayEvent(void);

public:
    // 错误回调：播放过程中发生不可恢复错误
    void onError(IArLivePlayer* player, int32_t code, const char* msg, void* extraInfo);
    // 警告回调：播放过程中发生可恢复的异常情况
    void onWarning(IArLivePlayer* player, int32_t code, const char* msg, void* extraInfo);
    // 视频播放状态变化回调（如加载中/播放中/停止等），附带状态变化原因
    void onVideoPlayStatusUpdate(IArLivePlayer* player, ArLivePlayStatus status, ArLiveStatusChangeReason reason, void* extraInfo);
    // 音频播放状态变化回调，附带状态变化原因
    void onAudioPlayStatusUpdate(IArLivePlayer* player, ArLivePlayStatus status, ArLiveStatusChangeReason reason, void* extraInfo);
    // 播放音量更新回调
    void onPlayoutVolumeUpdate(IArLivePlayer* player, int32_t volume);
    // 播放统计信息更新回调（CPU/码率/分辨率/帧率等）
    void onStatisticsUpdate(IArLivePlayer* player, ArLivePlayerStatistics statistics);
    // 截图完成回调
    void onSnapshotComplete(IArLivePlayer* player, const char* image, int length, int width, int height, ArLivePixelFormat format);
    // 渲染视频帧回调：把解码后的原始视频帧数据（YUV/RGB 等）传递给 Java 层进行渲染，
    // 需要手动构造 jbyteArray + 直接 ByteBuffer 参数（见 .cpp 实现）
    void onRenderVideoFrame(IArLivePlayer* player, const ArLiveVideoFrame* videoFrame);
    // 接收到 SEI（补充增强信息）消息回调：把 SEI 负载数据以 jbyteArray 形式传递给 Java 层
    void onReceiveSeiMessage(IArLivePlayer* player, int payloadType, const uint8_t* data, uint32_t dataSize);
    // 点播（VOD）播放进度回调：总时长/当前播放时间/缓冲时长
    void onVodPlaybackProcess(IArLivePlayer* player, int nAllDuration, int nCurrentPlaybackTime, int nBufferDuration);
protected:
    // 持有 Java 层播放观察者对象的全局引用。原因同 LivePushEvent：
    // 构造函数收到的只是局部引用，必须提升为全局引用才能在后续任意时刻的异步回调中安全使用，
    // 否则可能引用一个已经被 JVM 回收/失效的对象
    jobject m_jJavaObj;
    // 持有该 Java 对象所属 Class 的全局引用，供 onRenderVideoFrame/onReceiveSeiMessage 中
    // 手写的 GetMethodID 反射调用使用
    jclass m_jClass;
};
}

#endif //LIVEPLAYER_ARLIVEPLAYEVENT_H
