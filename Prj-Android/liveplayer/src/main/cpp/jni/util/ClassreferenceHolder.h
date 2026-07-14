/*
 *  Copyright 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

// Android's FindClass() is trickier than usual because the app-specific
// ClassLoader is not consulted when there is no app-specific frame on the
// stack.  Consequently, we only look up all classes once in app/webrtc.
// http://developer.android.com/training/articles/perf-jni.html#faq_FindClass
//
// 中文说明：在 Android 上，如果当前 native 线程的调用栈里没有一个来自 App 自身的 Java 帧
// （例如某些回调发生在纯 native 线程、而不是从 Java 方法调用进入 native 的场景），
// 这时调用 JNIEnv::FindClass() 会使用系统/启动 ClassLoader，而不是 App 的 ClassLoader，
// 导致找不到 App 私有的类（例如下面用到的 io/anyrtc/live/util/AsyncHttpURLConnection）。
// 解决办法就是：只在能保证处于“正确”调用栈环境的时机（通常是 JNI_OnLoad，或者由 UI 线程触发的初始化）
// 把所有会用到的类一次性 FindClass 并转换为全局引用缓存起来，之后任何线程都可以直接从缓存里取用，
// 不必再依赖调用栈上下文去做 FindClass。这正是本文件 ClassReferenceHolder 存在的原因。

#ifndef WEBRTC_SDK_ANDROID_SRC_JNI_CLASSREFERENCEHOLDER_H_
#define WEBRTC_SDK_ANDROID_SRC_JNI_CLASSREFERENCEHOLDER_H_

#include <jni.h>
#include <map>
#include <string>

namespace webrtc_loader {

// LoadGlobalClassReferenceHolder must be called in JNI_OnLoad.
// 中文：必须在 JNI_OnLoad（即 so 被加载、JNI 环境初始化完成）时调用一次，
// 用来创建全局的 ClassReferenceHolder 单例并预先加载所有需要用到的 Java 类。
// 在本工程中，实际由 LiveEngine.cpp 的 initWebRTC() 负责调用。
void LoadGlobalClassReferenceHolder();
// FreeGlobalClassReferenceHolder must be called in JNI_UnLoad.
// 中文：必须在 JNI_OnUnload（so 卸载）时调用一次，释放所有缓存的全局引用（jclass），
// 避免造成 JNI 全局引用泄漏（全局引用不会被 GC 自动回收，必须显式 DeleteGlobalRef）。
void FreeGlobalClassReferenceHolder();

// Returns a global reference guaranteed to be valid for the lifetime of the
// process.
// 中文：从预加载好的缓存里按类名取出对应的 jclass 全局引用。
// 因为是全局引用（Global Reference），只要没有被 FreeGlobalClassReferenceHolder 释放，
// 这个 jclass 在整个进程生命周期内、在任意线程上都是有效且可以安全使用的，
// 从而避免了在非主线程/非 App 调用栈环境下调用 FindClass 可能失败的问题。
jclass FindClass(JNIEnv* jni, const char* name);
// Convenience macro defining JNI-accessible methods in the org.webrtc package.
// Eliminates unnecessary boilerplate and line-wraps, reducing visual clutter.
// 中文：一个便捷宏，用来简化在 org.webrtc 包下声明 JNI 可调用方法时的样板代码，
// 展开后会生成形如 extern "C" JNIEXPORT <返回类型> JNICALL Java_org_webrtc_<name> 的函数签名，
// 减少重复书写完整的 JNI 命名规则，降低视觉噪音。
#define JOW(rettype, name) \
  extern "C" JNIEXPORT rettype JNICALL Java_org_webrtc_##name

}  // namespace webrtc_jni
// 注意：上面这行结尾注释写的是 webrtc_jni，但本文件实际打开的命名空间名字是 webrtc_loader（见上方 namespace 声明），
// 二者不一致——这是原始代码里遗留的注释小瑕疵，仅作说明，不影响编译（注释不参与语法检查），本次不做修改。

#endif  // WEBRTC_SDK_ANDROID_SRC_JNI_CLASSREFERENCEHOLDER_H_
