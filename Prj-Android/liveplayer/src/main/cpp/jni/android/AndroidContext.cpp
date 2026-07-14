//
// Created by liu on 2021/9/18.
//

#include "AndroidContext.h"
// WebRTC 提供的 JNI 辅助头文件，声明了 AttachCurrentThreadIfNeeded 等工具函数
#include "sdk/android/native_api/jni/jvm.h"
#include <modules/utility/include/jvm_android.h>
#include "webrtc/sdk/android/src/jni/jvm.h"
namespace arlive{

    // 构造函数：负责创建/绑定 Java 层 VideoCapturerDevice 的类引用与实例引用
    AndroidContext::AndroidContext(JNIEnv *env, jobject instance,bool isScreen) {
        // 1. env->FindClass(...) 按类的全限定名（用 '/' 分隔包名）在当前线程的类加载器中查找
        //    io/anyrtc/live/internal/VideoCapturerDevice 类，返回一个局部引用（LocalRef）；
        // 2. env->NewGlobalRef(...) 把这个局部引用提升为全局引用（GlobalRef），
        //    使其不会随当前 JNI 调用帧结束而被 GC 回收，可以跨函数、跨线程长期持有；
        // 3. 强制转换为 jclass 并保存到成员变量 VideoCapturerDeviceClass 中
        VideoCapturerDeviceClass = (jclass) env->NewGlobalRef(env->FindClass("io/anyrtc/live/internal/VideoCapturerDevice"));
        // 通过 GetMethodID 反射查找 VideoCapturerDevice 的构造函数：
        //   方法名固定为 "<init>"（JNI 中构造函数的特殊方法名）
        //   签名 "(Z)V" 表示：参数为 1 个 boolean（Z），返回值为 void（V）
        //   对应 Java 端形如 VideoCapturerDevice(boolean isScreen) 的构造函数
        jmethodID initMethodId = env->GetMethodID(VideoCapturerDeviceClass, "<init>", "(Z)V");
        // env->NewObject(...) 调用上面找到的构造函数，用 isScreen 作为实参创建一个新的 Java 对象
        // （isScreen 用于区分是屏幕采集还是摄像头采集）；
        // NewObject 返回的同样是局部引用，再次用 NewGlobalRef 提升为全局引用，
        // 保证 javaCapturer 在 AndroidContext 生命周期内始终有效
        javaCapturer = env->NewGlobalRef(env->NewObject(VideoCapturerDeviceClass, initMethodId,isScreen));
    }



    // 析构函数：在 native 对象销毁前，先通知 Java 端做资源清理（onDestroy），
    // 然后释放本类持有的两个 JNI 全局引用，避免内存/引用泄漏
    AndroidContext::~AndroidContext() {
            // AttachCurrentThreadIfNeeded()：确保当前线程已附加到 JVM 并获取可用的 JNIEnv*。
            // 因为析构函数可能在非 Java 创建的 native 线程（如采集/编码线程）中被调用，
            // 该线程未必已 attach 到 JVM，所以需要先 attach 才能安全调用 JNI 方法
            JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
            // 反射查找 VideoCapturerDevice.onDestroy() 方法，
            // 签名 "()V" 表示无参数、无返回值（void）
            jmethodID onDestroyMethodId = env->GetMethodID(VideoCapturerDeviceClass, "onDestroy", "()V");
            // 调用 Java 对象 javaCapturer 的 onDestroy() 实例方法，
            // 让 Java 层在真正释放 native 全局引用之前完成自身资源释放（如释放摄像头、Surface 等）
            env->CallVoidMethod(javaCapturer, onDestroyMethodId);
            // 释放 javaCapturer 全局引用，允许该 Java 对象被 GC 回收
            env->DeleteGlobalRef(javaCapturer);
            // 置空，防止悬挂指针被误用
            javaCapturer = nullptr;
            // 释放 VideoCapturerDeviceClass 全局引用，解除对该 Java 类对象的强引用
            env->DeleteGlobalRef(VideoCapturerDeviceClass);
    }


    // 返回 Java 层 VideoCapturerDevice 实例对象（全局引用），
    // 供其他 native 模块（如 VideoCameraCapturer）拿到后直接作为 CallXxxMethod 的 this 对象
    jobject AndroidContext::getJavaCapturer() {
        return javaCapturer;
    }

    // 返回 Java 层 VideoCapturerDevice 的 jclass 全局引用，
    // 供其他 native 模块通过 GetMethodID 反射查找该类上的方法
    jclass AndroidContext::getJavaCapturerClass() {
        return VideoCapturerDeviceClass;
    }


}
