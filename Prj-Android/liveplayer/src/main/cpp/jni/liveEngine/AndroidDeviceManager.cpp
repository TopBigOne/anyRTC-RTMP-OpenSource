//
// Created by liu on 2021/11/4.
//

// StaticThreads：提供全局共享的 signaling/worker/media 三个 rtc::Thread 实例，
// 避免每次创建视频源都各自新建线程
#include <StaticThreads.h>
// video_track_source_proxy.h：VideoTrackSourceProxy，用于把对 VideoTrackSource 的调用
// 从调用线程转发（marshal）到视频源实际所在的线程上执行
#include <pc/video_track_source_proxy.h>
// AndroidContext：PlatformContext 在 Android 上的具体实现，内部持有 Java 层
// VideoCapturerDevice 对象的全局引用及其 Class 引用
#include <AndroidContext.h>
// codecs/wrapper.h：提供 JavaToNativeVideoEncoderFactory / JavaToNativeVideoDecoderFactory，
// 把 Java 层的编解码器工厂对象包装成 native 的 VideoEncoderFactory/VideoDecoderFactory
#include <android/native_api/codecs/wrapper.h>
#include "AndroidDeviceManager.h"
// ArLiveCode.hpp：定义了 ArLiveCode 中的错误码常量（如 ArLIVE_OK、ArLIVE_ERROR_REFUSED）
#include "ArLiveCode.hpp"


using namespace arlive;
using namespace anyrtc;
// 全局裸指针，保存单例实例。注意：这是"裸指针懒汉式单例"，从未被 delete，
// 会随进程一直存活到进程退出，属于有意为之的简化写法（非内存泄漏排查目标），此处仅作说明
static AndroidDeviceManager *gInstance = NULL;

// 获取（或首次创建）AndroidDeviceManager 单例。
// 注意：非线程安全（无锁/无 std::call_once 保护），且创建的对象永不释放
AndroidDeviceManager &AndroidDeviceManager::Inst() {
    if (gInstance == NULL) {
        // 首次调用时用 new 分配单例对象，之后一直复用，进程结束前不会被回收
        gInstance = new AndroidDeviceManager();
    }
    return *gInstance;
}

// 构造函数：初始化成员变量的默认值，videoSource 置空、非屏幕采集、平台上下文置空
AndroidDeviceManager::AndroidDeviceManager(void) :
        videoSource(NULL),
        isScreenCapture(false),
        _platformContext(NULL) {

}

// 析构函数：当前是空实现（已被注释掉的 gInstance = NULL 说明作者曾考虑过在析构时
// 清空全局指针，但最终未启用），因为该单例本来就不会被销毁，这里更多是占位
AndroidDeviceManager::~AndroidDeviceManager(void) {
    // gInstance = NULL;
}

// 创建（或复用已创建的）视频轨道源。
// 该视频源最终会被摄像头采集器（VideoCameraCapturer）投递视频帧，
// 也会被上层 PeerConnection/编码流程读取，因此需要跨线程安全访问。
rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> AndroidDeviceManager::createVideoSource() {
    // 确保当前 native 线程已附着到 JVM，拿到可用的 JNIEnv*
    // （createVideoSource 可能在非 Java 创建的 native 线程上被调用，例如信令/媒体线程）
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    if (videoSource) {
        // 已经创建过，直接复用，避免重复创建 Java 视频源对象
        return videoSource;
    }
    // 调用 WebRTC Android native API 创建一个"由 Java 端驱动帧输入"的视频源：
    // 第二个参数传入 media 线程，表示该视频源对象在 media 线程上下文中创建/关联；
    // 两个 false 分别对应 is_screencast（是否屏幕共享）和 align_timestamps（是否对齐时间戳）
    videoSource = webrtc::CreateJavaVideoSource(env,
                                                arlive::StaticThreads::getThreads()->getMediaThread(),
                                                false,
                                                false);
    // 用 VideoTrackSourceProxy 包装原始视频源：
    // VideoTrackSourceProxy 会把跨线程的方法调用（例如信令线程查询视频源状态，
    // 而实际视频源逻辑跑在 worker/media 线程）自动转发（marshal）到正确的线程执行，
    // 从而让上层可以在任意线程安全地持有和调用这个视频源，而不用关心线程亲和性问题
    return webrtc::VideoTrackSourceProxy::Create(
            arlive::StaticThreads::getThreads()->getMediaThread(),
            arlive::StaticThreads::getThreads()->getWorkerThread(),
            videoSource);
}

/**
 * 创建视频采集器（摄像头或屏幕采集）。
 * @param width
 * @param height
 * @param fps
 * @param capture_device_index  0 表示前置摄像头，8 表示屏幕采集，其余值表示后置摄像头
 * @return
 */
void *AndroidDeviceManager::createVideoCapture(size_t width, size_t height, size_t fps,
                                               size_t capture_device_index) {
    if (capture_device_index == 8) {
        // 屏幕采集：把 videoSource 作为帧的投递目标，采集源标记为 "screen"
        _capturer = std::make_unique<VideoCameraCapturer>(videoSource, "screen", width, height, fps,
                                                          _platformContext);
        isScreenCapture = true;
    } else {
        // 摄像头采集：index==0 为前置摄像头 "front"，否则为后置摄像头 "back"
        _capturer = std::make_unique<VideoCameraCapturer>(videoSource,
                                                          capture_device_index == 0 ? "front"
                                                                                    : "back", width,
                                                          height, fps, _platformContext);
        isScreenCapture = false;
    }
    // 记录当前采集参数，供后续 switchCamera / setCameraCapturerParam 复用
    captureFps = fps;
    captureWidth = width;
    captureheight = height;
    // 返回 this 作为不透明句柄（void*），供 JNI 层保存/传递
    return this;
}

/**
 * 创建视频编码器工厂：通过 JNI 反射实例化 Java 层的 DefaultVideoEncoderFactory，
 * 再转换为 native 侧的 VideoEncoderFactory 接口供 PeerConnection 使用
 * @return
 */
std::unique_ptr<webrtc::VideoEncoderFactory> AndroidDeviceManager::makeVideoEncoderFactory() {
// 确保当前线程已附着到 JVM
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
// 取出 AndroidContext（PlatformContext 的 Android 实现），它持有 Java 层
// VideoCapturerDevice 对象及其 Class 的引用
    AndroidContext *context = (AndroidContext *) _platformContext.get();
// 反射查找 VideoCapturerDevice 类上的实例方法 getSharedEGLContext。
// JNI 签名 "()Lorg/webrtc/EglBase$Context;" 表示：无参数，返回值类型为
// org.webrtc.EglBase.Context 对象引用（Lxxx; 表示引用类型，括号内为空表示无入参）
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(),
                                          "getSharedEGLContext",
                                          "()Lorg/webrtc/EglBase$Context;");
// 调用该方法，获取 Java 层共享的 EGL 上下文对象（用于编码器的硬件加速渲染共享）
    jobject eglContext = env->CallObjectMethod(context->getJavaCapturer(), methodId);

// 通过类名反射获取 org.webrtc.DefaultVideoEncoderFactory 这个 Java 类
// （ScopedJavaLocalRef 会在其自身生命周期结束时自动释放这个 local ref，无需手动 DeleteLocalRef）
    webrtc::ScopedJavaLocalRef<jclass> factory_class = webrtc::GetClass(env,
                                                                        "org/webrtc/DefaultVideoEncoderFactory");
    // 反射查找该类的构造函数 <init>。
    // JNI 签名 "(Lorg/webrtc/EglBase$Context;ZZ)V" 表示：
    // 参数为 1 个 EglBase.Context 对象 + 2 个 boolean（Z），返回 void（V）
    // 对应 DefaultVideoEncoderFactory(EglBase.Context, boolean enableIntelVp8Encoder, boolean enableH264HighProfile)
    jmethodID factory_constructor = env->GetMethodID(factory_class.obj(), "<init>",
                                                     "(Lorg/webrtc/EglBase$Context;ZZ)V");
    // 调用构造函数创建 Java 对象：eglContext 传入共享上下文，
    // false 表示不启用 Intel VP8 编码器，
    // true 表示启用 H264 High Profile；
    // ScopedJavaLocalRef 包装以自动管理 local ref 生命周期
    webrtc::ScopedJavaLocalRef<jobject> factory_object(env, env->NewObject(factory_class.obj(),
                                                                           factory_constructor,
                                                                           eglContext,
                                                                           false,
                                                                           true));
// 将这个 Java 编码器工厂对象包装为 native 的 VideoEncoderFactory 接口实现并返回
    return webrtc::JavaToNativeVideoEncoderFactory(env, factory_object.obj());
}

// 创建视频解码器工厂：通过 JNI 反射实例化 Java 层的 DefaultVideoDecoderFactory，
// 再转换为 native 侧的 VideoDecoderFactory 接口
std::unique_ptr<webrtc::VideoDecoderFactory> AndroidDeviceManager::makeVideoDecoderFactory() {
    // 确保当前线程已附着到 JVM
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    // 取出 AndroidContext，用于访问 Java 层的 VideoCapturerDevice 对象/类
    AndroidContext *context = (AndroidContext *) _platformContext.get();
    // 同上：反射查找 getSharedEGLContext 方法，签名同样是"无参数，返回 EglBase.Context"
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(),
                                          "getSharedEGLContext",
                                          "()Lorg/webrtc/EglBase$Context;");
    // 获取共享的 EGL 上下文，供解码器硬件加速渲染共享
    jobject eglContext = env->CallObjectMethod(context->getJavaCapturer(), methodId);

    // 反射获取 org.webrtc.DefaultVideoDecoderFactory 类
    webrtc::ScopedJavaLocalRef<jclass> factory_class = webrtc::GetClass(env,
                                                                        "org/webrtc/DefaultVideoDecoderFactory");
    // 反射查找构造函数 <init>，签名 "(Lorg/webrtc/EglBase$Context;)V" 表示：
    // 只接受 1 个 EglBase.Context 参数，返回 void
    jmethodID factory_constructor = env->GetMethodID(factory_class.obj(), "<init>",
                                                     "(Lorg/webrtc/EglBase$Context;)V");
    // 调用构造函数创建 Java 解码器工厂对象
    webrtc::ScopedJavaLocalRef<jobject> factory_object(env, env->NewObject(factory_class.obj(),
                                                                           factory_constructor,
                                                                           eglContext));
    // 包装为 native 的 VideoDecoderFactory 接口实现并返回
    return webrtc::JavaToNativeVideoDecoderFactory(env, factory_object.obj());
}


// 保存平台上下文（Android 上为 AndroidContext），后续所有需要回调 Java 层的方法
// （美颜/变焦/编解码器工厂等）都通过它拿到 Java 对象/类的引用
void AndroidDeviceManager::setPlatformContext(std::shared_ptr<arlive::PlatformContext> context) {
    _platformContext = context;
}

/**
 *
// 获取 Java 侧的视频采集观察者对象：直接转发给 videoSource（JavaVideoTrackSourceInterface）
// 自身提供的同名方法，由 Java 层摄像头采集回调通过该观察者对象把帧数据送入 native 视频源
 * @param env
 * @return
 */
webrtc::ScopedJavaLocalRef<jobject>
AndroidDeviceManager::GetJavaVideoCapturerObserver(JNIEnv *env) {
    return videoSource->GetJavaVideoCapturerObserver(env);
}


// 切换前置/后置摄像头。
// 屏幕采集模式下不支持切换摄像头，直接拒绝
jint AndroidDeviceManager::switchCamera(bool isFront) {
    if (isScreenCapture)
        return ArLIVE_ERROR_REFUSED;
    if (videoSource) {
        //this should outlive the capturer
        // 先将旧的采集器置空（析构旧的 VideoCameraCapturer，停止旧摄像头），
        // 注意 videoSource 的生命周期比 _capturer 长（注释所述），
        // 保证新采集器创建时 videoSource 仍然有效
        _capturer = nullptr;
        // 重新创建一个指向新摄像头（前置/后置）的采集器，复用之前记录的分辨率与帧率
        _capturer = std::make_unique<VideoCameraCapturer>(videoSource, isFront ? "front" : "back",
                                                          captureWidth, captureheight, captureFps,
                                                          _platformContext);
    }
    if (_capturer != nullptr) {
        // 把新采集器的状态恢复为切换前记录的状态（Active/Inactive），
        // 避免切换摄像头后采集意外停止或未按预期启动
        _capturer->setState(_state);
        return ArLiveCode::ArLIVE_OK;
    }
    return ArLiveCode::ArLIVE_ERROR_REFUSED;
}

// 查询是否为前置摄像头：当前实现是简单占位，恒定返回 false（未跟踪真实状态）
jboolean AndroidDeviceManager::isFrontCamera() {
    return JNI_FALSE;
}

// 获取摄像头支持的最大变焦倍率：透传给底层 _capturer；采集器未创建时返回 0
jfloat AndroidDeviceManager::getCameraZoomMaxRatio() {
    if (_capturer != NULL) {
        return _capturer->getCameraZoomMaxRatio();
    }
    return (jfloat) 0;
}

// 设置摄像头变焦倍率：透传给底层 _capturer；采集器未创建时返回 -1 表示失败
jint AndroidDeviceManager::setCameraZoomRatio(float zoomRatio) {
    if (_capturer != NULL) {
        return _capturer->setCameraZoomRatio(zoomRatio);
    }
    return (jint) -1;
}

// 设置摄像头采集参数（mode 参数当前未使用），更新宽高并触发采集格式变更（帧率不变）
void AndroidDeviceManager::setCameraCapturerParam(int mode, int width, int height) {
    if (_capturer != NULL) {
        captureWidth = width;
        captureheight = height;
        _capturer->changeCaptureFormat(captureWidth, captureheight, captureFps);
    }
}

// 查询是否已开启自动对焦：透传给底层 _capturer；采集器未创建时返回 false
jboolean AndroidDeviceManager::isAutoFocusEnabled() {
    if (_capturer != NULL) {
        return _capturer->isAutoFocusEnabled();
    }
    return JNI_FALSE;
}

// 开关自动对焦：透传给底层 _capturer，返回其执行结果码；采集器未创建时返回 -1
jint AndroidDeviceManager::enableCameraAutoFocus(bool enabled) {
    if (_capturer != NULL) {
        int result = _capturer->enableCameraAutoFocus(enabled);
        return (jint) result;
    }
    return (jint) -1;
}

// 设置对焦位置（归一化坐标 x, y）：透传给底层 _capturer；采集器未创建时返回 -1
jint AndroidDeviceManager::setCameraFocusPosition(float x, float y) {
    if (_capturer != NULL) {
        int result = _capturer->setCameraFocusPosition(x, y);
        return (jint) result;
    }
    return (jint) -1;
}

// 开关闪光灯（手电筒）：透传给底层 _capturer，返回值为 0 时视为成功（转换为 jboolean）；
// 采集器未创建时返回 false
jboolean AndroidDeviceManager::enableCameraTorch(bool enabled) {
    if (_capturer != NULL) {
        int result = _capturer->enableCameraTorch(enabled);
        return result == 0;
    }
    return JNI_FALSE;
}


// 开始采集：把底层采集器状态置为 Active，并记录到 _state（供切换摄像头后恢复用）；
// 采集器未创建时直接返回 false
bool AndroidDeviceManager::startCapture() {
    if (_capturer == NULL) {
        return false;
    }
    _capturer->setState(arlive::VideoState::Active);
    _state = arlive::VideoState::Active;
    return true;
}

// 停止采集：把底层采集器状态置为 Inactive，并同步记录到 _state；
// 采集器未创建时直接返回 false
bool AndroidDeviceManager::stopCapture() {
    if (_capturer == NULL) {
        return false;
    }
    _capturer->setState(arlive::VideoState::Inactive);
    _state = arlive::VideoState::Inactive;
    return true;
}

// 销毁采集器：reset() 触发 VideoCameraCapturer 的析构（停止摄像头/释放相关资源），
// 随后显式置 nullptr（reset() 之后 unique_ptr 本身已经是 nullptr，这里是双重保险写法）
void AndroidDeviceManager::destoryCapture() {
    if (_capturer != nullptr) {
        _capturer.reset();
        _capturer = nullptr;
    }
}

// 设置美颜总开关：通过 JNI 反射调用 Java 层 VideoCapturerDevice.needBeautyEffect(boolean)
int AndroidDeviceManager::setBeautyEffect(bool enable) {

    // 确保当前线程已附着到 JVM
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    // 取出 AndroidContext，用于访问 Java 层的 VideoCapturerDevice 对象与类
    auto context = (AndroidContext *) _platformContext.get();
    // 反射查找方法 needBeautyEffect，签名 "(Z)V" 表示：接受 1 个 boolean 参数，返回 void
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "needBeautyEffect",
                                          "(Z)V");
    // 调用该 Java 方法，把 enable 转为 jboolean 传入
    env->CallVoidMethod(context->getJavaCapturer(), methodId, (jboolean) enable);
    return anyrtc::ArLiveCode::ArLIVE_OK;
}

// 设置美白级别：通过 JNI 反射调用 Java 层 VideoCapturerDevice.setWhitenessLevel(float)
int AndroidDeviceManager::setWhitenessLevel(float level) {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "(F)V" 表示：接受 1 个 float 参数，返回 void
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "setWhitenessLevel",
                                          "(F)V");
    env->CallVoidMethod(context->getJavaCapturer(), methodId, (jfloat) level);
    // 注意：此处返回值为 ArLIVE_ERROR_REFUSED，与其余几个设置类方法返回 ArLIVE_OK 不一致，
    // 但因为不涉及逻辑修改，此处只做说明不做改动
    return anyrtc::ArLiveCode::ArLIVE_ERROR_REFUSED;
}

// 设置磨皮/美颜级别：通过 JNI 反射调用 Java 层 VideoCapturerDevice.setBeautyLevel(float)
int AndroidDeviceManager::setBeautyLevel(float level) {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "(F)V"：1 个 float 参数，返回 void
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "setBeautyLevel",
                                          "(F)V");
    env->CallVoidMethod(context->getJavaCapturer(), methodId, (jfloat) level);
    return anyrtc::ArLiveCode::ArLIVE_OK;
}

// 设置色调/红润级别：通过 JNI 反射调用 Java 层 VideoCapturerDevice.setToneLevel(float)
int AndroidDeviceManager::setToneLevel(float level) {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "(F)V"：1 个 float 参数，返回 void
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "setToneLevel", "(F)V");
    env->CallVoidMethod(context->getJavaCapturer(), methodId, (jfloat) level);
    return anyrtc::ArLiveCode::ArLIVE_OK;
}

// 恢复摄像头默认状态：通过 JNI 反射调用 Java 层 VideoCapturerDevice.recoverCamera()
void AndroidDeviceManager::recoverCamera() {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "()V"：无参数，返回 void
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "recoverCamera", "()V");
    env->CallVoidMethod(context->getJavaCapturer(), methodId);
}




// 手写（非自动生成）的 JNI 导出函数，供 Java 层 VideoCapturerDevice 类通过
// native 方法 nativeGetJavaVideoCapturerObserver 调用。
// 函数名遵循 JNI 命名规范：Java_<包名（.替换为_）>_<类名>_<方法名>
extern "C" {
JNIEXPORT jobject
Java_io_anyrtc_live_internal_VideoCapturerDevice_nativeGetJavaVideoCapturerObserver(JNIEnv *env,
                                                                                    jclass clazz,
                                                                                    jlong ptr) {
    // 注意：AndroidDeviceManager() 会先构造一个临时的局部对象，再调用其 Inst()，
    // 但由于 Inst() 是静态方法，实际访问的仍然是 gInstance 单例，
    // 这个临时对象构造后立即被丢弃，只是写法上略显多余（不影响正确性，此处不做修改）；
    // GetJavaVideoCapturerObserver 返回 ScopedJavaLocalRef，Release() 会放弃其自动释放权，
    // 把裸的 local ref 所有权转交给 JNI 调用方（Java 侧），避免 ScopedJavaLocalRef
    // 析构时提前删除这个即将返回给 Java 的引用
    return AndroidDeviceManager().Inst().GetJavaVideoCapturerObserver(env).Release();
}
}