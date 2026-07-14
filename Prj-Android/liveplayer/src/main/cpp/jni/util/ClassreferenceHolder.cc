/*
 *  Copyright 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
// 中文说明（以上版权/许可声明保持原样，不做任何改动）：
// 本文件基本沿用了 Chromium WebRTC 项目自带的 JNI 辅助代码（ClassReferenceHolder），
// 其作用是在 native 层预先缓存一批常用 Java 类的全局引用（jclass），
// 避免后续业务代码在任意线程上下文中反复调用 FindClass 可能失败或产生额外开销的问题
// （原因见 ClassreferenceHolder.h 顶部的说明：Android 上非 App 调用栈环境下 FindClass 可能找不到 App 私有类）。
#include "ClassreferenceHolder.h"

#include "jni_helpers.h"

namespace webrtc_loader {

// ClassReferenceHolder holds global reference to Java classes in app/webrtc.
// 中文：ClassReferenceHolder 是一个“类引用缓存池”，
// 内部用一个 std::map<类名, jclass 全局引用> 保存所有预加载的 Java 类。
// 之所以要缓存“全局引用”而不是每次现查现用的局部引用（Local Reference），有两个原因：
//   1) 局部引用只在当前 JNI 调用/当前 native 方法的生命周期内有效，函数返回后会被自动释放，
//      不能跨越多次 JNI 调用长期持有；而全局引用（Global Reference）由开发者手动管理生命周期，
//      在调用 DeleteGlobalRef 之前会一直有效，适合“启动时加载一次、进程运行期间随处使用”的场景。
//   2) 用 std::map 做名字到 jclass 的映射，可以在任意线程、任意时刻通过类名快速查到已经缓存好的类，
//      而不需要重新执行开销较大且在某些线程上可能失败的 FindClass 调用。
class ClassReferenceHolder {
 public:
  explicit ClassReferenceHolder(JNIEnv* jni);
  ~ClassReferenceHolder();

  void FreeReferences(JNIEnv* jni);
  jclass GetClass(const std::string& name);

 private:
  void LoadClass(JNIEnv* jni, const std::string& name);

  // classes_：类名 -> 全局引用(jclass) 的缓存表。
  // 之所以选用 std::map<std::string, jclass>，是因为访问模式是“按类名查找”，
  // 用关联容器可以清晰地表达“类名 -> jclass”的语义，同时避免线性查找。
  std::map<std::string, jclass> classes_;
};

// Allocated in LoadGlobalClassReferenceHolder(),
// freed in FreeGlobalClassReferenceHolder().
// 中文：全局单例指针。整个进程只应该存在一份 ClassReferenceHolder 实例，
// 在 LoadGlobalClassReferenceHolder() 中 new 出来，在 FreeGlobalClassReferenceHolder() 中 delete 掉。
static ClassReferenceHolder* g_class_reference_holder = nullptr;

// LoadGlobalClassReferenceHolder：对外暴露的初始化入口。
// 中文：
//   1) RTC_CHECK 断言此前尚未初始化过（防止重复调用导致内存泄漏/覆盖旧指针）；
//   2) 通过 webrtc::jni::GetEnv() 拿到当前线程的 JNIEnv，构造 ClassReferenceHolder，
//      构造函数内部会依次 FindClass + NewGlobalRef 加载所有预定义的类（见下方构造函数实现）。
// 该函数需要在“能保证 FindClass 正常工作”的时机被调用（通常是 JNI_OnLoad，或者本工程里的 initWebRTC 阶段的 UI/主线程环境）。
void LoadGlobalClassReferenceHolder() {
  RTC_CHECK(g_class_reference_holder == nullptr);
  g_class_reference_holder = new ClassReferenceHolder(webrtc::jni::GetEnv());
}

// FreeGlobalClassReferenceHolder：对外暴露的反初始化入口，与 LoadGlobalClassReferenceHolder 成对使用。
// 中文：
//   1) 先调用 FreeReferences 释放所有缓存的全局引用（DeleteGlobalRef），避免 JNI 全局引用泄漏；
//   2) 再 delete 掉 ClassReferenceHolder 实例本身（其析构函数里会断言 classes_ 必须已经清空）；
//   3) 最后把全局指针置空，允许后续再次 Load（例如 so 被重新加载的场景）。
//   这里用 webrtc::AttachCurrentThreadIfNeeded() 而不是 GetEnv()，是因为反初始化可能发生在
//   一个尚未 Attach 到 JVM 的 native 线程上，需要先确保拿到一个可用的 JNIEnv。
void FreeGlobalClassReferenceHolder() {
  g_class_reference_holder->FreeReferences(webrtc::AttachCurrentThreadIfNeeded());
  delete g_class_reference_holder;
  g_class_reference_holder = nullptr;
}

// ClassReferenceHolder 构造函数：在这里集中列出所有需要预加载的 Java 类。
// 中文：每一行 LoadClass 调用都会执行一次“FindClass + NewGlobalRef + 存入 map”的完整流程（见 LoadClass 实现），
// 如果其中任何一步失败，都会通过 RTC_CHECK/CHECK_EXCEPTION 直接让进程崩溃退出（属于“快速失败”的防御性编程风格，
// 因为这些类是基础类型，缺失了后续代码根本无法正常工作）。
// 加载的类可以分为两类：
//   - JDK 基础类型/容器类：Boolean、Double、Integer、Long、String、BigInteger、ByteBuffer、ArrayList、LinkedHashMap，
//     这些类在 JNI 层做 Java<->Native 数据类型转换时会被频繁用到（比如把 native 的数值包装成 Java 的装箱类型）。
//   - Android 系统类：SurfaceTexture（视频渲染用到的纹理对象）、ActivityThread（用于在 native 层拿到当前应用的 Context/ClassLoader 等）。
//   - App 自身的类：io/anyrtc/live/util/AsyncHttpURLConnection，是本 SDK 自己定义的一个 Java 类
//     （用于 native 层发起异步 HTTP 请求时反向调用 Java 层实现），因为它不属于系统/JDK 类，
//     更容易受到“非 App 调用栈下 FindClass 找不到”这个问题的影响，所以同样需要在此预加载缓存。
ClassReferenceHolder::ClassReferenceHolder(JNIEnv* jni) {
  LoadClass(jni, "android/graphics/SurfaceTexture");
  LoadClass(jni, "java/lang/Boolean");
  LoadClass(jni, "java/lang/Double");
  LoadClass(jni, "java/lang/Integer");
  LoadClass(jni, "java/lang/Long");
  LoadClass(jni, "java/lang/String");
  LoadClass(jni, "java/math/BigInteger");
  LoadClass(jni, "java/nio/ByteBuffer");
  LoadClass(jni, "java/util/ArrayList");
  LoadClass(jni, "java/util/LinkedHashMap");
  LoadClass(jni, "android/app/ActivityThread");
  LoadClass(jni, "io/anyrtc/live/util/AsyncHttpURLConnection");
}

// 析构函数：这里刻意不在析构函数里做释放全局引用的工作，而是要求调用方必须先显式调用 FreeReferences，
// 再触发析构（本类的实例是通过 delete g_class_reference_holder 手动销毁的）。
// RTC_CHECK(classes_.empty()) 起到“契约检查”的作用：如果开发者忘记调用 FreeReferences 就直接销毁对象，
// 这里会立刻断言失败并崩溃，而不是悄悄地泄漏掉一堆 JNI 全局引用——用可预测的崩溃代替难以排查的资源泄漏。
ClassReferenceHolder::~ClassReferenceHolder() {
  RTC_CHECK(classes_.empty()) << "Must call FreeReferences() before dtor!";
}

// FreeReferences：遍历 classes_ 这个缓存表，对每一个缓存的 jclass 全局引用调用 DeleteGlobalRef，
// 中文：JNI 全局引用不受 GC 管理，必须手动 DeleteGlobalRef 才能被回收，否则会造成引用计数/内存泄漏。
// 遍历结束后调用 classes_.clear() 清空整个 map，这样析构函数里的 RTC_CHECK(classes_.empty()) 才能通过。
void ClassReferenceHolder::FreeReferences(JNIEnv* jni) {
  for (std::map<std::string, jclass>::const_iterator it = classes_.begin();
      it != classes_.end(); ++it) {
    jni->DeleteGlobalRef(it->second);
  }
  classes_.clear();
}

// GetClass：按类名从缓存表里查找已经加载好的全局引用 jclass。
// 中文：如果传入的类名此前没有通过 LoadClass 预加载过，RTC_CHECK 会直接触发断言崩溃，
// 这是有意为之的设计——GetClass 只应该用来获取“已知会被用到、已经在构造函数里注册过”的类，
// 如果出现未预注册的类名，说明代码逻辑有误（漏加了 LoadClass），应尽早暴露问题而不是返回空指针让调用方在别处出错。
jclass ClassReferenceHolder::GetClass(const std::string& name) {
  std::map<std::string, jclass>::iterator it = classes_.find(name);
  RTC_CHECK(it != classes_.end()) << "Unexpected GetClass() call for: " << name;
  return it->second;
}

// LoadClass：加载单个 Java 类并转换为全局引用、存入缓存表，这是整个类加载机制的核心步骤：
//   1) jni->FindClass(name)：按完全限定名（如 "java/lang/String"）查找类，得到一个局部引用(local ref)。
//      局部引用只在当前 native 方法调用期间有效，函数返回后会被自动回收，不能长期持有。
//   2) CHECK_EXCEPTION(jni)：检查上一步 FindClass 是否抛出了 Java 异常（比如 ClassNotFoundException），
//      如果有异常，这里会记录日志并触发相应处理（具体行为由 jni_helpers.h 中的宏定义决定）。
//   3) RTC_CHECK(localRef)：进一步断言 FindClass 确实返回了非空的局部引用。
//   4) jni->NewGlobalRef(localRef)：把局部引用“升级”为全局引用——这一步是关键，
//      只有全局引用才能跨越多次 JNI 调用、跨线程长期有效地持有，
//      从而实现“只 FindClass 一次，之后随时可用”的目标。
//   5) 再次 CHECK_EXCEPTION + RTC_CHECK，确保 NewGlobalRef 也成功。
//   6) 把 <name, globalRef> 存入 classes_ 这个 map；RTC_CHECK(inserted) 确保不会出现同名类被重复加载
//      （std::map::insert 在 key 已存在时不会覆盖，返回值的 second 为 false，代表插入失败/重复）。
void ClassReferenceHolder::LoadClass(JNIEnv* jni, const std::string& name) {
  jclass localRef = jni->FindClass(name.c_str());
  CHECK_EXCEPTION(jni) << "error during FindClass: " << name;
  RTC_CHECK(localRef) << name;
  jclass globalRef = reinterpret_cast<jclass>(jni->NewGlobalRef(localRef));
  CHECK_EXCEPTION(jni) << "error during NewGlobalRef: " << name;
  RTC_CHECK(globalRef) << name;
  bool inserted = classes_.insert(std::make_pair(name, globalRef)).second;
  RTC_CHECK(inserted) << "Duplicate class name: " << name;
}

// Returns a global reference guaranteed to be valid for the lifetime of the
// process.
// 中文：这是暴露给本文件之外（其它 .cc/.cpp 文件）使用的自由函数版本的 FindClass 便捷封装，
// 与 JNIEnv::FindClass() 同名但语义不同——它并不会真的去做类查找，而是直接委托给全局单例
// g_class_reference_holder->GetClass(name) 从缓存里取出之前已经预加载好的全局引用。
// 这样，其余业务代码只需要 #include "ClassreferenceHolder.h" 并调用这个 FindClass，
// 就能安全地在任意线程拿到目标类的 jclass，而不必关心 FindClass 在当前调用栈环境下是否可靠。
jclass FindClass(JNIEnv* jni, const char* name) {
  return g_class_reference_holder->GetClass(name);
}

}  // namespace webrtc_jni
// 同 ClassreferenceHolder.h 中的说明：此处结尾注释写的命名空间名 webrtc_jni 与实际的 namespace webrtc_loader 不一致，
// 属于原始代码遗留的注释小瑕疵，不影响编译，本次仅作说明、不做修改。
