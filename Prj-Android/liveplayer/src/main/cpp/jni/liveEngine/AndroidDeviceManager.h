//
// Created by liu on 2021/11/4.
//

#ifndef LIVEPLAYER_ANDROIDDEVICEMANAGER_H
#define LIVEPLAYER_ANDROIDDEVICEMANAGER_H

// jni.h：标准 JNI 头文件，提供 JNIEnv、jobject、jclass 等 JNI 基础类型和函数声明
#include "jni.h"
// jvm_android.h：WebRTC 提供的 Android JVM 辅助工具（如 AttachCurrentThreadIfNeeded，
// 用于让当前 native 线程附着到 JVM 上，获得可用的 JNIEnv*）
#include <modules/utility/include/jvm_android.h>
// IArDeviceManager.h：设备管理相关的抽象接口/类型定义（如 VideoState 等）
#include <IArDeviceManager.h>
// webrtc 内部 jvm 头文件，提供 GetClass 等根据类名反射查找 Java 类的工具函数
#include "webrtc/sdk/android/src/jni/jvm.h"
// video_source.h：提供 CreateJavaVideoSource 等 native 层创建"由 Java 端摄像头驱动的
// 视频源"的工厂函数，以及 JavaVideoTrackSourceInterface 类型
#include "sdk/android/native_api/video/video_source.h"
// WebRTC 视频编码器工厂抽象接口（VideoEncoderFactory）
#include "api/video_codecs/video_encoder_factory.h"
// WebRTC 视频解码器工厂抽象接口（VideoDecoderFactory）
#include "api/video_codecs/video_decoder_factory.h"
// PlatformContext：跨平台上下文抽象基类，Android 上由 AndroidContext 实现，
// 用于持有 Java 侧 VideoCapturerDevice 对象及其 Class 引用
#include "PlatformContext.h"
// VideoCameraCapturer：真正驱动摄像头/屏幕采集的 native 采集器类
#include "VideoCameraCapturer.h"
// class_loader.h：提供跨 ClassLoader 加载 Java 类的辅助能力
#include "sdk/android/native_api/jni/class_loader.h"

// AndroidDeviceManager：Android 平台下的视频设备（摄像头/屏幕采集）统一管理类。
// 负责创建 VideoTrackSource、创建/切换摄像头采集器、创建编解码器工厂，
// 以及把美颜、变焦、对焦、闪光灯等控制指令转发给 Java 层的 VideoCapturerDevice。
class AndroidDeviceManager{

public:
    // 构造函数：仅做成员初始化，不持有任何 JNI 全局引用（JNI 引用在 createVideoSource 等
    // 具体使用时才创建）
    AndroidDeviceManager(void);

    // 析构函数：当前实现为空（见 .cpp），并未释放 videoSource/_capturer 之外的资源
    virtual ~AndroidDeviceManager(void);

    // 获取全局单例。注意：这是一个"裸指针 + 懒汉式"单例（Meyer's 单例的裸指针变体），
    // 首次调用时 new 出对象，但从未 delete，进程退出前会一直存活（详见 .cpp 中的说明）
    static AndroidDeviceManager &Inst();

    // 设置平台上下文（Android 下实际是 AndroidContext，封装了 Java 侧 VideoCapturerDevice
    // 对象及其 Class 的引用），后续所有需要回调 Java 层的方法都依赖它
    void setPlatformContext(std::shared_ptr<arlive::PlatformContext> _platformContext);

    // 创建视频源：内部会创建 JavaVideoTrackSourceInterface，再包一层 VideoTrackSourceProxy
    // 返回，使得跨线程访问该视频源是安全的（见 .cpp 实现注释）
    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> createVideoSource();

    // 设置外部视频源（本文件对应的 .cpp 中未实现该方法，仅在头文件声明，可能是预留接口）
    void setVideoSource(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> _videoSource,
                        std::string deviceId, bool isScreenCapture);


    // 创建视频采集器：根据 capture_device_index 选择前置/后置摄像头或屏幕采集，
    // 并记录采集分辨率与帧率；返回 this 指针（void* 形式，供上层作为句柄使用）
    void *
    createVideoCapture(size_t width, size_t height, size_t fps, size_t capture_device_index);

    // 创建视频编码器工厂：通过 JNI 反射实例化 Java 层 org.webrtc.DefaultVideoEncoderFactory，
    // 再转换为 native 的 VideoEncoderFactory 接口
    std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactory();
    // 创建视频解码器工厂：通过 JNI 反射实例化 Java 层 org.webrtc.DefaultVideoDecoderFactory，
    // 再转换为 native 的 VideoDecoderFactory 接口
    std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactory();

    // 切换前置/后置摄像头，返回错误码（jint，对应 ArLiveCode 定义的枚举值）
    jint switchCamera(bool isFront);
    // 查询当前是否为前置摄像头（当前实现固定返回 false，见 .cpp）
    jboolean isFrontCamera();
    // 获取摄像头支持的最大变焦倍率
    jfloat getCameraZoomMaxRatio();
    // 设置摄像头变焦倍率，返回错误码
    jint setCameraZoomRatio(float zoomRatio);
    // 设置摄像头采集参数（模式/宽/高），内部会触发采集格式变更
    void setCameraCapturerParam(int mode,int width,int height);
    // 查询是否已开启自动对焦
    jboolean isAutoFocusEnabled();
    // 开关自动对焦，返回错误码
    jint enableCameraAutoFocus(bool enabled);
    // 设置对焦位置（归一化坐标 x,y），返回错误码
    jint setCameraFocusPosition(float x, float y);
    // 开关闪光灯（手电筒模式），返回是否成功
    jboolean enableCameraTorch(bool enabled);
    // 恢复摄像头到默认状态（转发给 Java 层 VideoCapturerDevice.recoverCamera）
    void recoverCamera();

    // 开始采集：把 _capturer 状态置为 Active
    bool startCapture();

    // 停止采集：把 _capturer 状态置为 Inactive
    bool stopCapture();

    // 销毁采集器：重置 unique_ptr，释放底层 VideoCameraCapturer 资源
    void destoryCapture();

    // 获取 Java 侧的视频采集观察者对象（用于 Java 层向 native 视频源投递视频帧），
    // 转发调用 videoSource 的同名方法
    webrtc::ScopedJavaLocalRef<jobject> GetJavaVideoCapturerObserver(JNIEnv *env);

    // 美颜总开关：转发给 Java 层 VideoCapturerDevice.needBeautyEffect(boolean)
    int setBeautyEffect(bool enable);

    // 设置美白级别：转发给 Java 层 VideoCapturerDevice.setWhitenessLevel(float)
    int setWhitenessLevel(float level);

    // 设置磨皮/美颜级别：转发给 Java 层 VideoCapturerDevice.setBeautyLevel(float)
    int setBeautyLevel(float level);

    // 设置色调/红润级别：转发给 Java 层 VideoCapturerDevice.setToneLevel(float)
    int setToneLevel(float level);

public:
    // 持有的 Java 视频轨道源（由 CreateJavaVideoSource 创建），是引用计数的智能指针，
    // 生命周期跟随本对象及所有持有它的地方共同管理
    rtc::scoped_refptr<webrtc::JavaVideoTrackSourceInterface> videoSource;
    // 实际驱动摄像头/屏幕采集的采集器实例，unique_ptr 独占持有，切换摄像头时会先置空再重建
    std::unique_ptr<arlive::VideoCameraCapturer> _capturer;
    // 当前采集宽度
    int captureWidth;
    // 当前采集高度（注意命名沿用原代码，未做修改）
    int captureheight;
    // 当前采集帧率
    int captureFps;
    // 是否为屏幕采集模式（true 时部分摄像头相关操作会被拒绝，如 switchCamera）
    bool isScreenCapture;
    // 平台上下文（Android 下为 AndroidContext），持有 Java 侧对象引用，供各 JNI 转发方法使用
    std::shared_ptr<arlive::PlatformContext> _platformContext;
    // 当前采集状态（Active/Inactive），切换摄像头后需要把新采集器恢复到该状态
    arlive::VideoState _state = arlive::VideoState::Inactive;
};


#endif //LIVEPLAYER_ANDROIDDEVICEMANAGER_H
