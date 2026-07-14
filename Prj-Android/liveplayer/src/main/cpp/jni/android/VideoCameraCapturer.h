// 头文件保护宏，防止重复包含
#ifndef ARLIVE_VIDEO_CAMERA_CAPTURER_H
#define ARLIVE_VIDEO_CAMERA_CAPTURER_H

// WebRTC 引用计数智能指针模板 rtc::scoped_refptr
#include "api/scoped_refptr.h"
// WebRTC 媒体流相关接口定义
#include "api/media_stream_interface.h"
// WebRTC 视频采集模块基础定义
#include "modules/video_capture/video_capture.h"
// JNI 局部引用的 RAII 包装类（ScopedJavaLocalRef 等），用于自动管理局部引用生命周期
#include "sdk/android/native_api/jni/scoped_java_ref.h"
// JavaVideoTrackSourceInterface 等视频源相关类型的声明
#include "sdk/android/native_api/video/video_source.h"

#include <memory>
#include <vector>
#include <stddef.h>
// JNI 核心头文件：JNIEnv、jobject、jclass、jmethodID 等类型
#include <jni.h>
// 跨平台上下文抽象基类，Android 端由 AndroidContext 实现
#include <PlatformContext.h>
// ArDeviceManager 抽象基类的声明（音视频设备管理接口，如摄像头切换、音频路由等）
#include "IArDeviceManager.h"
namespace arlive {

// 采集状态枚举：未激活 / 暂停 / 采集中，
// 会通过 setState() 同步给 Java 层（对应 onStateChanged 的 int 参数）
enum class VideoState {
		Inactive,
		Paused,
		Active,
};
// 引入 anyrtc 命名空间，方便直接使用 ArAudioRoute、ArSystemVolumeType 等类型
using namespace anyrtc;
// 前向声明（其下方紧接着就是完整定义，这里是为了兼容某些编译单元的前置引用需要）
class VideoCameraCapturer;

// VideoCameraCapturer：单路摄像头/屏幕采集会话的 native 封装。
// 私有继承自 ArDeviceManager（设备管理抽象基类），
// 重写（override，虽然此处没写 override 关键字，但通过同名同参 virtual 函数实现覆盖）
// 其中 isFrontCamera/switchCamera/setAudioRoute/setSystemVolumeType 四个接口，
// 但注意：这四个方法在本类中的实现是"空实现"（见 .cpp），实际调用路径并未经过这里，
// 详见 .cpp 中对应方法处的说明
class VideoCameraCapturer :ArDeviceManager{

public:
	// 构造函数：创建一路摄像头/屏幕采集器
	//   source         —— WebRTC 视频轨道源（Java 层 JavaVideoTrackSourceInterface 的 native 封装），
	//                      采集到的视频帧最终会灌入这个 source
	//   deviceId       —— 设备标识字符串，如 "front"（前置摄像头）/"back"（后置摄像头）/"screen"（屏幕采集）
	//   width/height   —— 期望的采集分辨率
	//   fps            —— 期望的采集帧率
	//   platformContext—— 平台上下文（Android 上实际是 AndroidContext），
	//                      用于反射调用 Java 层 VideoCapturerDevice 的方法
	VideoCameraCapturer(rtc::scoped_refptr<webrtc::JavaVideoTrackSourceInterface> source, std::string deviceId, int width,int height,int fps, std::shared_ptr<PlatformContext> platformContext);
	// 通知 Java 层采集状态发生变化（Inactive/Paused/Active）
	void setState(arlive::VideoState state);
	// [ArDeviceManager 接口重写] 是否为前置摄像头。
	// 注意：本类中该方法为空实现（见 .cpp），是死代码/不可达路径，详见 .cpp 中说明
	virtual bool isFrontCamera();
	// [ArDeviceManager 接口重写] 切换前后摄像头。
	// 注意：本类中该方法为空实现（见 .cpp），是死代码/不可达路径，详见 .cpp 中说明
	virtual int switchCamera(bool frontCamera);
	// 变更采集参数（分辨率/帧率），通过 JNI 反射调用 Java 层 setCameraCapturerParam
	virtual void changeCaptureFormat(int width,int height,int fps);
	// 获取摄像头最大变焦倍数，通过 JNI 反射调用 Java 层 getCameraZoomMaxRatio
	virtual float getCameraZoomMaxRatio();
	// 设置摄像头变焦倍数，通过 JNI 反射调用 Java 层 setCameraZoomRatio
	virtual int setCameraZoomRatio(float zoomRatio);
	// 查询自动对焦是否开启，通过 JNI 反射调用 Java 层 isAutoFocusEnabled
	virtual bool isAutoFocusEnabled();
	// 开关自动对焦，通过 JNI 反射调用 Java 层 enableCameraAutoFocus
	virtual int enableCameraAutoFocus(bool enabled);
	// 设置对焦点坐标（一般为归一化坐标 x,y），通过 JNI 反射调用 Java 层 setCameraFocusPosition
	virtual int setCameraFocusPosition(float x, float y);
	// 开关手电筒/闪光灯，通过 JNI 反射调用 Java 层 enableCameraTorch
	virtual int enableCameraTorch(bool enabled);
	// [ArDeviceManager 接口重写] 设置音频路由（听筒/扬声器/耳机等）。
	// 注意：本类中该方法为空实现（见 .cpp），是死代码/不可达路径，详见 .cpp 中说明
	virtual int setAudioRoute(ArAudioRoute route);
	// [ArDeviceManager 接口重写] 设置系统音量类型（媒体音量/通话音量等）。
	// 注意：本类中该方法为空实现（见 .cpp），是死代码/不可达路径，详见 .cpp 中说明
	virtual int setSystemVolumeType(ArSystemVolumeType type);

private:
	// 持有平台上下文的共享指针（Android 上实际指向 AndroidContext 实例），
	// 用于在各方法中反射调用 Java 层 VideoCapturerDevice 对象的方法
	std::shared_ptr<arlive::PlatformContext> _platformContext;
};

}  // namespace ARLIVE

#endif
