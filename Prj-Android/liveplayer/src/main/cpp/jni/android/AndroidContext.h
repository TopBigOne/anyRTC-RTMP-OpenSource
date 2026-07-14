//
// Created by liu on 2021/9/18.
//

// 头文件保护宏：防止本头文件被同一编译单元重复 #include 而导致类型重定义
#ifndef LIVEPLAYER_ANDROIDCONTEXT_H
#define LIVEPLAYER_ANDROIDCONTEXT_H

// PlatformContext 是跨平台抽象基类（声明于 ../PlatformContext.h），
// 不同平台（Android/iOS 等）通过继承它来提供各自的平台上下文实现
#include "PlatformContext.h"
// JNI（Java Native Interface）核心头文件，提供 JNIEnv、jobject、jclass 等类型定义，
// 是 C++ 层与 Java 层互相调用的基础
#include <jni.h>


namespace arlive {
    // AndroidContext：PlatformContext 在 Android 平台上的具体实现。
    // 核心职责是持有并管理与 Java 层 VideoCapturerDevice 相关的两个 JNI 全局引用
    // （jclass 类对象、jobject 实例对象），供其他 native 模块（如 VideoCameraCapturer）
    // 通过 getJavaCapturer()/getJavaCapturerClass() 反射调用 Java 方法。
    // final：禁止再被继承；public 继承 PlatformContext 并实现其纯虚接口
    class AndroidContext final : public PlatformContext {
    public:
        // 构造函数：
        //   env      —— 当前线程绑定的 JNIEnv 指针，用于调用 JNI API
        //   instance —— 调用方传入的 Java 对象引用（当前实现未直接使用，
        //                实际的 Java 端 VideoCapturerDevice 实例是内部通过 NewObject 新建的）
        //   isScreen —— 是否为屏幕采集（true）还是摄像头采集（false），
        //                会透传给 Java 端构造函数用于区分采集类型
        AndroidContext(JNIEnv *env,jobject instance,bool isScreen);
        // 析构函数：override 表明重写基类 PlatformContext 的虚析构函数，
        // 保证通过基类指针 delete 时也能正确释放本类持有的 JNI 全局引用资源
        ~AndroidContext() override;
        // 获取 Java 层 VideoCapturerDevice 实例对象（全局引用），
        // 供其他 native 类调用其上的实例方法（如 init/onStateChanged 等）
        jobject getJavaCapturer();
        // 获取 Java 层 VideoCapturerDevice 类对象（全局引用），
        // 供其他 native 类通过 GetMethodID 反射查找该类的方法 ID
        jclass getJavaCapturerClass();
    private:
        // Java 类 io/anyrtc/live/internal/VideoCapturerDevice 对应的 jclass 全局引用。
        // 使用全局引用（GlobalRef）是因为局部引用（LocalRef）只在当前 JNI 调用栈内有效，
        // 而该 jclass 需要跨多次 JNI 调用、跨线程长期持有，故必须提升为全局引用
        jclass VideoCapturerDeviceClass = nullptr;
        // Java 层 VideoCapturerDevice 实例对象的全局引用，
        // 生命周期与 AndroidContext 对象本身一致，构造时创建，析构时释放
        jobject javaCapturer = nullptr;
    };
}


#endif //LIVEPLAYER_ANDROIDCONTEXT_H
