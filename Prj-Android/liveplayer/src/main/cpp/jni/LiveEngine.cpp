// ================== 头文件引入 ==================
#include <jni.h>                                          // JNI 基础类型与函数声明（JNIEnv、jobject、jlong 等）
#include <string>                                          // std::string
#include <sdk/android/src/jni/jni_helpers.h>               // WebRTC 提供的 JNI 辅助函数（如 JavaToStdString）
#include <rtc_base/ssl_adapter.h>                          // WebRTC SSL 适配层（网络加密相关，本文件未直接使用但依赖其初始化链路）
#include <sdk/android/native_api/video/wrapper.h>          // WebRTC Android 视频封装（view_t 等类型定义）
#include <modules/utility/include/jvm_android.h>           // webrtc::JVM，用于给 WebRTC 内部注入 JavaVM
#include <sdk/android/native_api/base/init.h>              // webrtc::InitAndroid，WebRTC Android 平台初始化
#include <memory>                                          // std::shared_ptr / std::unique_ptr
#include <utility>                                         // std::move 等工具
#include <map>                                             // std::map（本文件未见直接使用，可能供头文件依赖）
#include "ArLive2Engine.h"                                 // IArLive2Engine 引擎接口定义
#include "IArLivePusher.hpp"                               // IArLivePusher 推流器接口定义
#include <android/AndroidContext.h>                        // AndroidContext 平台上下文实现
#include <android/native_api/jni/class_loader.h>           // Android 端 Java 类加载辅助
#include "liveEngine/ArLivePushEvent.h"                    // LivePushEvent：推流回调事件转发到 Java 层
#include "liveEngine/ArLivePlayEvent.h"                    // LivePlayEvent：播放回调事件转发到 Java 层
#include "media/engine/webrtc_media_engine.h"              // WebRTC 媒体引擎相关声明
#include "libyuv.h"                                        // libyuv 图像格式转换库（I420<->ABGR 等）
#include "util/ClassreferenceHolder.h"                     // webrtc_loader::LoadGlobalClassReferenceHolder，缓存 Java 类/方法引用
#include "liveEngine/AndroidDeviceManager.h"               // AndroidDeviceManager：摄像头/美颜/编码器等设备能力管理单例

// extern "C" 代码块 1：WebRTC 初始化与 InstanceHolder 辅助结构/函数
// 使用 extern "C" 防止 C++ 名称修饰，保证下面声明的符号可以按普通 C 符号被链接（虽然这里都是内部使用的非 JNIEXPORT 函数）
extern "C" {
using namespace arlive;                                    // arlive 命名空间：推流/播放相关类型（ArLiveXxx）
using namespace anyrtc;                                    // anyrtc 命名空间：另一部分引擎类型
bool webrtcLoaded = false;                                 // 全局标记：WebRTC Android 环境是否已初始化，避免重复初始化
jclass NativeInstance;                                     // 全局缓存的 Java 类 io/anyrtc/live/internal/NativeInstance 的 jclass（全局引用）

// initWebRTC：WebRTC Android 环境的懒加载初始化函数，仅在第一次调用时真正执行初始化逻辑
void initWebRTC(JNIEnv *env) {
    if (webrtcLoaded) {
        return;                                            // 已经初始化过，直接返回，避免重复初始化导致崩溃或资源泄漏
    }
    JavaVM *vm;                                             // 用于保存当前进程的 JavaVM 指针
    env->GetJavaVM(&vm);                                    // 从当前 JNIEnv 反查所属的 JavaVM
    webrtc::InitAndroid(vm);                                // 初始化 WebRTC 的 Android 平台相关组件（JNI 环境、Context 等）
    webrtc::JVM::Initialize(vm);                            // 将 JavaVM 注入 WebRTC 内部的 JVM 单例，供内部模块回调 Java 层使用
    webrtcLoaded = true;                                    // 标记初始化完成，后续调用直接跳过
    NativeInstance = static_cast<jclass>(env->NewGlobalRef( // 查找 NativeInstance 类并创建全局引用（局部引用在函数返回后会失效，必须转为全局引用长期持有）
            env->FindClass("io/anyrtc/live/internal/NativeInstance")));
    webrtc_loader::LoadGlobalClassReferenceHolder();        // 预加载并缓存 WebRTC 内部用到的 Java 类/方法/字段引用，提升后续 JNI 调用性能
}


// InstanceHolder：native 侧保存的“实例句柄”结构体，Java 层通过 jlong（nativePtr 字段）持有其地址
struct InstanceHolder {
    std::shared_ptr<PlatformContext> _platformContext;     // 平台上下文（封装 Android Context/JNIEnv 等），供引擎与设备管理器使用
    IArLive2Engine *arLiveEngine;                           // 引擎主对象指针，负责创建/释放推流器、播放器
};

// getInstanceHolderId：从 Java 侧的 NativeInstance 对象中读取其 nativePtr 字段（long 类型），即 InstanceHolder 的地址
jlong getInstanceHolderId(JNIEnv *env, jobject obj) {
    return env->GetLongField(obj, env->GetFieldID(NativeInstance, "nativePtr", "J")); // 反射获取字段 ID 后读取 long 值；"J" 是 JNI 类型签名，表示 long
}

// getInstanceHolder：将 nativePtr（jlong）还原为 InstanceHolder* 指针，便于在 native 层直接操作
InstanceHolder *getInstanceHolder(JNIEnv *env, jobject obj) {
    return reinterpret_cast<InstanceHolder *>(getInstanceHolderId(env, obj)); // reinterpret_cast：将整型句柄按位重新解释为指针，这是 JNI 中常见的“句柄化”对象传递方式
}


}

// extern "C" 代码块 2：引擎生命周期相关 JNI 导出函数（创建实例、前后台切换等）
extern "C" {
// 对应 Java: NativeInstance.makeNativeInstance(NativeInstance instance)
// 功能：创建并初始化整个 native 引擎实例，返回其地址作为句柄（jlong）保存到 Java 对象的 nativePtr 字段
// 参数：obj 为调用该 native 方法的 NativeInstance 对象本身；instance 为传入的（同一个）NativeInstance 对象引用，供 AndroidContext 回调使用
// 返回值：新建的 InstanceHolder 的地址，转换为 jlong 供 Java 侧保存
JNIEXPORT jlong JNICALL
Java_io_anyrtc_live_internal_NativeInstance_makeNativeInstance(JNIEnv *env, jobject obj, jobject instance) {
    initWebRTC(env);                                        // 确保 WebRTC Android 环境已初始化（懒加载，仅第一次真正执行）
    auto *holder = new InstanceHolder();                    // 在堆上创建 InstanceHolder，其生命周期由 Java 侧通过 nativeRelease 手动管理（无自动析构）
    holder->arLiveEngine = V2_CALL::createArLive2Engine();  // 调用引擎工厂方法创建 IArLive2Engine 实例
    std::shared_ptr<PlatformContext> platformContext;       // 声明平台上下文智能指针，用于跨对象共享同一个 Android 上下文
    platformContext = std::make_shared<AndroidContext>(env,obj, false); // 构造 AndroidContext，封装 JNIEnv/Java 对象引用，false 可能表示某种非 Activity/非UI模式标志
    holder->_platformContext = platformContext;             // 保存到 holder，随 holder 生命周期持有
    AndroidDeviceManager::Inst().setPlatformContext(platformContext); // 将同一个平台上下文注入设备管理器单例，供摄像头/美颜等功能使用
    holder->arLiveEngine->initialize(NULL);                 // 初始化引擎，参数 NULL 表示使用默认配置
    return reinterpret_cast<jlong>(holder);                 // 将 holder 指针按位转换为 jlong 返回给 Java，作为后续调用的句柄
}

// 对应 Java: NativeInstance.nativeSetAppInBackground(boolean background)
// 功能：通知引擎当前 App 是否进入后台，便于引擎调整推流/播放策略（如降帧率、暂停渲染等）
// 参数：background 为 true 表示进入后台，false 表示回到前台
// 返回值：0 表示成功，-1 表示引擎未初始化
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetAppInBackground(JNIEnv *env, jobject obj,
                                                                     jboolean background) {
    InstanceHolder *instance = getInstanceHolder(env, obj); // 从 Java 对象取回 InstanceHolder 指针
    if (instance->arLiveEngine == nullptr) {                // 引擎为空说明尚未创建/已释放
        return (jint) -1;                                   // 返回 -1 表示失败
    }
    instance->arLiveEngine->setAppInBackground(background); // 转发前后台状态给引擎
    return (jint) 0;                                        // 返回 0 表示成功
}




// 对应 Java: NativeInstance.nativeCreatePushKit()
// 功能：创建一个推流器（IArLivePusher）并绑定基于 Android MediaCodec 的硬件视频编码器工厂
// 返回值：新建推流器对象的地址（jlong），失败（引擎未初始化）时以 jint -1 的位模式返回（注意：函数声明返回 jlong，这里返回 (jint)-1 会被隐式转换/截断，属于原有实现细节，未做修改）
JNIEXPORT jlong JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeCreatePushKit(JNIEnv *env, jobject obj) {
    InstanceHolder *instance = getInstanceHolder(env, obj); // 取回引擎句柄
    if (instance->arLiveEngine == nullptr){                 // 引擎未创建，无法继续
        return (jint)-1;                                    // 返回失败标记
    }
    IArLivePusher *arLivePushKit = instance->arLiveEngine->createArLivePusher(NULL); // 由引擎创建推流器对象，NULL 表示默认配置
    std::unique_ptr<webrtc::VideoEncoderFactory> video_encoder_factory = AndroidDeviceManager::Inst().makeVideoEncoderFactory(); // 从设备管理器获取一个（可能基于硬件编码的）视频编码器工厂
    arLivePushKit->setExVideoEncoderFactory(video_encoder_factory.release()); // release() 交出 unique_ptr 的所有权，将裸指针传给推流器，后续生命周期由推流器内部管理
    return reinterpret_cast<jlong>(arLivePushKit);          // 将推流器指针转为 jlong 句柄返回给 Java
}

// 对应 Java: NativeInstance.nativeSetPushObserver(Object observer, long nativePtr)
// 功能：为指定推流器设置事件回调对象，native 侧事件会通过 LivePushEvent 转发给 Java 层的 observer
// 参数：observer 为 Java 层实现的回调接口对象；nativePtr 为推流器句柄
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetPushObserver(JNIEnv *env, jobject obj,jobject observer,jlong nativePtr) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 将 jlong 句柄还原为 IArLivePusher 指针
    if (arLivePushKit != NULL){                             // 推流器有效才设置观察者
        LivePushEvent *pushEvent = new LivePushEvent(observer); // 新建事件转发对象，持有 Java observer 引用；注意：此处 new 出的对象未见对应 delete，其生命周期由 arLivePushKit 内部管理（此处不做修改，仅记录）
        arLivePushKit->setObserver(pushEvent);              // 将事件转发对象设置为推流器的观察者，后续 native 事件会调用其方法回调到 Java
    }
}


// 对应 Java: NativeInstance.nativeStartPush(long nativePtr, String pushUrl)
// 功能：开始推流到指定的推流地址（RTMP/WebRTC 等 URL）
// 参数：pushUrl 为推流地址字符串
// 返回值：底层 startPush 的结果码，推流器为空时返回 -1
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStartPush(JNIEnv *env, jobject obj,jlong nativePtr,jstring pushUrl) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit != NULL){
        std::string strPushUrl = webrtc::JavaToStdString(env, pushUrl); // 将 Java String 转换为 std::string（内部会做 UTF-16 到 UTF-8 的转换）
        int result =arLivePushKit->startPush(strPushUrl.c_str()); // 调用 native 推流接口，传入 C 字符串形式的推流地址
        return (jint)result;                                // 返回结果码
    }
    return (jint)-1;                                        // 推流器无效，返回失败
}

// 对应 Java: NativeInstance.nativeStopPush(long nativePtr)
// 功能：停止当前推流
// 返回值：底层 stopPush 的结果码，失败时返回 -1
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStopPush(JNIEnv *env, jobject obj,jlong nativePtr) {
    InstanceHolder *instance = getInstanceHolder(env, obj); // 取回引擎句柄（用于校验引擎仍然存活）
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针

    if (arLivePushKit != NULL&&instance->arLiveEngine!=NULL){ // 推流器和引擎都必须有效
        int result =arLivePushKit->stopPush();              // 调用底层停止推流
        return (jint)result;                                // 返回结果码
    }
    return (jint)-1;                                        // 任一为空则返回失败
}

// 对应 Java: NativeInstance.nativeReleasePusher(long nativePtr)
// 功能：释放推流器对象，交由引擎内部销毁
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeReleasePusher(JNIEnv *env, jobject obj,jlong nativePtr) {
    InstanceHolder *instance = getInstanceHolder(env, obj); // 取回引擎句柄
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit != NULL&&instance->arLiveEngine!=NULL){ // 两者都有效才执行释放，避免空指针访问
        instance->arLiveEngine->releaseArLivePusher(arLivePushKit); // 由引擎负责销毁推流器及其内部资源
    }
}




// 对应 Java: NativeInstance.nativeIsPushing(long nativePtr)
// 功能：查询当前推流器是否正在推流
// 返回值：1/0 表示是否推流中（bool 转 jint），推流器为空时返回 -1
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeIsPushing(JNIEnv *env, jobject obj,jlong nativePtr) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!=NULL){
        bool result = arLivePushKit->isPushing();           // 查询推流状态
        return (jint)result;                                // bool -> jint（true 为 1，false 为 0）
    }
    return (jint)-1;                                        // 推流器无效返回 -1

}





// 对应 Java: NativeInstance.nativeRelease()
// 功能：释放整个引擎实例（InstanceHolder），是与 makeNativeInstance 配对的销毁函数
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeRelease(JNIEnv *env, jobject obj) {
    InstanceHolder *instance = getInstanceHolder(env, obj); // 取回引擎句柄
    if (instance == NULL){                                  // 已经为空说明未创建或已释放
        return;                                             // 直接返回，避免重复释放
    }
    if (instance->arLiveEngine != NULL){                    // 引擎存在则先释放引擎内部资源
        instance->arLiveEngine->release();                  // 调用引擎自身的 release，回收其内部所有子对象（如未手动释放的推流器/播放器）
    }
    delete instance;                                        // 释放 InstanceHolder 本身（makeNativeInstance 中 new 出来的对象，在此完成配对销毁）
}


// 对应 Java: NativeInstance.nativeSetRenderView(long nativePtr, Object localSink)
// 功能：为推流器设置本地预览渲染视图（用于本地摄像头画面预览）
// 参数：localSink 通常是封装了 SurfaceView/TextureView 的渲染 sink 对象
// 返回值：设置结果码，推流器为空时返回 -1
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetRenderView(JNIEnv *env, jobject obj, jlong nativePtr,jobject localSink) {
    jint result = -1;                                       // 默认返回值，-1 表示失败/未处理
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->setRenderView(reinterpret_cast<view_t *>(localSink)); // 将 Java 侧渲染对象指针重新解释为 native 的 view_t*，设置为本地预览视图
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeSetPushRenderRotation(long nativeHandle, int rotation)
// 功能：设置推流本地预览画面的渲染旋转角度
// 参数：rotation 为角度枚举值（对应 ArLiveRotation，如 0/90/180/270 度）
// 返回值：设置结果码，推流器为空时返回 -1
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetPushRenderRotation(JNIEnv *env, jobject thiz,jlong nativeHandle,jint rotation
) {
    jint result = -1;                                       // 默认返回失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativeHandle); // 还原推流器指针
    if (arLivePushKit != NULL){
        result = arLivePushKit->setRenderRotation(static_cast<ArLiveRotation>(rotation)); // 将 jint 转换为 ArLiveRotation 枚举后设置旋转角度
    }
    return (jint)result;
}


// 对应 Java: NativeInstance.nativeStartCamera(long nativePtr, boolean isFront)
// 功能：开启摄像头采集
// 参数：isFront 为 true 表示使用前置摄像头，false 表示后置摄像头
// 返回值：结果码，推流器为空时返回 -1
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStartCamera(JNIEnv *env, jobject obj,jlong nativePtr,jboolean isFront) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->startCamera(isFront);        // 启动摄像头采集，指定前后置
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeStopCamera(long nativePtr)
// 功能：停止摄像头采集
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStopCamera(JNIEnv *env, jobject obj,jlong nativePtr) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->stopCamera();                // 停止摄像头采集
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeSetEncoderMirror(long nativePtr, boolean mirror)
// 功能：设置编码器输出（即远端看到的画面）是否镜像
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetEncoderMirror(JNIEnv *env, jobject obj,jlong nativePtr,jboolean mirror) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->setEncoderMirror(mirror);    // 设置编码端镜像开关
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeStartMicrophone(long nativePtr)
// 功能：开启麦克风采集音频
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStartMicrophone(JNIEnv *env, jobject obj,jlong nativePtr) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->startMicrophone();           // 启动麦克风采集
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeStopMicrophone(long nativePtr)
// 功能：停止麦克风采集音频
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStopMicrophone(JNIEnv *env, jobject obj,jlong nativePtr) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->stopMicrophone();            // 停止麦克风采集
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeSetBeautyEffect(boolean enable)
// 功能：开启/关闭美颜效果（全局设备级设置，不依赖具体推流器句柄）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetBeautyEffect(JNIEnv *env, jobject obj,jboolean enable) {
    return (jint)AndroidDeviceManager::Inst().setBeautyEffect(enable); // 直接转发给设备管理器单例处理美颜开关
}

// 对应 Java: NativeInstance.nativeSetWhitenessLevel(float level)
// 功能：设置美白等级
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetWhitenessLevel(JNIEnv *env, jobject obj,
                                                                    jfloat level) {
    return (jint)AndroidDeviceManager::Inst().setWhitenessLevel(level); // 转发给设备管理器设置美白强度
}
// 对应 Java: NativeInstance.nativeSetBeautyLevel(float level)
// 功能：设置磨皮/美颜强度等级
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetBeautyLevel(JNIEnv *env, jobject obj,
                                                                 jfloat level) {
    return (jint)AndroidDeviceManager::Inst().setBeautyLevel(level); // 转发给设备管理器设置美颜强度
}
// 对应 Java: NativeInstance.nativeSetToneLevel(float level)
// 功能：设置肤色/色调调节等级
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetToneLevel(JNIEnv *env, jobject obj,
                                                               jfloat level) {
    return (jint)AndroidDeviceManager::Inst().setToneLevel(level); // 转发给设备管理器设置色调强度
}


// 对应 Java: NativeInstance.nativeStartVirtualCamera(long nativePtr, int type, byte[] bitmap, int width, int height)
// 功能：启动“虚拟摄像头”——用一张静态图片替代真实摄像头画面进行推流（例如占位图/背景图场景）
// 参数：type 为图片类型（对应 ArLiveImageType，如 RGBA/JPEG 等）；bitmap 为图片原始字节数据；width/height 为图片宽高
// 返回值：结果码，推流器为空时返回 -1
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStartVirtualCamera(JNIEnv *env, jobject thiz,
                                                                     jlong nativePtr,jint type,
                                                                     jbyteArray bitmap, jint width,
                                                                     jint height) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        jbyte* bufferPtr=(env)->GetByteArrayElements(bitmap, NULL); // 获取 Java byte[] 的底层指针（可能是拷贝也可能是直接指针，取决于 JVM 实现），第二参数 isCopy 传 NULL 表示不关心是否发生了拷贝
        ArLiveImage *image = new ArLiveImage();              // 在堆上创建图片描述结构体；注意：未见对应的 delete，其生命周期交由 startVirtualCamera 内部处理或存在潜在泄漏，此处不做修改，仅记录
        image->imageType = static_cast<ArLiveImageType>(type); // 将 jint 转换为图片类型枚举
        image->imageWidth = width;                           // 设置图片宽度
        image->imageHeight = height;                         // 设置图片高度
        image->imageSrc = reinterpret_cast<const char *>(bufferPtr); // 将 jbyte* 重新解释为 const char*，作为图片原始数据指针
        result = arLivePushKit->startVirtualCamera(image);   // 调用底层接口，用该图片启动虚拟摄像头
        env->ReleaseByteArrayElements(bitmap, bufferPtr, 0); // 释放/归还数组元素访问；模式 0 表示将缓冲区内容拷回原数组（若发生了拷贝）并释放缓冲区，这是与 GetByteArrayElements 配对的必要操作，否则可能造成内存泄漏或 JVM 堆锁定
    }
    return (jint)result;
}
// 对应 Java: NativeInstance.nativeStopVirtualCamera(long nativePtr)
// 功能：停止虚拟摄像头，恢复正常摄像头采集（若之前有开启）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStopVirtualCamera(JNIEnv *env, jobject thiz, jlong nativePtr) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->stopVirtualCamera();         // 停止虚拟摄像头
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativePausePusherAudio(long nativePtr)
// 功能：暂停推流侧的音频采集/发送
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativePausePusherAudio(JNIEnv *env, jobject obj,jlong nativePtr) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->pauseAudio();                // 暂停音频
    }
    return (jint)result;
}
// 对应 Java: NativeInstance.nativeResumePusherAudio(long nativePtr)
// 功能：恢复推流侧的音频采集/发送
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeResumePusherAudio(JNIEnv *env, jobject obj,jlong nativePtr) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->resumeAudio();               // 恢复音频
    }
    return (jint)result;
}
// 对应 Java: NativeInstance.nativePausePusherVideo(long nativePtr)
// 功能：暂停推流侧的视频采集/发送（通常会推送最后一帧或黑帧占位，具体由底层实现决定）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativePausePusherVideo(JNIEnv *env, jobject obj,jlong nativePtr) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->pauseVideo();                // 暂停视频
    }
    return (jint)result;
}
// 对应 Java: NativeInstance.nativeResumePusherVideo(long nativePtr)
// 功能：恢复推流侧的视频采集/发送
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeResumePusherVideo(JNIEnv *env, jobject obj,jlong nativePtr) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->resumeVideo();               // 恢复视频
    }
    return (jint)result;
}


// 对应 Java: NativeInstance.switchCamera(boolean isFront)
// 功能：切换前后摄像头（设备级操作，直接作用于设备管理器单例，不区分具体推流器）
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_switchCamera(JNIEnv *env, jobject obj,jboolean isFront) {
        AndroidDeviceManager::Inst().switchCamera(isFront);  // 转发给设备管理器执行摄像头切换
}



// 对应 Java: NativeInstance.nativeSetVideoQuality(long nativePtr, int videoResolution, int videoResolutionMode, int videoFps, int videoBitrate, int minVideoBitrate, int scaleMode)
// 功能：设置推流视频编码参数（分辨率、分辨率模式、帧率、码率范围、缩放模式）
// 返回值：结果码，推流器为空时返回 -1
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetVideoQuality(JNIEnv *env, jobject obj,jlong nativePtr,jint videoResolution,jint videoResolutionMode,jint videoFps,jint videoBitrate,jint minVideoBitrate,jint scaleMode) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        AR::ArLiveVideoEncoderParam param = anyrtc::ArLiveVideoEncoderParam(ArLiveVideoResolution640x480); // 以 640x480 分辨率为基准构造默认参数对象，随后逐项覆盖
        param.videoResolution = static_cast<ArLiveVideoResolution>(videoResolution); // 覆盖：目标分辨率档位（枚举）
        param.videoResolutionMode = static_cast<ArLiveVideoResolutionMode>(videoResolutionMode); // 覆盖：分辨率模式（如横屏/竖屏自适应）
        param.videoBitrate = videoBitrate;                   // 覆盖：目标视频码率（kbps）
        param.videoFps  = videoFps;                          // 覆盖：目标视频帧率
        param.minVideoBitrate = minVideoBitrate;             // 覆盖：最小视频码率（码率自适应下限）
        param.videoScaleMode= static_cast<ArLiveVideoScaleMode>(scaleMode); // 覆盖：画面缩放/裁剪模式
        result = arLivePushKit->setVideoQuality(param);      // 将组装好的参数应用到推流器
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeSetAudioQuality(long nativePtr, int mode)
// 功能：设置推流音频质量档位（对应 ArLiveAudioQuality 枚举，如流畅/默认/高音质）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetAudioQuality(JNIEnv *env, jobject obj,jlong nativePtr,jint mode) {
    jint result = -1;                                       // 默认失败
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!= NULL){
        result = arLivePushKit->setAudioQuality(static_cast<ArLiveAudioQuality>(mode)); // jint 转枚举后设置音频质量
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativePusherEnableVolumeEvaluation(long nativePtr, int interval_ms)
// 功能：开启/配置推流端音量大小回调（用于说话人音量指示等场景）
// 参数：interval_ms 为回调间隔（毫秒），通常 <=0 表示关闭
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativePusherEnableVolumeEvaluation(JNIEnv *env,
                                                                               jobject thiz,jlong nativePtr,
                                                                               jint interval_ms) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    jint result = -1;                                       // 默认失败
    if (arLivePushKit!= NULL){
        result = arLivePushKit->enableVolumeEvaluation(interval_ms); // 设置音量回调间隔
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeSendSeiMessage(long nativePtr, int var1, byte[] var2)
// 功能：在推流视频帧中插入 SEI（补充增强信息）自定义消息，可用于音视频同步的自定义数据通道
// 参数：var1 为 SEI payload type（负载类型标识）；var2 为要发送的原始字节数据
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSendSeiMessage(JNIEnv *env, jobject thiz,jlong nativePtr,
                                                                 jint var1, jbyteArray var2) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    jint result = -1;                                       // 默认失败
    if (arLivePushKit!= NULL){
        jbyte* bufferPtr=(env)->GetByteArrayElements(var2, NULL); // 获取 SEI 数据的底层字节指针
        jint size = env->GetArrayLength(var2);               // 获取数组长度，作为数据字节数传给底层接口
        result = arLivePushKit->sendSeiMessage(var1, reinterpret_cast<const uint8_t *>(bufferPtr), size); // 发送 SEI 消息：payload type + 数据指针（重解释为 uint8_t*）+ 长度
        env->ReleaseByteArrayElements(var2, bufferPtr, 0);   // 释放数组元素访问，模式 0 表示拷回并释放缓冲区（与 Get 配对，防止内存/引用泄漏）
    }
    return (jint)result;

}

// 对应 Java: NativeInstance.nativeEnableCustomVideoCapture(long nativePtr, boolean var1)
// 功能：开启/关闭“自定义视频采集”模式——开启后不再使用内置摄像头采集，而是由业务方通过 nativeSendCustomVideoFrame 主动喂入视频帧
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeEnableCustomVideoCapture(JNIEnv *env,
                                                                           jobject thiz,
                                                                           jlong nativePtr,
                                                                           jboolean var1) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    jint result = -1;                                       // 默认失败
    if (arLivePushKit!= NULL){
        result = arLivePushKit->enableCustomVideoCapture(var1); // 开启/关闭自定义视频采集模式
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeSendCustomVideoFrame(long nativePtr, int pixel_format, int buffer_type, byte[] data, Object buffer, int width, int height, int rotation, int stride)
// 功能：在自定义视频采集模式下，由 Java 层将一帧视频数据喂给推流器进行编码发送
// 参数：pixel_format/buffer_type 描述数据的像素格式与内存布局；data 为帧原始字节数据（buffer 参数在本实现中未被使用）；width/height 为帧宽高；rotation 为帧旋转角度；stride 为行跨度（每行字节数，用于处理内存对齐）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSendCustomVideoFrame(JNIEnv *env, jobject thiz,
                                                                       jlong nativePtr,
                                                                       jint pixel_format,
                                                                       jint buffer_type,
                                                                       jbyteArray data,
                                                                       jobject buffer, jint width,
                                                                       jint height, jint rotation,jint stride) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    jint result = -1;                                       // 默认失败
    if (arLivePushKit!= NULL){
        ArLiveVideoFrame *frame = new ArLiveVideoFrame();    // 在堆上创建视频帧描述对象；注意：未见配对的 delete，帧数据的生命周期依赖 sendCustomVideoFrame 内部处理（可能同步拷贝使用后不需要调用方释放），此处不做修改，仅记录
        frame->pixelFormat = static_cast<ArLivePixelFormat>(pixel_format); // jint 转换为像素格式枚举（如 NV21/I420/RGBA 等）
        frame->bufferType = static_cast<ArLiveBufferType>(buffer_type); // jint 转换为缓冲区类型枚举（如原始字节数组/Texture 等）
        frame->width = width;                                // 帧宽度
        frame->height = height;                              // 帧高度
        frame->rotation = static_cast<ArLiveRotation>(rotation); // 帧旋转角度枚举
        jbyte* arrayData=(env)->GetByteArrayElements(data, NULL); // 获取 Java byte[] 帧数据的底层指针
        frame->data = reinterpret_cast<char *>(arrayData);  // 重解释为 char*，赋给帧结构体的数据字段
        frame->stride = stride;                              // 设置行跨度
        result = arLivePushKit->sendCustomVideoFrame(frame); // 将该帧送入推流器进行编码发送（推测为同步调用，函数返回前已完成对 data 的读取）
        env->ReleaseByteArrayElements(data, arrayData, 0);   // 释放/归还数组元素访问，模式 0 表示拷回（若有拷贝）并释放，是 Get 的必要配对操作
    }
    return (jint)result;
}


// 对应 Java: NativeInstance.nativeEnableCustomAudioCapture(long nativePtr, boolean enable)
// 功能：开启/关闭“自定义音频采集”模式——开启后不再使用内置麦克风采集，改由业务方通过 nativeSendCustomAudioFrame 主动喂入音频数据
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeEnableCustomAudioCapture(JNIEnv *env,
                                                                           jobject thiz,jlong nativePtr,
                                                                           jboolean enable) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    jint result  = -1;                                      // 默认失败
    if (arLivePushKit!= NULL){
        result = arLivePushKit->enableCustomAudioCapture(enable); // 开启/关闭自定义音频采集模式
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeSendCustomAudioFrame(long nativePtr, int channel, int sample_rate, byte[] data)
// 功能：在自定义音频采集模式下，将一段 PCM 音频数据喂给推流器进行编码发送
// 参数：channel 为声道数；sample_rate 为采样率；data 为 PCM 原始字节数据
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSendCustomAudioFrame(JNIEnv *env, jobject thiz,
                                                                       jlong nativePtr,
                                                                       jint channel,
                                                                       jint sample_rate,
                                                                       jbyteArray data) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    jint result  = -1;                                      // 默认失败
    if (arLivePushKit!= NULL){
        jbyte* bufferPtr=(env)->GetByteArrayElements(data, NULL); // 获取音频数据的底层指针
        jint size = env->GetArrayLength(data);               // 获取数组长度作为音频数据字节数
        ArLiveAudioFrame *audioFrame = new ArLiveAudioFrame(); // 在堆上创建音频帧描述对象；同样未见配对 delete，此处仅记录不修改
        audioFrame->data = reinterpret_cast<char *>(bufferPtr); // 重解释为 char*，赋给音频帧数据字段
        audioFrame->channel = channel;                       // 设置声道数
        audioFrame->sampleRate = sample_rate;                // 设置采样率
        audioFrame->length = size;                           // 设置数据长度（字节数）
        result = arLivePushKit->sendCustomAudioFrame(audioFrame); // 将该音频帧送入推流器进行编码发送
        env->ReleaseByteArrayElements(data, bufferPtr, 0);   // 释放/归还数组元素访问，模式 0 表示拷回并释放
    }
    return (jint)result;
}




// 对应 Java: NativeInstance.startScreenCapture(long nativePtr)
// 功能：启动屏幕录制/共享采集，作为推流的视频源
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_startScreenCapture(JNIEnv *env, jobject obj,jlong nativePtr) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!=NULL){
        arLivePushKit->startScreenCapture();                 // 启动屏幕采集（具体 MediaProjection 授权等应已在 Java 层完成）
    }
}

// 对应 Java: NativeInstance.stopScreenCapture(long nativePtr)
// 功能：停止屏幕录制/共享采集
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_stopScreenCapture(JNIEnv *env, jobject obj,jlong nativePtr) {
    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr); // 还原推流器指针
    if (arLivePushKit!=NULL){
        arLivePushKit->stopScreenCapture();                  // 停止屏幕采集
    }
}






}


// extern "C" 代码块 3：播放器（IArLivePlayer）相关 JNI 导出函数
extern "C" {

// 对应 Java: NativeInstance.nativeStartPlay(long native_ptr, String url)
// 功能：开始播放指定拉流地址
// 返回值：底层 startPlay 结果码，播放器为空时返回 -1
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStartPlay(JNIEnv *env, jobject thiz,
                                                            jlong native_ptr, jstring url) {

    int result = -1;                                        // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(native_ptr); // 将 jlong 句柄还原为 IArLivePlayer 指针
    if (arLivePlayKit !=NULL){
        std::string strUrl = webrtc::JavaToStdString(env, url); // 将 Java String 转为 std::string
        result = arLivePlayKit->startPlay(strUrl.c_str());  // 调用底层播放接口开始拉流播放
    }
    return (jint)result;
}


// 对应 Java: NativeInstance.nativeStopPlay(long nativeHandle)
// 功能：停止播放
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeStopPlay(JNIEnv *env, jobject thiz,
                                                            jlong nativeHandle) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->stopPlay();                  // 停止播放
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeIsPlaying(long nativeHandle)
// 功能：查询是否正在播放
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeIsPlaying(JNIEnv *env, jobject thiz,jlong nativeHandle
                                                          ) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->isPlaying();                 // 查询播放状态
    }
    return (jint)result;
}
// 对应 Java: NativeInstance.nativeSetRenderRotation(long nativeHandle, int rotation)
// 功能：设置播放端渲染画面的旋转角度
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetRenderRotation(JNIEnv *env, jobject thiz,jlong nativeHandle,jint rotation
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->setRenderRotation(static_cast<ArLiveRotation>(rotation)); // jint 转枚举后设置渲染旋转角度
    }
    return (jint)result;
}


// 对应 Java: NativeInstance.nativePauseAudio(long nativeHandle)
// 功能：暂停播放端音频输出
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativePauseAudio(JNIEnv *env, jobject thiz,jlong nativeHandle
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->pauseAudio();                // 暂停音频播放
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeResumeAudio(long nativeHandle)
// 功能：恢复播放端音频输出
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeResumeAudio(JNIEnv *env, jobject thiz,jlong nativeHandle
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->resumeAudio();               // 恢复音频播放
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeResumeVideo(long nativeHandle)
// 功能：恢复播放端视频渲染
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeResumeVideo(JNIEnv *env, jobject thiz,jlong nativeHandle
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->resumeVideo();               // 恢复视频渲染
    }
    return (jint)result;
}



// 对应 Java: NativeInstance.nativePauseVideo(long nativeHandle)
// 功能：暂停播放端视频渲染（通常停留在最后一帧）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativePauseVideo(JNIEnv *env, jobject thiz,jlong nativeHandle
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->pauseVideo();                // 暂停视频渲染
    }
    return (jint)result;
}


// 对应 Java: NativeInstance.nativeSetPlayoutVolume(long nativeHandle, int vom)
// 功能：设置播放音量（vom 为音量值，具体范围由业务约定，如 0~100）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetPlayoutVolume(JNIEnv *env, jobject thiz,jlong nativeHandle,jint vom
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->setPlayoutVolume(vom);       // 设置播放音量
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeSetCacheParams(long nativeHandle, float minTime, float maxTime)
// 功能：设置播放缓冲区的最小/最大缓存时长（秒），用于权衡卡顿与延迟
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetCacheParams(JNIEnv *env, jobject thiz,jlong nativeHandle,jfloat minTime,jfloat maxTime
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->setCacheParams(minTime,maxTime); // 设置最小/最大缓存时长
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeEnableVolumeEvaluation(long nativeHandle, int intervalMs)
// 功能：开启/配置播放端音量大小回调
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeEnableVolumeEvaluation(JNIEnv *env, jobject thiz,jlong nativeHandle,jint intervalMs
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->enableVolumeEvaluation(intervalMs); // 设置音量回调间隔
    }
    return (jint)result;
}


// 对应 Java: NativeInstance.nativeEnableCustomRendering(long nativeHandle, boolean enable, int format, int type)
// 功能：开启/关闭“自定义渲染”模式——开启后播放器不再自行渲染画面，而是通过回调把解码后的帧交给 Java 层自行渲染
// 参数：format/type 指定回调帧所需的像素格式与缓冲区类型（C 风格强制转换 (ArLivePixelFormat)/(ArLiveBufferType) 效果等同于 static_cast）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeEnableCustomRendering(JNIEnv *env, jobject thiz,jlong nativeHandle,jboolean enable,jint format,jint type
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->enableCustomRendering(enable,(ArLivePixelFormat)format,(ArLiveBufferType)type); // 设置自定义渲染开关及期望的像素格式/缓冲类型
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeEnableReceiveSeiMessage(long nativeHandle, boolean enable, int payloadType)
// 功能：开启/关闭接收 SEI 自定义消息（与推流端 nativeSendSeiMessage 配对使用），payloadType 用于过滤/标识特定类型的 SEI 数据
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeEnableReceiveSeiMessage(JNIEnv *env, jobject thiz,jlong nativeHandle,jboolean enable,jint payloadType
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->enableReceiveSeiMessage(enable,payloadType); // 设置 SEI 接收开关与关注的 payload 类型
    }
    return (jint)result;
}

// 对应 Java: NativeInstance.nativeSetPlayMode(long nativeHandle, int mode)
// 功能：设置播放模式（对应 ArLivePlayMode 枚举，如速度优先/流畅优先等）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetPlayMode(JNIEnv *env, jobject thiz,jlong nativeHandle,jint mode
) {
    jint result = -1;                                       // 默认失败
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        result = arLivePlayKit->setPlayMode(static_cast<ArLivePlayMode>(mode)); // jint 转枚举后设置播放模式
    }
    return (jint)result;
}



// 对应 Java: NativeInstance.nativeShowDebugView(long nativeHandle, boolean isShow)
// 功能：显示/隐藏播放端调试信息浮层（如码率、帧率、卡顿统计等）
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeShowDebugView(JNIEnv *env, jobject thiz,jlong nativeHandle,jboolean isShow
) {
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        arLivePlayKit->showDebugView(isShow);                // 设置调试视图显示开关
    }
}

// 对应 Java: NativeInstance.nativePlayKitRelease(long nativeHandle)
// 功能：释放播放器对象，交由引擎内部销毁
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativePlayKitRelease(JNIEnv *env, jobject thiz,jlong nativeHandle
) {
    InstanceHolder *instance = getInstanceHolder(env, thiz); // 取回引擎句柄
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (instance->arLiveEngine != NULL){                     // 注意：此处仅校验了引擎非空，未像其他函数一样同时判断 arLivePlayKit != NULL，若 nativeHandle 无效则可能向 releaseArLivePlayer 传入野指针/空指针，此处不做修改仅记录
       instance->arLiveEngine->releaseArLivePlayer(arLivePlayKit); // 由引擎负责销毁播放器及其内部资源
    }
}








// 对应 Java: NativeInstance.nativeCreatePlayKit()
// 功能：创建一个新的播放器（IArLivePlayer）实例
// 返回值：新建播放器对象地址（jlong），引擎未初始化时以 (jint)-1 位模式返回（与 nativeCreatePushKit 同样的实现细节，不做修改）
JNIEXPORT jlong JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeCreatePlayKit(JNIEnv *env, jobject obj) {
    InstanceHolder *instance = getInstanceHolder(env, obj); // 取回引擎句柄
    if (instance->arLiveEngine == nullptr){                 // 引擎未创建，无法继续
        return (jint)-1;                                    // 返回失败标记
    }
    IArLivePlayer* player = instance->arLiveEngine->createArLivePlayer(NULL); // 由引擎创建播放器对象，NULL 表示默认配置
   return reinterpret_cast<jlong>(player);                  // 将播放器指针转为 jlong 句柄返回给 Java
}

// 对应 Java: NativeInstance.nativeSetPlayerobserver(long nativeHandle, Object observer)
// 功能：为指定播放器设置事件回调对象，native 侧播放事件通过 LivePlayEvent 转发给 Java 层的 observer
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetPlayerobserver(JNIEnv *env, jobject thiz,jlong nativeHandle,jobject observer
) {
    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    if (arLivePlayKit != NULL){
        LivePlayEvent *event = new LivePlayEvent(observer);  // 新建播放事件转发对象，持有 Java observer 引用；同样未见对应 delete，生命周期由 arLivePlayKit 内部管理，此处仅记录不修改
        arLivePlayKit->setObserver(event);                   // 设置为播放器的观察者
    }
}




// 对应 Java: NativeInstance.nativeSetPlayerView(long nativeHandle, Object videoSink)
// 功能：为播放器设置渲染视图（用于显示拉流后的视频画面）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_nativeSetPlayerView(JNIEnv *env, jobject obj,jlong nativeHandle,jobject videoSink) {

    IArLivePlayer* arLivePlayKit = reinterpret_cast<IArLivePlayer *>(nativeHandle); // 还原播放器指针
    jint result = -1;                                       // 默认失败
    if (arLivePlayKit!= NULL){
        result = arLivePlayKit->setRenderView(reinterpret_cast<view_t *>(videoSink)); // 将 Java 侧渲染对象指针重新解释为 native 的 view_t*，设置为播放渲染视图
    }
    return (jint)result;
}

}


// extern "C" 代码块 4：LibYuvBridge —— 基于 libyuv 的 I420 <-> ABGR 像素格式转换桥接
// 这两个函数均通过 DirectByteBuffer（堆外直接内存）传递图像数据，避免了 JNI 数组拷贝开销
extern "C" {
// 对应 Java: org.webrtc.effector.format.LibYuvBridge.i420ToAbgrInternal(...)
// 功能：将 YUV420（I420，即 YUV 三平面各自独立存储）图像转换为 ABGR（即安卓 Bitmap 常用的字节序）格式
// 参数：dataYBuffer/dataUBuffer/dataVBuffer 为 Y/U/V 三个平面的直接缓冲区；strideY/strideU/strideV 为各平面的行跨度；
//      width/height 为图像宽高；outRgbaBuffer 为输出的 ABGR 结果直接缓冲区
JNIEXPORT void JNICALL
Java_org_webrtc_effector_format_LibYuvBridge_i420ToAbgrInternal(
        JNIEnv *env,
        jobject obj,
        jobject dataYBuffer, jint strideY,
        jobject dataUBuffer, jint strideU,
        jobject dataVBuffer, jint strideV,
        jint width, jint height,
        jobject outRgbaBuffer)
{
    uint8_t *data_y = (uint8_t*) env->GetDirectBufferAddress(dataYBuffer); // 获取 Y 平面直接缓冲区的原生内存地址（要求 Java 侧传入的是 DirectByteBuffer）
    uint8_t *data_u = (uint8_t*) env->GetDirectBufferAddress(dataUBuffer); // 获取 U 平面原生内存地址
    uint8_t *data_v = (uint8_t*) env->GetDirectBufferAddress(dataVBuffer); // 获取 V 平面原生内存地址
    uint8_t *out_rgba = (uint8_t *)(env->GetDirectBufferAddress(outRgbaBuffer)); // 获取输出 ABGR 缓冲区的原生内存地址（结果将写入此处）

    int stride_y = strideY;                                 // Y 平面每行字节数（行跨度）
    int stride_u = strideU;                                 // U 平面每行字节数
    int stride_v = strideV;                                 // V 平面每行字节数

    int dst_stride_rgba = width * 4;                         // 输出 ABGR 每行字节数：每像素 4 字节（A/B/G/R）
    int src_width = width;                                   // 源图像宽度
    int src_height = height;                                 // 源图像高度

    /*
    LIBYUV_API
    int I420ToABGR(const uint8_t* src_y,
                   int src_stride_y,
                   const uint8_t* src_u,
                   int src_stride_u,
                   const uint8_t* src_v,
                   int src_stride_v,
                   uint8_t* dst_abgr,
                   int dst_stride_abgr,
                   int width,
                   int height);
    */
    // 调用 libyuv 提供的 I420ToABGR 完成实际的像素格式转换（YUV -> RGB 色彩空间转换 + 内存重排）
    libyuv::I420ToABGR(data_y, stride_y,
               data_u, stride_u,
               data_v, stride_v,
               out_rgba, dst_stride_rgba,
               src_width, src_height);
}

// 对应 Java: org.webrtc.effector.format.LibYuvBridge.abgrToI420Internal(...)
// 功能：将 ABGR 图像转换为 YUV420（I420）三平面格式，是上面函数的逆过程
// 参数：rgbaBuffer 为输入 ABGR 直接缓冲区；outDataYBuffer/outDataUBuffer/outDataVBuffer 为输出的 Y/U/V 平面直接缓冲区及各自行跨度
JNIEXPORT void JNICALL
Java_org_webrtc_effector_format_LibYuvBridge_abgrToI420Internal(
        JNIEnv *env,
        jobject obj,
        jobject rgbaBuffer,
        jint width,
        jint height,
        jobject outDataYBuffer,
        jint strideY,
        jobject outDataUBuffer,
        jint strideU,
        jobject outDataVBuffer,
        jint strideV)
{
    uint8_t *rgba = (uint8_t*) env->GetDirectBufferAddress(rgbaBuffer); // 获取输入 ABGR 缓冲区的原生内存地址
    uint8_t *out_data_y = (uint8_t*) env->GetDirectBufferAddress(outDataYBuffer); // 获取输出 Y 平面缓冲区地址
    uint8_t *out_data_u = (uint8_t*) env->GetDirectBufferAddress(outDataUBuffer); // 获取输出 U 平面缓冲区地址
    uint8_t *out_data_v = (uint8_t*) env->GetDirectBufferAddress(outDataVBuffer); // 获取输出 V 平面缓冲区地址

    /*
    // ABGR little endian (rgba in memory) to I420.
    LIBYUV_API
    int ABGRToI420(const uint8* src_frame, int src_stride_frame,
                   uint8* dst_y, int dst_stride_y,
                   uint8* dst_u, int dst_stride_u,
                   uint8* dst_v, int dst_stride_v,
                   int width, int height);
    */
    // 调用 libyuv 提供的 ABGRToI420 完成 RGB -> YUV 色彩空间转换，并按三平面写出结果
    libyuv::ABGRToI420(rgba, width * 4,                      // 源 ABGR 行跨度为 width*4（每像素 4 字节）
               out_data_y, strideY,
               out_data_u, strideU,
               out_data_v, strideV,
               width, height);
}



}


//device manager
// extern "C" 代码块 5：设备管理（摄像头硬件能力）相关 JNI 导出函数，均直接转发给 AndroidDeviceManager 单例处理
extern "C"{
// 对应 Java: NativeInstance.enableCameraTorch(boolean enable)
// 功能：开启/关闭闪光灯（手电筒）
// 返回值：操作是否成功
JNIEXPORT jboolean JNICALL
Java_io_anyrtc_live_internal_NativeInstance_enableCameraTorch(JNIEnv *env, jobject thiz,
                                                              jboolean enable) {
    return AndroidDeviceManager::Inst().enableCameraTorch(enable); // 转发给设备管理器设置闪光灯开关
}

// 对应 Java: NativeInstance.getCameraZoomMaxRatio()
// 功能：获取当前摄像头支持的最大变焦倍率
JNIEXPORT jfloat JNICALL
Java_io_anyrtc_live_internal_NativeInstance_getCameraZoomMaxRatio(JNIEnv *env, jobject thiz) {
    return  AndroidDeviceManager::Inst().getCameraZoomMaxRatio(); // 转发给设备管理器查询最大变焦倍率
}

// 对应 Java: NativeInstance.setCameraZoomRatio(float var1)
// 功能：设置当前摄像头的变焦倍率
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_setCameraZoomRatio(JNIEnv *env, jobject thiz,
                                                               jfloat var1) {
    return  AndroidDeviceManager::Inst().setCameraZoomRatio(var1); // 转发给设备管理器设置变焦倍率
}

// 对应 Java: NativeInstance.isAutoFocusEnabled()
// 功能：查询摄像头自动对焦是否开启
JNIEXPORT jboolean JNICALL
Java_io_anyrtc_live_internal_NativeInstance_isAutoFocusEnabled(JNIEnv *env, jobject thiz) {
    return AndroidDeviceManager::Inst().isAutoFocusEnabled(); // 转发给设备管理器查询自动对焦状态
}

// 对应 Java: NativeInstance.enableCameraAutoFocus(boolean var1)
// 功能：开启/关闭摄像头自动对焦
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_enableCameraAutoFocus(JNIEnv *env, jobject thiz,
                                                                  jboolean var1) {
    return AndroidDeviceManager::Inst().enableCameraAutoFocus(var1); // 转发给设备管理器设置自动对焦开关
}

// 对应 Java: NativeInstance.setCameraCapturerParam(int mode, int width, int height)
// 功能：设置摄像头采集参数（如采集模式、期望宽高），供底层选择合适的摄像头输出规格
JNIEXPORT void JNICALL
Java_io_anyrtc_live_internal_NativeInstance_setCameraCapturerParam(JNIEnv *env, jobject thiz,
                                                                   jint mode, jint width,
                                                                   jint height) {
    AndroidDeviceManager::Inst().setCameraCapturerParam(mode,width,height); // 转发给设备管理器设置采集参数
}
// 对应 Java: NativeInstance.setCameraFocusPosition(float var1, float var2)
// 功能：设置手动对焦坐标位置（var1/var2 通常为归一化的 x/y 坐标）
JNIEXPORT jint JNICALL
Java_io_anyrtc_live_internal_NativeInstance_setCameraFocusPosition(JNIEnv *env, jobject thiz,
                                                                   jfloat var1, jfloat var2) {
    return AndroidDeviceManager::Inst().setCameraFocusPosition(var1,var2); // 转发给设备管理器设置对焦位置
}
}














// 对应 Java: org.webrtc.Camera1Session.recoverCamera()
// 功能：在 Camera1 采集会话异常（如被系统或其他应用抢占）后，尝试恢复/重建摄像头会话
extern "C"
JNIEXPORT void JNICALL
Java_org_webrtc_Camera1Session_recoverCamera(JNIEnv *env, jobject thiz) {
    AndroidDeviceManager::Inst().recoverCamera(); // 转发给设备管理器执行摄像头恢复逻辑
}