# ArLiveLite 用到的 C++/JNI 技术点

> 按技术类别组织，聚焦"这段代码在示范什么可复用的技巧"，而非功能本身。功能清单见 [ArLiveLite具体功能.md](./ArLiveLite具体功能.md)。所有代码块均为源码原文摘录（未改写），仅在超长处用 `...` 省略无关部分。

## 1. JNI 互操作技术

- **句柄化对象传递（handle pattern）**：`LiveEngine.cpp:48-83` 把 `shared_ptr<PlatformContext>` + `IArLive2Engine*` 打包进 `InstanceHolder`，`new` 出来后 `reinterpret_cast` 成 `jlong` 存进 Java 对象字段，后续每次调用逆向转换取回：

  ```cpp
  // InstanceHolder：native 侧保存的“实例句柄”结构体，Java 层通过 jlong（nativePtr 字段）持有其地址
  struct InstanceHolder {
      std::shared_ptr<PlatformContext> _platformContext;
      IArLive2Engine *arLiveEngine;
  };

  // getInstanceHolder：将 nativePtr（jlong）还原为 InstanceHolder* 指针
  InstanceHolder *getInstanceHolder(JNIEnv *env, jobject obj) {
      return reinterpret_cast<InstanceHolder *>(getInstanceHolderId(env, obj));
  }

  JNIEXPORT jlong JNICALL
  Java_io_anyrtc_live_internal_NativeInstance_makeNativeInstance(JNIEnv *env, jobject obj, jobject instance) {
      initWebRTC(env);
      auto *holder = new InstanceHolder();                    // 生命周期由 Java 侧手动管理，无自动析构
      holder->arLiveEngine = V2_CALL::createArLive2Engine();
      std::shared_ptr<PlatformContext> platformContext;
      platformContext = std::make_shared<AndroidContext>(env,obj, false);
      holder->_platformContext = platformContext;
      AndroidDeviceManager::Inst().setPlatformContext(platformContext);
      holder->arLiveEngine->initialize(NULL);
      return reinterpret_cast<jlong>(holder);                 // 按位转换为 jlong 返回给 Java 作为句柄
  }
  ```
  `IArLivePusher*`/`IArLivePlayer*` 也是同样的裸指针往返，没有引用计数或所有权校验——理解"Java 长期持有一个 native 句柄"这个最基础的 JNI 互操作模式的最直接例子。

- **反方向的 this 指针往返**：`VideoCameraCapturer` 构造函数（`VideoCameraCapturer.cpp:14-40`）把 `(jlong)(intptr_t)this` 传给 Java 保存，用于"Java 主动回调 native"的场景：

  ```cpp
  VideoCameraCapturer::VideoCameraCapturer(rtc::scoped_refptr<webrtc::JavaVideoTrackSourceInterface> source,
                                            std::string deviceId,int width,int height,int fps,
                                            std::shared_ptr<PlatformContext> platformContext) : _platformContext(platformContext) {
      AndroidContext *context = (AndroidContext *) platformContext.get();
      JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
      jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "init", "(JLjava/lang/String;III)V");
      // (jlong)(intptr_t)this —— 把当前 C++ 对象的 this 指针传给 Java 层保存，
      // 后续 Java 层在摄像头采集帧到达、状态变化等场景下会把这个 long 值原样传回 native 回调函数，
      // native 回调再将其还原为 VideoCameraCapturer* 指针
      env->CallVoidMethod(context->getJavaCapturer(), methodId, (jlong) (intptr_t) this,
                          env->NewStringUTF(deviceId.c_str()),width,height,fps);
  }
  ```
  与上面 `InstanceHolder` 的方向正好相反（Java 持有 native 句柄 vs native 把自己的地址托管给 Java），两者对照读能完整理解 JNI 双向句柄传递。

- **全局引用的完整生命周期配对**：`ArLivePushEvent`/`ArLivePlayEvent` 构造时 `NewGlobalRef` 提升 Java observer 为全局引用，析构时对称 `DeleteGlobalRef`（`ArLivePushEvent.cpp:13-46`）：

  ```cpp
  LivePushEvent::LivePushEvent(jobject event):
          m_jJavaObj(NULL),
          m_jClass(NULL) {
      if (event){
          JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
          // 局部引用只在当前 native 方法调用期间有效，LivePushEvent 的生命周期会跨越整个推流会话，
          // 因此必须用 NewGlobalRef 创建一个不会被自动回收、需手动释放的全局引用
          m_jJavaObj = jni->NewGlobalRef(event);
          m_jClass = static_cast<jclass>(jni->NewGlobalRef(
                  jni->GetObjectClass(m_jJavaObj)));
      }
  }

  LivePushEvent::~LivePushEvent() {
      if (m_jJavaObj){
          JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
          jni->DeleteGlobalRef(m_jClass);
          jni->DeleteGlobalRef(m_jJavaObj);
          m_jClass = NULL;
          m_jJavaObj = NULL;
      }
  }
  ```
  `AndroidContext` 析构时**先**调用 Java `onDestroy()` 让 Java 侧完成清理，**再** `DeleteGlobalRef`（`AndroidContext.cpp:36-53`）：

  ```cpp
  AndroidContext::~AndroidContext() {
          JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
          jmethodID onDestroyMethodId = env->GetMethodID(VideoCapturerDeviceClass, "onDestroy", "()V");
          // 让 Java 层在真正释放 native 全局引用之前完成自身资源释放（如释放摄像头、Surface 等）
          env->CallVoidMethod(javaCapturer, onDestroyMethodId);
          env->DeleteGlobalRef(javaCapturer);
          javaCapturer = nullptr;
          env->DeleteGlobalRef(VideoCapturerDeviceClass);
  }
  ```
  顺序本身就是教学点：反过来做（先 `DeleteGlobalRef` 再让 Java 调用 `onDestroy()`）会导致 Java 侧访问到已经失效的全局引用。

- **`AttachCurrentThreadIfNeeded` 贯穿所有 native→Java 回调**：因为编码/网络/解码线程不是 JVM 创建的线程，回调 Java 前必须先 attach 才能拿到合法 `JNIEnv*`。上面 `LivePushEvent` 的构造/析构/`onError` 每一处都以 `JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();` 开头就是范例——同一个模式在 `ArLivePlayEvent.cpp`、`AndroidContext.cpp`、`VideoCameraCapturer.cpp` 里反复出现，是整个 JNI 层最高频的一行代码。

- **`atomic<jmethodID>` 方法 ID 缓存 vs 每次 `GetMethodID`**：自动生成的 `NativePushObserver_JNI.h` 用 `static std::atomic<jmethodID>` 做懒加载缓存：

  ```cpp
  static std::atomic<jmethodID> g_io_anyrtc_live_internal_NativePushObserver_onError(nullptr);

  static void Java_NativePushObserver_onError(JNIEnv* env, const base::android::JavaRef<jobject>& obj,
      JniIntWrapper code,
      const base::android::JavaRef<jstring>& msg,
      const base::android::JavaRef<jstring>& extraInfo) {
    jclass clazz = io_anyrtc_live_internal_NativePushObserver_clazz(env);
    CHECK_CLAZZ(env, obj.obj(), io_anyrtc_live_internal_NativePushObserver_clazz(env));

    jni_generator::JniJavaCallContextChecked call_context;
    // 若缓存 g_..._onError 为空才真正 GetMethodID，查到后写入缓存；否则直接复用
    call_context.Init<base::android::MethodID::TYPE_INSTANCE>(
            env, clazz, "onError", "(ILjava/lang/String;Ljava/lang/String;)V",
            &g_io_anyrtc_live_internal_NativePushObserver_onError);

    env->CallVoidMethod(obj.obj(),
         call_context.base.method_id, as_jint(code), msg.obj(), extraInfo.obj());
  }
  ```
  `jclass` 同样用 `atomic<jclass>` + `LazyGetClass` 缓存。对比 `VideoCameraCapturer.cpp` 里每次调用都现场查找的写法：

  ```cpp
  // VideoCameraCapturer::setState —— 每次调用都重新 GetMethodID，没有任何缓存
  jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "onStateChanged", "(JI)V");
  env->CallVoidMethod(context->getJavaCapturer(), methodId, (jlong) (intptr_t) this, (jint) state);
  ```
  是"未优化 vs 已优化 JNI 反射调用"的直接对比材料。

- **DirectByteBuffer 零拷贝**：`LiveEngine.cpp:887-919`（`LibYuvBridge`）用 `GetDirectBufferAddress()` 直接拿到 Java `ByteBuffer` 的裸指针交给 `libyuv` 做原地转换，全程不经过 `jbyteArray` 拷贝：

  ```cpp
  uint8_t *data_y = (uint8_t*) env->GetDirectBufferAddress(dataYBuffer);
  uint8_t *data_u = (uint8_t*) env->GetDirectBufferAddress(dataUBuffer);
  uint8_t *data_v = (uint8_t*) env->GetDirectBufferAddress(dataVBuffer);
  uint8_t *out_rgba = (uint8_t *)(env->GetDirectBufferAddress(outRgbaBuffer));
  ...
  libyuv::I420ToABGR(data_y, stride_y, data_u, stride_u, data_v, stride_v,
             out_rgba, dst_stride_rgba, src_width, src_height);
  ```
  `ArLivePlayEvent.cpp:138-186` 的 `onRenderVideoFrame` 更进一步：同一份数据**同时**提供 `jbyteArray`（拷贝版）和 `NewDirectByteBuffer`（零拷贝版）供 Java 层按需选择：

  ```cpp
  jbyteArray dataArray = jni->NewByteArray(videoFrame->length);
  // 用 videoFrame->data 指向的 native 内存直接构造 ByteBuffer，不发生拷贝，
  // 要求这块 native 内存在 Java 使用期间保持有效
  jobject _buf = jni->NewDirectByteBuffer(videoFrame->data, videoFrame->length);
  jni->SetByteArrayRegion(dataArray, 0, videoFrame->length,
                          reinterpret_cast<const jbyte *>(videoFrame->data));

  jni->CallVoidMethod(m_jJavaObj,j_callJavaMId,videoFrame->pixelFormat,videoFrame->bufferType,
                      dataArray,_buf,videoFrame->width,videoFrame->height,videoFrame->rotation);
  ```
  `videoFrame->data` 的生命周期仅在这次方法调用期间有效，是"零拷贝"必须搭配"严格的生命周期约束"的典型例子。

- **`FindClass` 的 ClassLoader 陷阱与预缓存方案**：`ClassreferenceHolder.h` 的头部注释本身就是极好的教学材料：

  ```cpp
  // Android's FindClass() is trickier than usual because the app-specific
  // ClassLoader is not consulted when there is no app-specific frame on the
  // stack.  Consequently, we only look up all classes once in app/webrtc.
  // http://developer.android.com/training/articles/perf-jni.html#faq_FindClass
  ```
  标准解法是提前在已知安全的调用栈下把常用类 `FindClass`+`NewGlobalRef` 缓存进全局 `map<string, jclass>`：

  ```cpp
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
  ```
  之后任意线程直接查表（`jclass FindClass(JNIEnv* jni, const char* name) { return g_class_reference_holder->GetClass(name); }`），不必再依赖调用栈上下文。

- **jni_generator 自动生成跳板代码**：上面 `Java_NativePushObserver_onError` 那段就是 Chromium/WebRTC 工具链自动生成的文件（`NativePushObserver_JNI.h`），每个 Java 方法都对应一份"缓存 jmethodID → CHECK_CLAZZ 校验 → CallXxxMethod"的样板代码，展示了"手写 JNI 桥接"（如 `VideoCameraCapturer.cpp`）与"代码生成 JNI 桥接"两种工程方式的差异。

- **`.Release()` 打破 RAII 自动释放**：`AndroidDeviceManager.cpp:356-364` 用 `ScopedJavaLocalRef<jobject>.Release()` 把引用所有权显式转交给 Java 调用方：

  ```cpp
  extern "C" {
  JNIEXPORT jobject Java_io_anyrtc_live_internal_VideoCapturerDevice_nativeGetJavaVideoCapturerObserver(JNIEnv *env, jclass clazz, jlong ptr) {
      // GetJavaVideoCapturerObserver 返回 ScopedJavaLocalRef，Release() 放弃其自动释放权，
      // 把裸的 local ref 所有权转交给 JNI 调用方（Java 侧），避免 ScopedJavaLocalRef
      // 析构时提前删除这个即将返回给 Java 的引用
      return AndroidDeviceManager().Inst().GetJavaVideoCapturerObserver(env).Release();
  }
  }
  ```
  是"何时该主动打破自动生命周期管理"的典型场景——如果不调用 `.Release()`，函数返回前 `ScopedJavaLocalRef` 析构会把这个即将交给 Java 的引用删掉。

- **jbyteArray Get/Release 配对（及一处多余调用）**：`ArLivePlayEvent.cpp:171-180` 的 `onRenderVideoFrame` 里能看到这个模式，但用法本身有瑕疵，是很好的反面教材：

  ```cpp
  // 先取指针、再立即释放，但这份指针从未被读写过，和前面的 SetByteArrayRegion 毫无关系——
  // 属于"现取现放"式的多余调用，仅额外增加一次 Get/Release 开销
  jni->ReleaseByteArrayElements(dataArray, jni->GetByteArrayElements(dataArray, 0), 0);
  jni->DeleteLocalRef(_buf);
  ```
  多处自定义帧注入函数（`nativeStartVirtualCamera`/`nativeSendSeiMessage`/`nativeSendCustomVideoFrame`/`nativeSendCustomAudioFrame`）遵循"`GetByteArrayElements` 取指针 → 同步消费 → `ReleaseByteArrayElements` 归还"模式，隐含"假定底层调用同步且立即消费完数据"的前提。

## 2. 并发/线程模型技术

- **继承 `rtc::Thread` 实现独立编解码线程**：`V_H264Encoder`/`V_H264Decoder`（`AvCodec.h:81, 184`）各自是一个 `rtc::Thread` 子类：

  ```cpp
  class V_H264Encoder : public rtc::Thread, public EncodedImageCallback
  {
  public:
      V_H264Encoder(AVCodecCallback&callback);
      ...
      //* For Thread
      virtual void Run();
      virtual Result OnEncodedImage(const EncodedImage& encoded_image,
                                     const CodecSpecificInfo* codec_specific_info);
  private:
      rtc::RecursiveCriticalSection buffer_critsect_;
      std::unique_ptr<VideoRenderFrames> render_buffers_;
  };

  class V_H264Decoder : public rtc::Thread, public webrtc::DecodedImageCallback
  {
  public:
      V_H264Decoder(RtcVidDecoderEvent& callback);
      ...
      virtual void Run();
      virtual int32_t Decoded(webrtc::VideoFrame& decodedImage);
  };
  ```
  采集/播放线程只管往队列里塞数据（如 `V_H264Decoder::SetVideoData` 把数据 push 进 `lst_vid_data_`），编解码线程的 `Run()` 按自己的节拍从队列取数据消费，实现关注点分离与线程解耦。

- **延迟摘除的 tick 注册表**：`RtcTick.cpp` 的 `MThreadTick::RegisteRtcTick/DoProcess` 用 `map<void*, RtcTick*>` + "先标记 unAttach、下一轮循环才真正 erase"的两阶段删除：

  ```cpp
  void MThreadTick::UnAttachRtcTick(void* ptr)
  {
      rtc::CritScope l(&cs_rtc_tick_);
      if (map_rtc_tick_.find(ptr) != map_rtc_tick_.end()) {
          map_rtc_tick_[ptr]->unAttach = true;      // 只打标记，不立即 erase
      }
  }

  void MThreadTick::DoProcess()
  {
      std::list<RtcTick*> lstUnAttach;
      {
          rtc::CritScope l(&cs_rtc_tick_);
          MapRtcTick::iterator iter = map_rtc_tick_.begin();
          while (iter != map_rtc_tick_.end()) {
              if (iter->second->unAttach) {
                  lstUnAttach.push_back(iter->second);
                  iter = map_rtc_tick_.erase(iter);      // 真正 erase 发生在这一轮遍历里，且用返回值更新 iter
              } else {
                  iter->second->OnTick();
                  iter++;
              }
          }
      }
      std::list<RtcTick*>::iterator itor = lstUnAttach.begin();
      while (itor != lstUnAttach.end()) {
          (*itor)->OnTickUnAttach();                     // 锁外回调，避免持锁时间过长
          itor++;
      }
  }
  ```
  避免了"外部线程在遍历过程中直接对同一个 map 做 erase"导致的迭代器失效问题，同时把 `OnTickUnAttach()` 回调挪到锁外执行，减少临界区大小。

- **跨线程 Proxy 封送模式**：`AndroidDeviceManager::createVideoSource`（`AndroidDeviceManager.cpp:67-74`）用 `VideoTrackSourceProxy::Create` 包一层代理：

  ```cpp
  videoSource = webrtc::CreateJavaVideoSource(env, arlive::StaticThreads::getThreads()->getMediaThread(), false, false);
  // VideoTrackSourceProxy 会把跨线程的方法调用（例如信令线程查询视频源状态，
  // 而实际视频源逻辑跑在 worker/media 线程）自动转发（marshal）到正确的线程执行
  return webrtc::VideoTrackSourceProxy::Create(
          arlive::StaticThreads::getThreads()->getMediaThread(),
          arlive::StaticThreads::getThreads()->getWorkerThread(),
          videoSource);
  ```
  使任意线程对该视频源的调用自动被转发到正确线程执行，上层调用方完全不用关心线程亲和性。

- **跨线程写入用队列缓冲，I/O 只在归属线程执行**：`ArNetClient::sendData`/`runOnce`（`ArNetClient.cpp:60-94`）按调用线程分流：

  ```cpp
  void ArNetClient::runOnce()
  {
      RTC_DCHECK(main_thread_->IsCurrent());
      if (state_ == CONNECTED) {
          webrtc::MutexLock l(&cs_send_data_);
          while (lst_send_data_.size() > 0) {
              std::unique_ptr<ArNetData>& sendData = lst_send_data_.front();
              doSendData(sendData->pData, sendData->nLen);
              lst_send_data_.pop_front();
          }
      }
  }
  void ArNetClient::sendData(const char* pData, int nLen)
  {
      if (pData != NULL && nLen > 0)
      {
          if (main_thread_->IsCurrent()) {
              doSendData(pData, nLen);              // 已在归属线程，直接同步发送
          }
          else {
              std::unique_ptr<ArNetData> sendData = std::make_unique<ArNetData>(pData, nLen);
              webrtc::MutexLock l(&cs_send_data_);
              lst_send_data_.push_back(std::move(sendData));   // 非归属线程，加锁入队，等 runOnce() 统一 flush
          }
      }
  }
  ```
  主线程直接同步发送，非主线程加锁塞入队列由 `runOnce()` 统一 flush——是"只在固定线程做真正 I/O，其余线程只负责生产数据"模式的清晰实现。

- **防御性并发调试手段**：`StaticThreads.cpp:120` 的 `worker_->DisallowAllInvokes()`：

  ```cpp
  explicit ThreadsImpl(size_t i) {
      auto suffix = i == 0 ? "" : "#" + std::to_string(i);
      media_ = create("arlive-media" + suffix);
      worker_ = create("arlive-work"  + suffix);
      // 调用后如果有人再对 worker_ 线程执行同步的 Invoke()（阻塞等待其他线程在该线程上
      // 执行任务并返回结果），会触发断言/失败，而不是静默地阻塞或造成潜在死锁
      worker_->DisallowAllInvokes();
  }
  ```
  禁止对该线程做同步 `Invoke()`，只允许异步 `Post()`，用于提前暴露潜在的跨线程阻塞/死锁误用。

- **细粒度锁**：`FFBuffer` 的多层队列各自用独立的 `rtc::RecursiveCriticalSection`，而不是整个 buffer 共用一把大锁（`FFBuffer.h:117-126` 声明，`FFBuffer.cpp:30-65` 使用）：

  ```cpp
  // FFBuffer.h
  rtc::RecursiveCriticalSection cs_audio_recv_;
  rtc::RecursiveCriticalSection cs_video_recv_;
  rtc::RecursiveCriticalSection cs_audio_decode_;
  rtc::RecursiveCriticalSection cs_video_decode_;

  // FFBuffer.cpp::DoClear —— 四把锁各管各的队列，互不阻塞
  {
      rtc::CritScope cs(&cs_audio_recv_);
      ... // 清空 lst_audio_recv_
  }
  {
      rtc::CritScope cs(&cs_video_recv_);
      ... // 清空 lst_video_recv_
  }
  {
      rtc::CritScope cs(&cs_audio_decode_);
      ... // 清空 lst_audio_decode_
  }
  {
      rtc::CritScope cs(&cs_video_decode_);
      ... // 清空 lst_video_decode_
  }
  ```
  音频接收队列的操作不会被视频解码队列的锁阻塞，反之亦然，是"按数据流拆锁"而非"按对象拆锁"的实践。

## 3. 设计模式

- **模板方法模式**：`BitmapUtil.cpp:80-135` 的 `bitmapToByteArray<Func>` 把"lock pixels → 转换 → unlock → 打包"的通用 NDK 样板流程封装为模板函数：

  ```cpp
  template<typename Func>
  jbyteArray bitmapToByteArray(JNIEnv *env, jobject jbitmap, Func callback) {
      if (jbitmap == NULL) {
          return NULL;
      }
      AndroidBitmapInfo info;
      uint8_t *pixels;
      AndroidBitmap_getInfo(env, jbitmap, &info);
      AndroidBitmap_lockPixels(env, jbitmap, reinterpret_cast<void **>(&pixels));

      uint8_t *target_data = NULL;
      int dataSize = 0;
      int code = -1;
      // 具体像素格式转换逻辑由外部传入的 callback（差异化步骤）决定
      if (info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) {
          code = callback(&target_data, info.width, info.height, &dataSize, pixels, ARGB_8888);
      } else if (info.format == ANDROID_BITMAP_FORMAT_RGB_565) {
          code = callback(&target_data, info.width, info.height, &dataSize, pixels, RGB_565);
      }

      if (code != 0) {
          AndroidBitmap_unlockPixels(env, jbitmap);      // 无论成功失败都要解锁（通用步骤）
          if (target_data != NULL) { delete[] target_data; }
          return NULL;
      }
      jbyteArray res = env->NewByteArray(dataSize);
      env->SetByteArrayRegion(res, 0, dataSize, reinterpret_cast<const jbyte *>(target_data));
      AndroidBitmap_unlockPixels(env, jbitmap);
      if (target_data != NULL) { delete[] target_data; }
      return res;
  }
  ```
  具体像素格式转换逻辑通过回调参数（lambda）注入，实现通用流程与差异化逻辑解耦。注意函数本身没检查 `AndroidBitmap_getInfo`/`AndroidBitmap_lockPixels` 的返回值，属于潜在风险点。

- **对象池模式**：`V_H264Decoder` 的 `VidData` 解码消费完后进入 `lst_vid_data_cache_` 供下次复用而非 `delete`（`AvCodec.cc:655-685, 833-849`）：

  ```cpp
  // 取用：优先从缓存池里拿，没有才 new
  void V_H264Decoder::SetVideoData(bool bKeyFrame, const char* pData, int nLen)
  {
      VidData* vidData = NULL;
      rtc::CritScope l(&cs_lst_vid_data_);
      if (bKeyFrame) {  //关键帧则清空队列
          while (lst_vid_data_.size() > 0) {
              VidData* tpData = lst_vid_data_.front();
              lst_vid_data_.pop_front();
              lst_vid_data_cache_.push_back(tpData);   // 淘汰的数据回收进缓存池，而不是 delete
          }
      }
      if (lst_vid_data_cache_.size() > 0) {
          vidData = lst_vid_data_cache_.front();
          lst_vid_data_cache_.pop_front();
      }
      if (vidData == NULL) {
          vidData = new VidData();                     // 池里没有才真正分配
      }
      ...
  }

  // 归还：解码消费完的 VidData 放回池子
  void V_H264Decoder::CacheVidData(VidData* vidData)
  {
      rtc::CritScope l(&cs_lst_vid_data_);
      lst_vid_data_cache_.push_back(vidData);
  }
  ```
  `VidData::SetData` 内部还会判断 `nSize < len` 才重新分配底层 `char[]` buffer，避免了对象复用之后仍然频繁 `new[]`/`delete[]`，是"对象池 + 内部缓冲区复用"两层复用叠加的例子。

- **单例模式的两种实现对比**：`AndroidDeviceManager::Inst()`（`AndroidDeviceManager.cpp:24-36`）是裸指针懒汉单例：

  ```cpp
  static AndroidDeviceManager *gInstance = NULL;

  AndroidDeviceManager& AndroidDeviceManager::Inst(){
      if (gInstance == NULL){
          gInstance = new AndroidDeviceManager();   // 非线程安全：多线程同时首次调用可能重复 new
      }
      return *gInstance;                            // 从未 delete，进程结束前一直存活
  }
  ```
  `StaticThreads::getThreads()`（`StaticThreads.cpp:222-225`）则是 function-local static 单例：

  ```cpp
  std::shared_ptr<Threads> &getThreads() {
      // C++11 起标准保证函数内 static 局部变量的初始化是线程安全的，
      // 不需要额外加锁就能安全地做到“惰性 + 单例”
      static std::shared_ptr<Threads> threads = std::make_shared<ThreadsImpl>(0);
      return threads;
  }
  ```
  同一仓库里两种写法并存（前者非线程安全但简单直白，后者线程安全且是现代 C++ 推荐写法），适合作为"单例实现优劣"的讨论素材。

- **平台抽象/桥接模式**：`PlatformImpl.cpp` 是纯条件编译分发层，把公共代码与平台差异实现解耦：

  ```cpp
  rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> createPlatformVideoSouce() {
  #ifdef WEBRTC_WIN
      return rtc::make_ref_counted<WinVideoTrackSource>();
  #elif defined(WEBRTC_ANDROID)
      return AndroidDeviceManager::Inst().createVideoSource();
  #elif defined(WEBRTC_IOS)
      return rtc::make_ref_counted<webrtc::ObjCVideoTrackSource>();
  #endif
      return NULL;
  }
  ```
  每一个 `createPlatformXxx`/`startPlatformXxx`/`stopPlatformXxx` 函数都是同样的"按平台宏分发到对应实现类"套路；`PlatformContext`（空基类）让跨平台代码只认基类指针，平台细节（Java 全局引用等）封装在 `AndroidContext` 一个文件里。

- **契约式析构检查**：`ClassreferenceHolder` 析构函数用 `RTC_CHECK` 强制要求调用方先显式 `FreeReferences()`：

  ```cpp
  ClassReferenceHolder::~ClassReferenceHolder() {
    RTC_CHECK(classes_.empty()) << "Must call FreeReferences() before dtor!";
  }
  ```
  用可预测崩溃代替静默资源泄漏——如果开发者忘记调用 `FreeReferences()` 就直接销毁对象，这里会立刻断言失败，而不是悄悄地泄漏一堆 JNI 全局引用。

## 4. 音视频处理技术

- **libyuv 的缩放/镜像/格式转换**：`AvCodec.cc:388-393` 的镜像处理：

  ```cpp
  const webrtc::I420BufferInterface* i420_buffer = frame.video_frame_buffer()->GetI420();
  libyuv::I420Mirror(i420_buffer->DataY(), i420_buffer->StrideY(), i420_buffer->DataU(), i420_buffer->StrideU(),
      i420_buffer->DataV(), i420_buffer->StrideV(),
      (uint8_t*)video_mirror_buffer_->DataY(), video_mirror_buffer_->StrideY(),
      (uint8_t*)video_mirror_buffer_->DataU(), video_mirror_buffer_->StrideU(),
      (uint8_t*)video_mirror_buffer_->DataV(), video_mirror_buffer_->StrideV(),
      video_mirror_buffer_->width(), video_mirror_buffer_->height());
  ```
  加上 `I420Scale`（裁剪/留黑边/等比三种模式）、`LiveEngine.cpp` 里的 `I420ToABGR`/`ABGRToI420`（见第 1 节 DirectByteBuffer 示例），都是 YUV 处理的标准工具函数用法范例。

- **YUV420 4 字节对齐要求**：`AvCodec.cc:358-369` 编码前把目标分辨率对齐到 4 的倍数：

  ```cpp
  if (nScaleWidth % 4 != 0) {
      nScaleWidth += (4 - nScaleWidth % 4);
      if (nScaleWidth > nSrcWidth) {
          nScaleWidth = nSrcWidth;
      }
  }
  if (nScaleHeight % 4 != 0) {
      nScaleHeight += (4 - nScaleHeight % 4);
      if (nScaleHeight > nSrcHeight) {
          nScaleHeight = nSrcHeight;
      }
  }
  ```
  这是色度采样（U/V 平面为 Y 平面的 1/2）对宽高奇偶性的硬性要求——宽高不是偶数（更严格地说这里按 4 对齐）会导致 U/V 平面尺寸计算出现小数或错位。

- **H.264 SEI RBSP 变长编码手工打包**：`H264SeiPack.cpp:7-40` 严格实现 payload_type/payload_size 的"255 累加+余数"变长编码规则：

  ```cpp
  void h264_sei_pack_internal(uint8_t* sei, int* len, uint8_t* payload, int payload_size, int payload_type)
  {
      static unsigned char start_code[] = { 0x00, 0x00, 0x00, 0x01 };
      int i, index = 0;

      memcpy(sei + index, start_code, 4);
      index += 4;
      sei[index++] = 6;                 // nalu type = SEI

      /* sei payload type：每 255 编一个 0xFF 字节，余数写最后一个字节 */
      for (i = 0; i <= payload_type - 255; i += 255) {
          sei[index++] = 255;
      }
      sei[index++] = payload_type - i;

      /* sei payload size：同样的变长编码规则 */
      for (i = 0; i <= payload_size - 255; i += 255) {
          sei[index++] = 255;
      }
      sei[index++] = payload_size - i;

      for (i = 0; i < payload_size; i++) {
          sei[index++] = payload[i];
      }

      sei[index++] = 0x80;              // rbsp_trailing_bits
      *len = index;
  }
  ```
  末尾补 `0x80` trailing bits，这是少见的"手写比特流语法，不依赖第三方 SEI 库"的完整范例；`h264_insert_sei` 则手工扫描 NALU 起始码（`00 00 01`/`00 00 00 01`）定位 SPS(7)/PPS(8)/IDR(5) 边界，把 SEI NALU 插入到 SPS/PPS 之后、slice 数据之前。

- **AAC 帧长适配**：`aacencode.cc:105-128` 的 `aac_encoder_encode_frame` 用累积缓冲区把多次输入拼接凑够一个 AAC 帧才编码：

  ```cpp
  int aac_encoder_encode_frame(void*pHandle, unsigned char* inbuf, unsigned int inlen, unsigned char* outbuf, unsigned int* outlen)
  {
      int ret = 0;
      if (pHandle != NULL) {
          AacENC* pEnc = (AacENC*)pHandle;
          if (pEnc->nPcmALen + inlen < pEnc->nPcmSize){
              // 攒的数据还不够一帧，先拷进缓冲区累积，不触发编码
              memcpy(pEnc->pPCM + pEnc->nPcmALen, inbuf, inlen);
              pEnc->nPcmALen += inlen;
              return 0;
          }
          else{
              // 够一帧了：先补满、编码，再把本次多出来的“下一帧的头部数据”滚存到缓冲区开头
              ret = pEnc->nPcmALen;
              memcpy(pEnc->pPCM + pEnc->nPcmALen, inbuf, pEnc->nPcmSize - pEnc->nPcmALen);
              int nRet = faacEncEncode(pEnc->hEncoder, (int*)pEnc->pPCM, pEnc->nInputSamples, pEnc->pOutput, pEnc->nMaxOutputBytes);
              if (nRet > 0) {
                  memcpy(outbuf , pEnc->pOutput, nRet);
                  *outlen = (unsigned int)nRet;
              }
              memcpy(pEnc->pPCM, inbuf + (pEnc->nPcmSize - pEnc->nPcmALen), inlen - (pEnc->nPcmSize - pEnc->nPcmALen));
              pEnc->nPcmALen = inlen - (pEnc->nPcmSize - pEnc->nPcmALen);
          }
      }
      return ret;
  }
  ```
  `faacEncOpen` 返回的 `nInputSamples`（通常对应 1024 samples）决定了一帧需要多少 PCM 字节，是"输入帧长与编码器要求帧长不匹配时用累积缓冲区适配"模式的标准写法。

- **FFmpeg avformat/avio 构建 RTMP 推流的完整套路**：`ArFFWriter.cpp` 展示了从建 context 到收尾的完整生命周期。开流阶段（`:490, 764, 775`）：

  ```cpp
  int error = avformat_alloc_output_context2(&format_context_, output_format, nullptr, str_url_.c_str());
  ...
  int error = avio_open2(&format_context_->pb, format_context_->url, AVIO_FLAG_WRITE, nullptr, nullptr);
  ...
  int error = avformat_write_header(format_context_, &options);
  ```
  收尾阶段（`Release()`）：

  ```cpp
  if (format_context_->pb != nullptr)
  {
      av_write_trailer(format_context_);
      avformat_close_input(&format_context_);
  }
  ```
  这里只做最简略展示——`avformat_alloc_output_context2` → 添加 `AVStream` → 设置 `codecpar` → `avio_open2` → `avformat_write_header` → `av_interleaved_write_frame` → `av_write_trailer` 的完整流程和其中几个反直觉细节（比如 FLV 分支下时间戳被刻意覆盖），已经在配套文档《ArLiveLite用到的ffmpeg api.md》里详细讲解，本文档不重复展开。

- **PTS/DTS 的 timebase rescale**：`ArFFWriter.cpp:311-312` 把毫秒时间戳转换到目标流的 timebase：

  ```cpp
  av_packet.pts = pts;
  av_packet_rescale_ts(&av_packet, AVRational{ 1, 1000 }, aud_stream_->time_base);
  ```
  是跨时间基准转换的标准 API 用法（尽管 FLV 分支里紧接着又用 `av_packet.pts = pts; av_packet.dts = dts;` 把这个转换结果覆盖掉了，具体原因见"难点和复杂"文档）。

## 5. 内存/资源管理

- **RAII + 显式契约检查并存**：`ClassreferenceHolder` 既用 `NewGlobalRef`/`DeleteGlobalRef` 做资源管理，又用析构期 `RTC_CHECK` 兜底校验：

  ```cpp
  void ClassReferenceHolder::FreeReferences(JNIEnv* jni) {
    for (std::map<std::string, jclass>::const_iterator it = classes_.begin();
        it != classes_.end(); ++it) {
      jni->DeleteGlobalRef(it->second);       // 手动资源管理：逐个释放全局引用
    }
    classes_.clear();
  }

  ClassReferenceHolder::~ClassReferenceHolder() {
    RTC_CHECK(classes_.empty()) << "Must call FreeReferences() before dtor!";   // 兜底契约检查
  }
  ```
  是"自动管理 + 人工兜底"两种手段结合的例子——正常路径靠约定调用 `FreeReferences()` 完成释放，析构函数用断言防止有人忘记调用。

- **裸指针句柄 vs `shared_ptr`/`unique_ptr` 混用**：`InstanceHolder`（`LiveEngine.cpp:48-51`）内部同时持有 `shared_ptr<PlatformContext>`（自动管理）和裸指针 `IArLive2Engine*`（手动管理）：

  ```cpp
  struct InstanceHolder {
      std::shared_ptr<PlatformContext> _platformContext;   // 自动管理：引用计数归零自动释放
      IArLive2Engine *arLiveEngine;                         // 手动管理：需要显式调用释放接口/delete
  };
  ```
  体现真实生产代码里"新旧风格并存、迁移不彻底"的常见状态，适合作为代码审查/重构练习的真实素材：如果只看 `_platformContext` 会误以为整个 holder 都是自动管理的，实际上 `arLiveEngine` 的生命周期完全依赖调用方是否记得手动释放。
