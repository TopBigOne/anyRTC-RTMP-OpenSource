#include "VideoCameraCapturer.h"

#include <cstdint>
#include <memory>
#include <algorithm>

#include "AndroidContext.h"
#include "sdk/android/native_api/jni/jvm.h"

namespace arlive {

// 构造函数：创建一路采集会话，并把自身注册给 Java 层，以便 Java 端后续能够
// 通过持有的 native 指针回调到本 C++ 对象
VideoCameraCapturer::VideoCameraCapturer(rtc::scoped_refptr<webrtc::JavaVideoTrackSourceInterface> source, std::string deviceId,int width,int height,int fps, std::shared_ptr<PlatformContext> platformContext) : _platformContext(platformContext) {
    // platformContext 在 Android 平台上实际运行时类型就是 AndroidContext，
    // 这里做向下类型转换（static-style cast），以便调用 AndroidContext 特有的
    // getJavaCapturerClass()/getJavaCapturer() 接口
    AndroidContext *context = (AndroidContext *) platformContext.get();
    // 获取当前线程可用的 JNIEnv*；由于采集器可能在非 Java 主线程（如内部创建的采集线程）
    // 中被构造，AttachCurrentThreadIfNeeded 会在必要时把当前线程 attach 到 JVM
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    // 反射查找 Java 层 VideoCapturerDevice.init 方法：
    //   方法签名 "(JLjava/lang/String;III)V" 拆解为：
    //     J                    —— 第1个参数 long（对应 native 侧的 jlong，用来传递 this 指针）
    //     Ljava/lang/String;   —— 第2个参数 java.lang.String（设备 id，如 "front"/"back"/"screen"）
    //     I I I                —— 第3、4、5个参数均为 int（width、height、fps）
    //     V                    —— 返回值 void
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "init", "(JLjava/lang/String;III)V");
    // 调用 Java 对象的 init 方法，实参依次为：
    //   (jlong)(intptr_t)this —— 关键的"指针回传"手法：把当前 C++ 对象的 this 指针
    //                            强转为 intptr_t（保证不同位宽平台下指针能整数表示），
    //                            再转为 jlong 传给 Java 层保存起来；
    //                            后续 Java 层在摄像头采集帧到达、状态变化等场景下，
    //                            会把这个 long 值原样传回 native 层的 JNI 回调函数，
    //                            native 回调再将其还原为 VideoCameraCapturer* 指针，
    //                            从而找到发起调用的具体 C++ 对象实例——
    //                            这是 JNI 中"native 对象句柄"的经典往返（round-trip）模式
    //   env->NewStringUTF(...) —— 把 std::string 转换为 Java 层的 jstring 对象（局部引用）
    //   width,height,fps        —— 直接传递的 int 型采集参数
    env->CallVoidMethod(context->getJavaCapturer(), methodId, (jlong) (intptr_t) this, env->NewStringUTF(deviceId.c_str()),width,height,fps);
}


// 通知 Java 层采集状态变化（VideoState 的 Inactive/Paused/Active）
void VideoCameraCapturer::setState(VideoState state) {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 反射查找 onStateChanged 方法：
    //   签名 "(JI)V" —— 第1个参数 long（native 指针，同样是往返模式，供 Java 侧回调时识别对象），
    //                   第2个参数 int（VideoState 枚举值），返回值 void
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "onStateChanged", "(JI)V");
    // 调用时同样传入 (jlong)(intptr_t)this 作为 native 句柄，以及 (jint)state 表示新状态
    env->CallVoidMethod(context->getJavaCapturer(), methodId, (jlong) (intptr_t) this, (jint) state);
}

// 查询自动对焦是否已开启
bool VideoCameraCapturer::isAutoFocusEnabled() {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "()Z" —— 无参数，返回值 boolean（Z）
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "isAutoFocusEnabled", "()Z");
    // CallBooleanMethod 对应返回值为 boolean 的 Java 方法调用，得到 jboolean（JNI_TRUE/JNI_FALSE）
    jboolean result = env->CallBooleanMethod(context->getJavaCapturer(), methodId);
    return result;
}

// [ArDeviceManager 接口重写] 是否为前置摄像头。
// 注意：此处函数体为空，且没有 return 语句，而函数声明返回类型是 bool ——
// 这是未定义行为（UB）：调用者读取返回值将得到不确定的内容。
// 经排查确认，当前代码库中没有任何地方会调用 VideoCameraCapturer 这个具体类型上的
// isFrontCamera()；实际的"是否前置摄像头"查询是由 AndroidDeviceManager 自己的
// isFrontCamera() 方法承担（它内部会重新创建采集器而不是委托给这里），
// 所以这个空实现属于死代码/不可达路径，只是因为继承自 ArDeviceManager 而必须提供
// 该虚函数的实现以满足编译，实际从未被执行到。保留现状，仅作提示，不做修复。
bool VideoCameraCapturer::isFrontCamera() {

}

// [ArDeviceManager 接口重写] 切换前后摄像头。
// 注意：同上，函数体为空且无 return 语句，返回类型 int，属于未定义行为（UB）。
// 该方法在 VideoCameraCapturer 上从未被实际调用——JNI 层 switchCamera 的入口
// 实际是走 AndroidDeviceManager 的同名方法（它会重建采集器实例），
// 而不是委托到这里，因此这是死代码/不可达路径。保留现状，仅作提示，不做修复。
int VideoCameraCapturer::switchCamera(bool frontCamera) {

}

// 变更采集参数（分辨率、帧率）
void VideoCameraCapturer::changeCaptureFormat(int width, int height, int fps) {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "(III)V" —— 三个 int 参数（width,height,fps），返回值 void
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "setCameraCapturerParam", "(III)V");
    env->CallVoidMethod(context->getJavaCapturer(), methodId,(jint)width,(jint)height,(jint)fps);
}

// 获取摄像头支持的最大变焦倍数
float VideoCameraCapturer::getCameraZoomMaxRatio() {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "()F" —— 无参数，返回值 float（F）
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "getCameraZoomMaxRatio", "()F");
    // CallFloatMethod 对应返回值为 float 的 Java 方法调用
    jfloat result = env->CallFloatMethod(context->getJavaCapturer(), methodId);
    return result;
}

// 设置摄像头当前变焦倍数
int VideoCameraCapturer::setCameraZoomRatio(float zoomRatio) {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "(F)I" —— 1 个 float 参数（zoomRatio），返回值 int（一般作为结果码）
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "setCameraZoomRatio", "(F)I");
    // CallIntMethod 对应返回值为 int 的 Java 方法调用
    jint result = env->CallIntMethod(context->getJavaCapturer(), methodId,(jfloat)zoomRatio);
    return result;
}

// 开关摄像头自动对焦
int VideoCameraCapturer::enableCameraAutoFocus(bool enabled) {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "(Z)I" —— 1 个 boolean 参数（enabled），返回值 int（结果码）
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "enableCameraAutoFocus", "(Z)I");
    // 直接以 return 语句返回 CallIntMethod 的结果，省去中间局部变量
    return env->CallIntMethod(context->getJavaCapturer(), methodId,(jboolean)enabled);

}

// 设置对焦点位置（x,y 一般为归一化坐标，取值范围由 Java 层约定）
int VideoCameraCapturer::setCameraFocusPosition(float x, float y) {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "(FF)I" —— 2 个 float 参数（x,y），返回值 int（结果码）
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "setCameraFocusPosition", "(FF)I");
    return env->CallIntMethod(context->getJavaCapturer(), methodId,(jfloat)x,(jfloat)y);
}
// 开关手电筒/闪光灯
int VideoCameraCapturer::enableCameraTorch(bool enabled) {
    JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
    auto context = (AndroidContext *) _platformContext.get();
    // 签名 "(Z)Z" —— 1 个 boolean 参数（enabled），返回值 boolean（是否设置成功）
    jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "enableCameraTorch", "(Z)Z");
    // CallBooleanMethod 对应返回值为 boolean 的 Java 方法调用，
    // 这里额外传入 (jboolean)enabled 作为实参
    jboolean result = env->CallBooleanMethod(context->getJavaCapturer(), methodId, (jboolean) enabled);
    // 将 Java 层返回的 boolean 结果转换为本方法约定的 int 结果码：成功为 0，失败为 -1
    return result?0:-1;
}

// [ArDeviceManager 接口重写] 设置音频路由（如听筒/扬声器/蓝牙耳机等）。
// 注意：函数体为空且无 return 语句，返回类型 int，属于未定义行为（UB）。
// 经排查，当前没有任何调用点会在 VideoCameraCapturer 这个具体类型上调用
// setAudioRoute（音频路由属于音频设备管理范畴，与本类负责的视频采集职责不符），
// 因此这是因继承 ArDeviceManager 而被迫提供、实际从未被执行到的死代码/不可达路径。
// 保留现状，仅作提示，不做修复。
int VideoCameraCapturer::setAudioRoute(ArAudioRoute route) {

}

// [ArDeviceManager 接口重写] 设置系统音量类型（如媒体音量/通话音量）。
// 注意：函数体为空且无 return 语句，返回类型 int，属于未定义行为（UB）。
// 与 setAudioRoute 情况相同：没有调用点会在 VideoCameraCapturer 上调用该方法，
// 属于因继承关系被迫实现、实际不可达的死代码。保留现状，仅作提示，不做修复。
int VideoCameraCapturer::setSystemVolumeType(ArSystemVolumeType type) {

}







}  // namespace ARLIVE
