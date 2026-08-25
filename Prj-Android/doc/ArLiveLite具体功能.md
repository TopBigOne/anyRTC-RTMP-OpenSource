# ArLiveLite 具体功能清单

> 按功能模块组织，每项标注实现文件与关键行号，并配以从源码原文摘出的代码片段（原样保留，未做格式化或"修正"）。整体架构/数据流见 [ArLiveLite技术架构.md](./ArLiveLite技术架构.md)。
>
> 编码说明：`ArLiveLite/` 下部分 `.cc/.cpp` 源文件以 GBK 保存，用 UTF-8 读取时中文注释会显示为乱码（如 `//???Ҫ??open`）。下文引用到这类行时，要么原样保留乱码（说明"文件本来就是这样"），要么用文字另行转述注释含义，不会杜撰一个"修正后"的中文注释。

## 1. 引擎生命周期与音频设备管理

- `ArLive2Engine` 单例：创建/销毁推流器（`createArLivePusher`）、播放器（`createArLivePlayer`），持有全局唯一的真实音频采集/播放 `AudioDeviceModule`（`audio_device_ptr_`）与一个仅供 `PeerConnectionFactory` 占位用的 `kDummyAudio`（`ArLive2Engine.cpp:427`）。注意 `rtc_adm_` 是"假"设备，真实麦克风数据是通过下面的 `RecordedDataIsAvailable` 手工转发进 PeerConnection 体系的：

  ```cpp
  rtc_adm_ = webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kDummyAudio, task_queue_factory_.get());
  rtc_adm_->Init();

  peer_connection_factory_ = webrtc::CreatePeerConnectionFactory(
      nullptr /* network_thread */, this /* worker_thread */,
      nullptr /* signaling_thread */, rtc_adm_ /* default_adm */,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      webrtc::CreateBuiltinVideoEncoderFactory(),
      webrtc::CreateBuiltinVideoDecoderFactory(), nullptr /* audio_mixer */,
      nullptr /* audio_processing */);
  ```

- 多路 pusher/player 通过 `AttachAudCapture`/`AttachAudSpeaker` 引用计数 map 共享同一份硬件音频通道（`ArLive2Engine.cpp:462-491`）。核心是"map 从 0→1 才真正启动硬件采集，之后只加引用"：

  ```cpp
  void ArLive2Engine::AttachAudCapture(AudDevCaptureEvent* pEvent)
  {
      ...
      bool needStartCaptuer = false;
      {
          rtc::CritScope l(&cs_aud_capture_);
          if (map_aud_dev_capture_.find(pEvent) == map_aud_dev_capture_.end()) {
              if (map_aud_dev_capture_.size() == 0) {
                  needStartCaptuer = true;
              }
              map_aud_dev_capture_[pEvent] = pEvent;
          }
      }

      if (needStartCaptuer) {
          if (!audio_device_ptr_->Recording()) {
              audio_device_ptr_->InitRecording();
              if (audio_device_ptr_->StartRecording() != 0) {
                  audio_device_ptr_->StopRecording();
                  b_aud_cap_exception_ = true;
              }
              ...
          }
      }
  }
  ```
  `DetachAudCapture` 是对称的减引用逻辑，size 减到 0 才真正 `StopRecording()`。

- `RecordedDataIsAvailable`（WebRTC `AudioTransport` 回调）把麦克风 PCM 分发给所有注册的 pusher（`ArLive2Engine.cpp:321-338`）：

  ```cpp
  int32_t ArLive2Engine::RecordedDataIsAvailable(const void* audioSamples, const size_t nSamples,
      const size_t nBytesPerSample, const size_t nChannels, const uint32_t samplesPerSec, const uint32_t totalDelayMS,
      const int32_t clockDrift, const uint32_t currentMicLevel, const bool keyPressed, uint32_t& newMicLevel)
  {
      rtc::CritScope l(&cs_aud_capture_);
      MapAudDevCapture::iterator itadr = map_aud_dev_capture_.begin();
      while (itadr != map_aud_dev_capture_.end()) {
          itadr->first->RecordedDataIsAvailable(audioSamples, nSamples, nBytesPerSample, nChannels, samplesPerSec, totalDelayMS);
          itadr++;
      }
      ...
      return 0;
  }
  ```
  注意点：这里是**一份采集数据广播给多路 pusher**，而不是每路 pusher 各自开一份硬件采集，这也是为什么上面要做引用计数。

## 2. 摄像头采集与设备控制（JNI 层）

- `VideoCameraCapturer`（`jni/android/VideoCameraCapturer.cpp`）：单路摄像头/屏幕采集会话的 native 封装，本身不采集数据，构造函数把 `this` 指针强转成 `jlong` 传给 Java 层保存，形成"native 对象句柄"往返模式（`.cpp:14-41`）：

  ```cpp
  VideoCameraCapturer::VideoCameraCapturer(rtc::scoped_refptr<webrtc::JavaVideoTrackSourceInterface> source, std::string deviceId,int width,int height,int fps, std::shared_ptr<PlatformContext> platformContext) : _platformContext(platformContext) {
      AndroidContext *context = (AndroidContext *) platformContext.get();
      JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
      jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(), "init", "(JLjava/lang/String;III)V");
      env->CallVoidMethod(context->getJavaCapturer(), methodId, (jlong) (intptr_t) this, env->NewStringUTF(deviceId.c_str()),width,height,fps);
  }
  ```
  值得注意：`VideoCameraCapturer::isFrontCamera()`、`switchCamera()`、`setAudioRoute()`、`setSystemVolumeType()` 这几个继承自 `ArDeviceManager` 的虚函数在这个类里函数体是**空的、没有 return 语句**（未定义行为），但代码库里从未真正调用到这几个具体重载——真正干活的是下面 `AndroidDeviceManager` 的同名方法，所以是死代码，不是 bug。

- `AndroidDeviceManager`（`jni/liveEngine/AndroidDeviceManager.cpp`）：
  - 创建 `VideoTrackSource`（`createVideoSource`，`.cpp:56-75`），用 `VideoTrackSourceProxy` 包一层跨线程代理：

    ```cpp
    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> AndroidDeviceManager::createVideoSource(){
        JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
        if (videoSource){
            return videoSource;   // 已创建过就复用
        }
        videoSource = webrtc::CreateJavaVideoSource(env, arlive::StaticThreads::getThreads()->getMediaThread(), false,
                                                        false);
        return webrtc::VideoTrackSourceProxy::Create(arlive::StaticThreads::getThreads()->getMediaThread(), arlive::StaticThreads::getThreads()->getWorkerThread(),
                                                     videoSource);
    }
    ```

  - 切换前后摄像头（`switchCamera`，`.cpp:180-199`），策略是"先销毁旧 capturer 再重建新的"，而不是让同一个 capturer 切换内部摄像头 id：

    ```cpp
    jint AndroidDeviceManager::switchCamera(bool isFront){
        if (isScreenCapture)
            return ArLIVE_ERROR_REFUSED;
        if (videoSource) {
            //this should outlive the capturer
            _capturer = nullptr;   // 先析构旧的 VideoCameraCapturer，停止旧摄像头
            _capturer=std::make_unique<VideoCameraCapturer>(videoSource, isFront?"front":"back",captureWidth,captureheight,captureFps, _platformContext);
        }
        if (_capturer!= nullptr){
            _capturer->setState(_state);   // 恢复切换前的 Active/Inactive 状态
            return ArLiveCode::ArLIVE_OK;
        }
        return ArLiveCode::ArLIVE_ERROR_REFUSED;
    }
    ```

  - 反射实例化 Java 端 `DefaultVideoEncoderFactory` 并包装回 native 接口（`makeVideoEncoderFactory`，`.cpp:100-133`）：

    ```cpp
    std::unique_ptr<webrtc::VideoEncoderFactory> AndroidDeviceManager::makeVideoEncoderFactory() {
        JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
        AndroidContext *context = (AndroidContext *) _platformContext.get();
        jmethodID methodId = env->GetMethodID(context->getJavaCapturerClass(),
                                              "getSharedEGLContext",
                                              "()Lorg/webrtc/EglBase$Context;");
        jobject eglContext = env->CallObjectMethod(context->getJavaCapturer(), methodId);

        webrtc::ScopedJavaLocalRef<jclass> factory_class = webrtc::GetClass(env,
                                                                            "org/webrtc/DefaultVideoEncoderFactory");
        jmethodID factory_constructor = env->GetMethodID(factory_class.obj(), "<init>",
                                                         "(Lorg/webrtc/EglBase$Context;ZZ)V");
        webrtc::ScopedJavaLocalRef<jobject> factory_object(env, env->NewObject(factory_class.obj(),
                                                                               factory_constructor,
                                                                               eglContext, false,
                                                                               true));
        return webrtc::JavaToNativeVideoEncoderFactory(env, factory_object.obj());
    }
    ```
    要点：硬件编码优先靠共享 EGL 上下文，`enableIntelVp8Encoder=false`、`enableH264HighProfile=true`。`makeVideoDecoderFactory` 是同款套路。

  - 变焦、对焦、闪光灯、美颜参数全部是"查不到 capturer 就返回失败码"的透传，例如 `enableCameraTorch`：把 Java 侧的 boolean 结果转换为 native 的 0/-1 结果码。

## 3. 视频编码（`V_H264Encoder`，`ArLiveLite/codec/AvCodec.cc:208-576`）

- 摄像头/自定义帧接入：`Encode(const webrtc::VideoFrame&)`（`:311-340`），非零旋转角度会立即用 `I420Buffer::Rotate` 转正，再统一走 `AddToFrameList`：

  ```cpp
  void V_H264Encoder::Encode(const webrtc::VideoFrame& frame)
  {
      if (!running_)
          return;

      if (frame.video_frame_buffer()->GetI420() != NULL && frame.rotation() == webrtc::kVideoRotation_0) {
          webrtc::VideoFrame copy_frame(frame);
          copy_frame.set_timestamp_us(rtc::TimeMicros());
          AddToFrameList(copy_frame);
      }
      else {
          /* Apply pending rotation. */
          rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer(frame.video_frame_buffer());
          webrtc::VideoFrame rotated_frame(frame);
          if (buffer->GetI420() != NULL) {
              rotated_frame.set_video_frame_buffer(
                  webrtc::I420Buffer::Rotate(*buffer->GetI420(), frame.rotation()));
          }
          ...
          rotated_frame.set_rotation(webrtc::kVideoRotation_0);
          AddToFrameList(rotated_frame);
      }
  }
  ```

- 分辨率适配三种模式并支持镜像，核心缩放函数是 `ScaleToReqYuvCrop/Fit/ScaleToReqYuv`（`:102-206`）。以 `VideoScaleModeFill`（裁剪铺满）为例，先按目标宽高比算出源图上要裁的区域，再对齐到 4 的倍数（YUV420 色度平面硬性要求），最后调用一次 `libyuv::I420Scale` 完成裁剪+缩放：

  ```cpp
  void ScaleToReqYuvCrop(const webrtc::I420BufferInterface* i420_src, const uint8_t* yPtr, const uint8_t* uPtr, const uint8_t* vPtr, int ww, int hh, int stride)
  {
      ...
      int nScaleWidth = nSrcWidth;
      int nScaleHeight = (int)((float)nScaleWidth / realF);
      if (nScaleHeight > nSrcHeight) {
          nScaleHeight = nSrcHeight;
          nScaleWidth = (int)((float)nSrcHeight * realF);
      }
      if (nScaleWidth % YUV_B_SIZE != 0) {
          nScaleWidth += (YUV_B_SIZE - nScaleWidth % YUV_B_SIZE);
          if (nScaleWidth > nSrcWidth) { nScaleWidth = nSrcWidth; }
      }
      ...
      libyuv::I420Scale(buf0_y + offsetW, i420_src->StrideY(), buf0_u + offsetW / 2, i420_src->StrideU(),
          buf0_v + offsetW / 2, i420_src->StrideV(), nScaleWidth, nScaleHeight, (uint8_t*)yPtr, stride, (uint8_t*)uPtr, stride / 2, (uint8_t*)vPtr, stride / 2, ww, hh, libyuv::kFilterBilinear);
  }
  ```

- 镜像通过 `libyuv::I420Mirror` 处理（`:379-393`），与缩放共用同一次内存操作（先镜像到 `video_mirror_buffer_`，再对镜像后的 buffer 做缩放）：

  ```cpp
  if (b_mirror_) {
      ...
      const webrtc::I420BufferInterface* i420_buffer = frame.video_frame_buffer()->GetI420();
      libyuv::I420Mirror(i420_buffer->DataY(), i420_buffer->StrideY(), i420_buffer->DataU(), i420_buffer->StrideU(),
          i420_buffer->DataV(), i420_buffer->StrideV(),
          (uint8_t*)video_mirror_buffer_->DataY(), video_mirror_buffer_->StrideY(), (uint8_t*)video_mirror_buffer_->DataU(), video_mirror_buffer_->StrideU(),
          (uint8_t*)video_mirror_buffer_->DataV(), video_mirror_buffer_->StrideV(), video_mirror_buffer_->width(), video_mirror_buffer_->height());

      if (e_scale_mode_ == VideoScaleModeFill) {
          ScaleToReqYuvCrop(video_mirror_buffer_, ...);
      }
      ...
  }
  ```

- 独立编码线程（`Run()`，`:428-511`）：从 `VideoRenderFrames` 队列按时间戳节拍取帧，编码前检测分辨率变化并重建编码器，每 3000ms 强制插入一次关键帧：

  ```cpp
  if (frame_to_render) {
      if(h264_.width != frame_to_render->width() || h264_.height != frame_to_render->height())
      {
          h264_.width = frame_to_render->width();
          h264_.height = frame_to_render->height();
          if(encoder_) { encoder_->Release(); encoder_ = NULL; }
          video_encode_buffer_ = NULL;
      }
      if(encoder_ == NULL) { NewVideoEncoder(); }
      ...
      if (n_next_keyframe_time_ <= rtc::TimeUTCMillis()) {
          n_next_keyframe_time_ = rtc::TimeUTCMillis() + 3000;
          need_keyframe_ = true;
      }
      std::vector<webrtc::VideoFrameType> next_frame_types(1, webrtc::VideoFrameType::kVideoFrameDelta);
      if (need_keyframe_) {
          need_keyframe_ = false;
          next_frame_types[0] = webrtc::VideoFrameType::kVideoFrameKey;
      }
      if(encoder_) {
          int ret = encoder_->Encode(*frame_to_render, &next_frame_types);
      }
  }
  ```
  外部主动 `RequestKeyFrame()` 只是把 `need_keyframe_` 置 true，真正强插关键帧的判断点就在这里。

- 编码器创建（`NewVideoEncoder`，`:525-558`）：优先尝试外部注入的硬件编码器工厂，`InitEncode` 失败才回退到 `webrtc::H264Encoder::Create()`（openh264 软编码）：

  ```cpp
  void V_H264Encoder::NewVideoEncoder()
  {
      std::string strCodecName = "H264";
      webrtc::SdpVideoFormat sdpParam(strCodecName);
      sdpParam.parameters[cricket::kH264FmtpPacketizationMode] = "1";

      std::unique_ptr<VideoEncoder> extern_encoder = NULL;
      if (video_encoder_factory_ != NULL)
          extern_encoder = video_encoder_factory_->CreateVideoEncoder(sdpParam);
      if (extern_encoder != NULL)
      {// Try to use encoder use H/W
          if (extern_encoder->InitEncode(&h264_, 1, 0) == WEBRTC_VIDEO_CODEC_OK)
              encoder_.reset(extern_encoder.release());
          else
              extern_encoder = NULL;
      }
      if (encoder_ == NULL)
      {// Use software codec
          encoder_ = webrtc::H264Encoder::Create(cricket::VideoCodec(sdpParam));
          if (encoder_->InitEncode(&h264_, 1, 0) != WEBRTC_VIDEO_CODEC_OK) { assert(false); }
      }
      encoder_->RegisterEncodeCompleteCallback(this);
  }
  ```

- 码率动态调整：`UpdateBitrate`（`:256-266`），通过 `VideoBitrateAllocation` 只设 spatial/temporal layer 0 的码率，再调用 `SetRates`：

  ```cpp
  void V_H264Encoder::UpdateBitrate(int bitrate)
  {
      h264_.startBitrate = bitrate;
      h264_.maxBitrate = bitrate;
      if(encoder_ != NULL)
      {
          webrtc::VideoBitrateAllocation allocation;
          allocation.SetBitrate(0, 0, (bitrate * 1000));
          webrtc::VideoEncoder::RateControlParameters rateCtrlParams(allocation, h264_.maxFramerate);
          encoder_->SetRates(rateCtrlParams);
      }
  }
  ```

- 编码完成回调 `OnEncodedImage`（`:513-523`）：判断关键帧、记录下次强制关键帧时间、把 NALU 数据回传给上层：

  ```cpp
  webrtc::EncodedImageCallback::Result V_H264Encoder::OnEncodedImage(
      const EncodedImage& encoded_image,
      const CodecSpecificInfo* codec_specific_info)
  {
      if (encoded_image._frameType == webrtc::VideoFrameType::kVideoFrameKey) {
          n_next_keyframe_time_ = rtc::TimeUTCMillis() + 3000;
      }
      callback_.OnEncodeDataCallback(false, encoded_image._frameType == webrtc::VideoFrameType::kVideoFrameKey, encoded_image.data(), encoded_image.size(), rtc::Time32());
      return EncodedImageCallback::Result(EncodedImageCallback::Result::OK);
  }
  ```

## 4. 音频编码（`A_AACEncoder`，`AvCodec.cc:30-97` + `codec/aacencode.cc`）

- `A_AACEncoder::Encode` 每次接收一段 PCM（通常对应 10ms），采样率/声道不匹配源配置时先用 `webrtc::acm2::ACMResampler` 重采样，再送进 FAAC：

  ```cpp
  int A_AACEncoder::Encode(const void* audioSamples, const size_t nSamples, const size_t nBytesPerSample,
          const size_t nChannels, const uint32_t samplesPerSec, const uint32_t totalDelayMS)
  {
      int status = 0;
      if(encoder_)
      {
          int16_t temp_output[kMaxDataSizeSamples];
          if (audio_record_sample_hz_ != samplesPerSec || audio_record_channels_ != nChannels) {
              int samples_per_channel_int = resampler_record_.Resample10Msec((int16_t*)audioSamples, samplesPerSec * nChannels,
                  audio_record_sample_hz_ * audio_record_channels_, 1, kMaxDataSizeSamples, temp_output);
          }
          else {
              memcpy(temp_output, audioSamples, (audio_record_sample_hz_*audio_record_channels_*sizeof(short))/100);
          }
          unsigned int outlen = 0;
          uint8_t encoded[1024];
          status = aac_encoder_encode_frame(encoder_, (uint8_t*)temp_output, (audio_record_sample_hz_*audio_record_channels_ * sizeof(short)) / 100, encoded, &outlen);
          if(outlen > 0) {
              callback_.OnEncodeDataCallback(true, false, encoded, outlen, curtime);
          }
      }
      return status;
  }
  ```

- 真正的"攒够一帧（AAC-LC 1024 samples）才编码"逻辑在 `aacencode.cc:105-128` 里，`A_AACEncoder::Encode` 每次只喂 10ms 数据，`aac_encoder_encode_frame` 内部用 `pEnc->pPCM` 累积，攒够 `nPcmSize` 才真正调用 `faacEncEncode`，多余样本平移到下一轮：

  ```cpp
  int aac_encoder_encode_frame(void*pHandle, unsigned char* inbuf, unsigned int inlen, unsigned char* outbuf, unsigned int* outlen)
  {
      int ret = 0;
      if (pHandle != NULL) {
          AacENC* pEnc = (AacENC*)pHandle;
          if (pEnc->nPcmALen + inlen < pEnc->nPcmSize){
              memcpy(pEnc->pPCM + pEnc->nPcmALen, inbuf, inlen);
              pEnc->nPcmALen += inlen;
              return 0;   // 没攒够，先不编码
          }
          else{
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

- 输出裸 AAC 帧（无 ADTS 头）：`aac_encoder_open` 里 `outputFormat = mp4 ? 0 : 1`（0=raw，1=ADTS），调用处传的是 `mp4=true`，所以吐出来的是裸帧，需要靠 FLV 的 AudioSpecificConfig 单独描述参数：

  ```cpp
  void*aac_encoder_open(char ucAudioChannel, int u32AudioSamplerate, int u32PCMBitSize, int audBitrate, bool mp4)
  {
      ...
      faacEncConfigurationPtr pConfiguration = faacEncGetCurrentConfiguration(pEnc->hEncoder);
      pConfiguration->inputFormat = FAAC_INPUT_16BIT;
      /*0 - raw; 1 - ADTS*/
      pConfiguration->outputFormat = mp4 ?0:1;
      pConfiguration->useTns = DEFAULT_TNS;
      pConfiguration->aacObjectType = objectType;
      pConfiguration->mpegVersion = mpegVersion;
      pConfiguration->bitRate = audBitrate;
      faacEncSetConfiguration(pEnc->hEncoder, pConfiguration);
      return pEnc;
  }
  ```

## 5. 视频解码（`V_H264Decoder`，`AvCodec.cc:581-838`）

- 独立解码线程，维护 `lst_vid_data_` / `lst_vid_data_cache_` 双队列做对象复用（避免频繁 new/delete）。关键帧到达时清空队列中所有陈旧数据（`:655-685`），并顺手解析出自定义的 `'*'` 魔术字节前缀（5 字节：`* rotate 00 00 00`，携带旋转角度等带外元数据）：

  ```cpp
  void V_H264Decoder::SetVideoData(bool bKeyFrame, const char* pData, int nLen)
  {
      has_video_ = true;
      VidData* vidData = NULL;
      rtc::CritScope l(&cs_lst_vid_data_);
      if (bKeyFrame) {//关键帧则清空队列
          while (lst_vid_data_.size() > 0) {
              VidData* tpData = lst_vid_data_.front();
              lst_vid_data_.pop_front();
              lst_vid_data_cache_.push_back(tpData);
          }
      }
      if (lst_vid_data_cache_.size() > 0) {
          vidData = lst_vid_data_cache_.front();
          lst_vid_data_cache_.pop_front();
      }
      if (vidData == NULL) { vidData = new VidData(); }
      if (pData[0] == '*') {
          // 5bytes: * 00(rotate) 00 00 00
          vidData->SetData(bKeyFrame, pData + 5, nLen - 5);
          vidData->nRotate = pData[1];
      }
      else {
          vidData->SetData(bKeyFrame, pData, nLen);
      }
      lst_vid_data_.push_back(vidData);
  }
  ```
  这样处理避免了"关键帧后面又混入过期非关键帧"导致花屏。

- 解码线程 `Run()`（`:693-765`）：解码器懒创建时先用硬编码的 `640x480` 初始化，首个关键帧到达后会扫描 NALU 找 SPS(`nType==7`)/PPS(`nType==8`)，但 `h264_decode_seq_parameter_set(...)` 那行调用被注释掉了——也就是说这段 SPS 解析代码目前**没有真正生效**，宽高上报用的仍是写死的 640x480，这正是"首帧分辨率上报存在延迟"的根因：

  ```cpp
  webrtc::VideoCodec codecSet;
  codecSet.codecType = webrtc::kVideoCodecH264;
  codecSet.width = 640;
  codecSet.height = 480;
  vid_decoder_->InitDecode(&codecSet, 2);
  ...
  if (!has_decoded_ && vidData->bKeyFrame)
  {
      has_decoded_ = true;
      int w = 640;
      int h = 480;
      ...
      while ((nFind + 4) < vidData->nLen)
      {
          char* ptr = vidData->pData + nFind;
          if (ptr[0] == 0x0 && ptr[1] == 0x0 && ptr[2] == 0x0 && ptr[3] == 0x1)
          {
              int nType = ptr[4] & 0x1f;
              if (nType == 7) { nFind7 = nFind; }
              if (nType == 8)
              {
                  if (nFind7 >= 0)
                  {
                      int size7 = nFind - nFind7 - 4;
                      char* ptr7 = vidData->pData + nFind7 + 4;
                      //h264_decode_seq_parameter_set((uint8_t*)ptr7, size7, w, h);   // 被注释掉，w/h 从未真正被 SPS 更新
                  }
                  break;
              }
          }
          nFind++;
      }
      v_width_ = w;   // 恒为 640
      v_height_ = h;  // 恒为 480
      if (status_event_ != NULL) { status_event_->OnFirstRemoteVideoDecoded(str_idd_.c_str(), w, h); }
  }
  ```

- 解码结果通过 `Decoded()` 回调（`webrtc::DecodedImageCallback`）上报（`:807-831`），真正准确的宽高在这里通过 `decodedImage.width()/height()` 拿到并触发 `OnRemoteVideoFrameSizeChange`：

  ```cpp
  int32_t V_H264Decoder::Decoded(webrtc::VideoFrame& decodedImage)
  {
      if (!has_dec_frame_) {
          has_dec_frame_ = true;
          if (status_event_ != NULL) {
              status_event_->OnFirstRemoteVideoFrame(str_idd_.c_str(), decodedImage.width(), decodedImage.height());
          }
      }
      if (v_width_ != decodedImage.width() || v_height_ != decodedImage.height())
      {
          v_width_ = decodedImage.width();
          v_height_ = decodedImage.height();
          if (status_event_ != NULL) { status_event_->OnRemoteVideoFrameSizeChange(str_idd_.c_str(), v_width_, v_height_); }
      }
      const webrtc::I420BufferInterface* yuv420 = decodedImage.video_frame_buffer()->GetI420();
      callback_.OnDecodeFrame(str_idd_.c_str(), (char*)yuv420->DataY(), (char*)yuv420->DataU(), (char*)yuv420->DataV(), yuv420->StrideY(), yuv420->StrideU(), yuv420->StrideV(), yuv420->width(), yuv420->height(), decodedImage.rotation(), decodedImage.timestamp());
      return 0;
  }
  ```

## 6. 推流传输（`ARFFPusher` + `ArFFWriter`）

- 根据 URL scheme 选择 FFmpeg muxer：`rtmp://` → `"flv"`，`rtp://` → `"rtp_mpegts"`，`rtsp://` → `"rtsp"`（`ARFFPusher.cpp:30-58`）：

  ```cpp
  int ARFFPusher::startTask(const char* strUrl)
  {
      ...
      if (!b_pushed_) {
          b_pushed_ = true;
          b_need_keyframe_ = true;
          n_retry_times_ = 0;
          webrtc::MutexLock l(&cs_ar_writer_);
          if (ar_writer_ == NULL) {
              ar_writer_ = createArWriter();
              ar_writer_->SetCallback(this);
              ar_writer_->SetAutoRetry(true);
              if (strstr(strUrl, "rtmp://") != NULL) {
                  ar_writer_->StartTask("flv", strUrl);
              }
              else if (strstr(strUrl, "rtp://") != NULL) {
                  ar_writer_->StartTask("rtp_mpegts", strUrl);
              }
              else if (strstr(strUrl, "rtsp://") != NULL) {
                  ar_writer_->StartTask("rtsp", strUrl);
                  ar_writer_->SetEncType(CT_H264, CT_G711A);
              }
          }
      }
      return 0;
  };
  ```

- 关键帧门控：连接成功后，`setVideoData`/`setAudioData` 会一直丢弃数据直到遇到第一个视频关键帧，确保推流侧发出的第一帧一定是 IDR（`ARFFPusher.cpp:118-146`）：

  ```cpp
  int ARFFPusher::setVideoData(const char* pData, int nLen, bool bKeyFrame, uint32_t ts)
  {
      if (b_need_keyframe_) {
          if (bKeyFrame) {
              b_need_keyframe_ = false;
          }
      }
      if (!b_need_keyframe_) {
          webrtc::MutexLock l(&cs_ar_writer_);
          if (ar_writer_ != NULL) {
              int64_t pts = rtc::TimeUTCMillis() - n_push_time_;
              ar_writer_->SetVideoEncData(pData, nLen, bKeyFrame, pts, pts);
          }
      }
      return 0;
  };
  ```
  `setAudioData` 同样受 `b_need_keyframe_` 控制——在拿到第一个视频关键帧之前，连音频也一起丢弃。PTS/DTS 用"连接成功那一刻置零的挂钟毫秒计数"生成，PTS=DTS（无 B 帧）。

- 状态机：`WS_Init → WS_Connecting → WS_Connected`，失败或写包出错触发 `WS_Failed` → 按 `b_auto_retry_` 决定 1500ms 后重试或结束（`ArFFWriter::RunOnce`，`:231-269`）：

  ```cpp
  void ArFFWriter::RunOnce()
  {
      if (n_next_retry_time_ != 0 && n_next_retry_time_ <= rtc::Time32()) {
          n_next_retry_time_ = 0;
          ArWriterState oldState = now_state_;
          now_state_ = WS_Connecting;
          if (callback_ != NULL) { callback_->OnArWriterStateChange(oldState, now_state_); }
          bool bRet = Connect();
          oldState = now_state_;
          if (!bRet) {
              now_state_ = WS_Failed;
              if (callback_ != NULL) { callback_->OnArWriterStateChange(oldState, now_state_); }
              Release();
              if (b_auto_retry_) {
                  n_next_retry_time_ = rtc::Time32() + 1500;
              }
              else {
                  oldState = now_state_;
                  now_state_ = WS_Ended;
                  if (callback_ != NULL) { callback_->OnArWriterStateChange(oldState, now_state_); }
              }
          }
          else {
              now_state_ = WS_Connected;
              if (callback_ != NULL) { callback_->OnArWriterStateChange(oldState, now_state_); }
          }
      }
  }
  ```
  每次断线重连都是**完整重建** `AVFormatContext`（`Connect()` 重新握手），不是断线续传。

- 首帧 extradata（SPS/PPS）通过临时构建一个独立的 libx264 `AVCodecContext` 生成（`ArFFWriter.cpp:600-644`），`avcodec_open2` 失败则用硬编码兜底数组：

  ```cpp
  int res = avcodec_open2(vidEncodeCtx, codec, &param);
  if (res < 0) {
      char buff[128] = { 0 };
      if (vid_codec_type_ == CT_H264)
      {
          unsigned char sps_pps[23] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x0a, 0xf8, 0x0f, 0x00, 0x44, 0xbe, 0x8,
                0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80 };
          codecpar->extradata_size = 23;
          codecpar->extradata = (uint8_t*)av_malloc(23 + AV_INPUT_BUFFER_PADDING_SIZE);
          ...
          memcpy(codecpar->extradata, sps_pps, 23);
      }
  }
  else {
      if (vidEncodeCtx->extradata_size > 0 && vidEncodeCtx->extradata != NULL) {
          codecpar->extradata_size = vidEncodeCtx->extradata_size;
          codecpar->extradata = (uint8_t*)av_malloc(vidEncodeCtx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
          memcpy(codecpar->extradata, vidEncodeCtx->extradata, vidEncodeCtx->extradata_size);
      }
  }
  ```

- RTMP 兼容性处理：截取 `tcUrl`、伪装 `rtmp_flashver = "FMLE/3.0..."` 迎合 CDN 的 flashver 白名单校验（`ArFFWriter.cpp:735-745`）：

  ```cpp
  AVDictionary* options = nullptr;
  // Compatibility with specific RTMP servers
  // tc_url : rtmp://[host]:[port]/[app_name]
  if (str_url_.find("rtmp://") != std::string::npos || str_url_.find("rtmps://") != std::string::npos) {
      char tc_url[1024];
      sprintf(tc_url, "%.*s", strrchr(str_url_.c_str(), '/') - str_url_.c_str(), str_url_.c_str());
      av_dict_set(&options, "rtmp_tcurl", tc_url, 0);
      av_dict_set(&options, "fflags", "flush_packets", 0);
      av_dict_set(&options, "rtmp_flashver", "FMLE/3.0 (compatible; FMSc/1.0)", 0);
  }
  ```

- 释放时先写 trailer 再关流，`Release()`（`:801-823`）：

  ```cpp
  void ArFFWriter::Release()
  {
      if (format_context_ != nullptr)
      {
          if (format_context_->pb != nullptr)
          {
              av_write_trailer(format_context_);
              avformat_close_input(&format_context_);
          }
          avformat_free_context(format_context_);
          format_context_ = nullptr;
      }
      vid_stream_ = NULL;
      aud_stream_ = NULL;
      if (avcc_data_ != NULL) { delete avcc_data_; avcc_data_ = NULL; }
  }
  ```

## 7. 拉流/播放（`ARFFPlayer` + `FFBuffer` + `PlayBuffer`）

- `ARFFPlayer` 独立读线程 `ReadThreadProcess`（`:280-425`）：`av_read_frame` 循环读包，把 dts/pts 换算成毫秒（缺失 dts 用 pts 兜底，反之亦然），再按流类型送入 `FFBuffer`：

  ```cpp
  ret = av_read_frame(fmt_ctx_, packet);
  if (ret >= 0) {
      n_last_recv_data_time_ = rtc::Time32();
      if (packet->stream_index == n_video_stream_idx_) {
          int64_t pts = 0, dts = 0;
          if (packet->dts == AV_NOPTS_VALUE) { dts = 0; }
          else { dts = av_rescale_q(packet->dts, astream_timebase_, TIMEBASE_MS); }
          if (packet->pts == AV_NOPTS_VALUE) { pts = 0; }
          else { pts = av_rescale_q(packet->pts, astream_timebase_, TIMEBASE_MS); }
          if (dts == 0 && pts != 0) { dts = pts; }
          if (pts == 0 && dts != 0) { pts = dts; }
          if (b_no_buffer_) {
              OnBufferDecodeVideoData(packet);
              av_packet_unref(packet); delete packet;
          }
          else {
              FFBuffer::RecvVideoData(packet, dts, pts, av_rescale_q(packet->duration, vstream_timebase_, TIMEBASE_MS));
          }
      }
      ...
  }
  ```
  每轮最多连续读 5 个包就跳出（`ii > 5` break），避免读线程长期占用导致 FFmpeg 内部缓冲跟不上。

- `FFBuffer` 两级缓冲：第一级 `lst_*_recv_` 是网络到达顺序队列，`DoTick()`（`:78-172`）按"累计到量"把包从 `recv` 队列搬进第二级 `lst_*_decode_` 队列，并在起播/重缓冲时判断累计缓存时长是否达到 `n_cache_to_play_time_`：

  ```cpp
  {
      rtc::CritScope cs(&cs_video_recv_);
      if (lst_video_recv_.size() > 0) {
          vidCacheTime = lst_video_recv_.back()->dts_ - lst_video_recv_.front()->dts_;
          vidFirstDtsTime = lst_video_recv_.front()->dts_;
          if (rtc::TimeUTCMillis() - lst_video_recv_.front()->recv_time_ >= n_cacheing_time_) {
              vidCacheTime = n_cacheing_time_;
              rtc::CritScope l(&cs_video_decode_);
              lst_video_decode_.push_back(lst_video_recv_.front());
              lst_video_recv_.pop_front();
          }
      }
  }
  ...
  int dataCacheTime = audCacheTime > vidCacheTime ? audCacheTime : vidCacheTime;
  if (dataCacheTime >= n_cache_to_play_time_) {
      n_sys_played_time_ = 0;
      n_played_time_ = 0;
      decode_data_time_ = audFirstDtsTime != 0 ? audFirstDtsTime : vidFirstDtsTime;
      play_status_ = PS_Playing;
  }
  ```
  起播/重缓冲时缓存阈值 `n_cache_to_play_time_` 从 1000ms 起步，每次缓存耗尽（`vidListSize == 0 && audListSize == 0`）都自适应 +1000ms，直到封顶 `n_cache_to_play_max_`（`:280-292`）：

  ```cpp
  #define DFT_CACHEING_TIME	10000	// 10senconds
  #define MAX_CACHEING_TIME	60000	// 1mins
  ...
  if (vidListSize == 0 && audListSize == 0) {
      play_status_ = PS_Caching;
      OnBufferStatusChanged(play_status_);
      if (n_cache_to_play_time_ < n_cache_to_play_max_) {
          n_cache_to_play_time_ += 1000;
      }
      if (n_cache_to_play_time_ > n_cache_to_play_max_) {
          n_cache_to_play_time_ = n_cache_to_play_max_;
      }
  }
  ```

- `PlayBuffer`（第三级，渲染/播放节奏）：`DoVidRender`（`:123-159`）按挂钟时间 vs 帧 `pts` 决定是否该出帧，积压时循环 pop 但只在第一次 `!bRender` 时真正渲染，其余直接 `delete` 丢弃，实现"跳帧但只渲染一帧"：

  ```cpp
  int PlayBuffer::DoVidRender(bool bVideoPaused)
  {
      bool bRender = false;
      while (1) {
          VideoData* vidPkt = NULL;
          {
              rtc::CritScope cs(&cs_video_play_);
              if (lst_video_play_.size() > 0) {
                  vidPkt = lst_video_play_.front();
                  if (n_last_render_video_pts_ == 0 || n_last_render_video_pts_ > vidPkt->pts_) {
                      n_last_render_video_time_ = rtc::TimeUTCMillis();
                      n_last_render_video_pts_ = vidPkt->pts_;
                  }
                  if (vidPkt->pts_ <= (rtc::TimeUTCMillis() - n_last_render_video_time_) + n_last_render_video_pts_) {
                      lst_video_play_.pop_front();
                  }
                  else { vidPkt = NULL; }
              }
          }
          if (vidPkt != NULL) {
              if (!bVideoPaused && !b_app_in_background_) {
                  if (!bRender) {//@Eric - 跳帧处理，防止一次性输出过多
                      OnBufferVideoRender(vidPkt, vidPkt->pts_);
                  }
                  bRender = true;
              }
              delete vidPkt;
          }
          else { break; }
      }
      return 0;
  }
  ```

- 音频队列超阈值清一半并联动丢弃（音频优先的同步策略），Android 平台音频缓冲上限特殊放宽到 30（而非桌面端的 20），应对部分机型音频线程实时性较差：

  ```cpp
  #ifdef WEBRTC_ANDROID
  //android 的某些机型，音频播放的线程实时性不高，所以一次性需要更多的数据
  const int kMaxAudioPlaySize = 30;
  #else
  const int kMaxAudioPlaySize = 20;
  #endif
  const int kMaxVedeoPlaySize = kMaxAudioPlaySize / 2;
  ...
  void PlayBuffer::PlayAudioData(PcmData*pcmData)
  {
      ...
      lst_audio_play_.push_back(pcmData);
      //@Eric - 跳帧处理 - 防止缓存太多：延时增大
      if (lst_audio_play_.size() >= kMaxAudioPlaySize) {//清一半缓存
          while (lst_audio_play_.size() > kMaxAudioPlaySize/2) {
              PcmData* pkt = lst_audio_play_.front();
              dropPts = pkt->pts_;
              lst_audio_play_.pop_front();
              delete pkt;
              OnBufferAudioDropped();
          }
      }
  }
  ```

## 8. SEI 自定义消息收发

- 推流侧：`H264SeiPack::h264_insert_sei`（`ArLiveLite/H264SeiPack.cpp:42-101`）逐字节扫描关键帧 NALU，记住 SPS(`nType==7`)/PPS(`nType==8`)/SEI(`nType==6`) 的位置，遇到 Slice(`nType==5`) 时才在其之前插入自定义 SEI：

  ```cpp
  else if (nType == 5) {
      ptrFind5 = ptr + ii - limLen;
      if (ptrFind7 != NULL && ptrFind8 != NULL) {
          memcpy(pMemBuffer, ptrFind7, (ptrFind8 - ptrFind7));
          nRet += (ptrFind8 - ptrFind7);
          if (ptrFind6 != NULL) {
              memcpy(pMemBuffer + nRet, ptrFind8, (ptrFind6 - ptrFind8));
              nRet += (ptrFind6 - ptrFind8);
          }
          else {
              memcpy(pMemBuffer + nRet, ptrFind8, (ptrFind5 - ptrFind8));
              nRet += (ptrFind5 - ptrFind8);
          }
      }
      int wrSei = 0;
      h264_sei_pack_internal((uint8_t*)pMemBuffer + nRet, &wrSei, (uint8_t*)payload, payload_size, payload_type);
      nRet += wrSei;
      memcpy(pMemBuffer + nRet, ptrFind5, (n264Len - (ptrFind5 - ptr)));
      nRet += (n264Len - (ptrFind5 - ptr));
      break;
  }
  ```
  只在关键帧插入，payload type 限定 5 或 242。

- 消息先排队，只有在最近一次插入关键帧之后 250ms 内还没消费掉才会主动请求新的关键帧（`ArLive2Pusher.cpp:628-645, 717-724`）：

  ```cpp
  int32_t ArLive2Pusher::sendSeiMessage(int payloadType, const uint8_t* data, uint32_t dataSize)
  {
      ...
      if (data == NULL || dataSize <= 0 || (payloadType != 5 && payloadType != 242)) {
          return ArLIVE_ERROR_INVALID_PARAMETER;
      }
      if (!b_live_pushed_) { return ArLIVE_ERROR_REFUSED; }
      SeiMsg* seiMsg = new SeiMsg();
      seiMsg->ePayloadType = (sei_payload_type_e)payloadType;
      seiMsg->SetMsg((char*)data, dataSize);
      lst_sei_msg_.push_back(seiMsg);
      return ArLIVE_OK;
  }
  ...
  // OnTick 里：
  if (lst_sei_msg_.size() > 0) {
      if (n_last_keyframe_time_ != 0 && n_last_keyframe_time_ + 250 <= rtc::TimeUTCMillis()) {
          n_last_keyframe_time_ = 0;
          webrtc::MutexLock l(&cs_h264_encoder_);
          if (h264_encoder_ != NULL) { h264_encoder_->RequestKeyFrame(); }
      }
  }
  ```

- 播放侧：`ARFFPlayer::ParseVideoSei`（`:812-848`）兼容 Annex-B（`00 00 00 01`/`00 00 01` 起始码）与"4 字节长度前缀"两种 NALU 封装，找到 `nType==6` 的 SEI NALU 后回调出去：

  ```cpp
  void ARFFPlayer::ParseVideoSei(char* pData, int nLen, int64_t pts)
  {
      char* ptr = pData;
      if (ptr[0] == 0x00 && ptr[1] == 0x00 && ptr[2] == 0x00 && ptr[3] == 0x01) {
          // Annex-B：逐字节找起始码，nType==6 记下位置，遇到下一个 NALU 就回调 [ptrFind6, ptrFindN)
          ...
          if (nType == 6) { ptrFind6 = ptr + ii - limLen; }
          else if (ptrFind6 != NULL) {
              callback_.OnArPlySeiData(this, ptrFind6, ptrFindN - ptrFind6, pts);
              break;
          }
      }
      else {
          // 4 字节长度前缀封装：ptr[1..3] 是 NALU 长度（大端），逐个 NALU 跳
          while (nPtr < nLen) {
              if (ptr[0] != 0x00 || ptr[1] >= 0x03) { return; }
              unsigned int vpkg_len = ((int)(ptr[1] & 0xff) << 16) + ((int)(ptr[2] & 0xff) << 8) + (int)(ptr[3] & 0xff);
              int nType = ptr[4] & 0x1f;
              if (nType == 6) {
                  // 补一个 Annex-B 起始码再回调，统一给上层处理
                  ...
                  callback_.OnArPlySeiData(this, pH264Raw, vpkg_len + 4, pts);
                  break;
              }
              ptr += (4 + vpkg_len);
              nPtr += (4 + vpkg_len);
          }
      }
  }
  ```

- `ArLive2Player::OnArPlySeiData`（`:535-561`）解析 SEI 里的变长 payload 长度字段（H.264 SEI 语法：字节值为 255 表示"还没完，继续累加"，非 255 表示"这是最后一段"）：

  ```cpp
  void ArLive2Player::OnArPlySeiData(void* player, const char* pData, int nLen, int64_t pts)
  {
      int semiLen = 4;
      int nType = pData[4] & 0x1f;
      if (pData[2] == 1) { nType = pData[3] & 0x1f; semiLen = 3; }
      if (nType == 6) {
          int nPayloadType = pData[semiLen + 1] & 0xff;
          if (nPayloadType == 5 || nPayloadType == 242) {// self defined
              int nPayloadLen = 0;
              const char* sei = pData + semiLen + 2;
              int sl = 0;
              do {
                  sl = (*sei) & 0xff;
                  sei++;
                  nPayloadLen += sl;
              } while (sl == 255);
              if (observer_ != NULL) {
                  observer_->onReceiveSeiMessage(this, nPayloadType, (uint8_t*)sei, nPayloadLen);
              }
          }
      }
  }
  ```

## 9. 渲染

- `AndroidRenderer` 把 Java 层 `VideoSink` 包装为 native `VideoSinkInterface`，全部逻辑只有"转发一层"（`AndroidRenderer.cpp` 全文 29 行）：

  ```cpp
  AndroidRenderer::AndroidRenderer(const void* javaSink)
      : width_(0), height_(0)
  {
      JNIEnv *env = webrtc::AttachCurrentThreadIfNeeded();
      video_sink_=webrtc::JavaToNativeVideoSink(env,(jobject) javaSink);
  }
  void AndroidRenderer::OnFrame(const webrtc::VideoFrame& frame)
  {
      if (video_sink_ != NULL) {
          video_sink_->OnFrame(frame);
      }
  }
  ```

- `MgrRender` 按会话 ID（`pId`）管理多路预览/渲染视图，用 `map_renders_` 存储，`SetRender` 替换时会先 `delete` 旧的：

  ```cpp
  void MgrRender::SetRender(const char*pId, webrtc::VideoRenderer*render)
  {
      rtc::CritScope l(&cs_renders_);
      if (map_renders_.find(pId) != map_renders_.end()) {
          webrtc::VideoRenderer*findRender = map_renders_[pId];
          map_renders_.erase(pId);
          delete findRender;
      }
      if (render != NULL) { map_renders_[pId] = render; }
  }
  void MgrRender::DoRenderFrame(const char*pId, const webrtc::VideoFrame& frame)
  {
      rtc::CritScope l(&cs_renders_);
      if (map_renders_.find(pId) != map_renders_.end()) {
          map_renders_[pId]->OnFrame(frame);
      }
  }
  ```

- 旋转/填充模式相关 API 存在但实现为空，完整函数体就是这样（`MgrRender.cpp:27-35`），业务侧调用这两个接口目前不会产生任何效果：

  ```cpp
  void MgrRender::SetRotation(const char* pId, int nRotation)
  {

  }

  void MgrRender::SetFillMode(const char* pId, int nMode)
  {

  }
  ```

## 10. JNI 层导出的完整功能面（`LiveEngine.cpp`）

按功能分组的 native 方法，全部集中在这一个上千行的文件里，下面挑 3 组有代表性的：

- 引擎/推流器句柄创建销毁，例如 `nativeStartPush`（`:131-144`）——典型的"还原指针→调用→转返回码"三段式，指针无效时统一返回 -1：

  ```cpp
  JNIEXPORT jint JNICALL
  Java_io_anyrtc_live_internal_NativeInstance_nativeStartPush(JNIEnv *env, jobject obj,jlong nativePtr,jstring pushUrl) {
      IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr);
      if (arLivePushKit != NULL){
          std::string strPushUrl = webrtc::JavaToStdString(env, pushUrl);
          int result =arLivePushKit->startPush(strPushUrl.c_str());
          return (jint)result;
      }
      return (jint)-1;
  }
  ```

- 自定义帧/SEI 注入，例如 `nativeSendSeiMessage`（`:467-483`），用 `GetByteArrayElements`/`ReleaseByteArrayElements` 配对访问 Java `byte[]`：

  ```cpp
  JNIEXPORT jint JNICALL
  Java_io_anyrtc_live_internal_NativeInstance_nativeSendSeiMessage(JNIEnv *env, jobject thiz,jlong nativePtr,
                                                                   jint var1, jbyteArray var2) {
      IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr);
      jint result = -1;
      if (arLivePushKit!= NULL){
          jbyte* bufferPtr=(env)->GetByteArrayElements(var2, NULL);
          jint size = env->GetArrayLength(var2);
          result = arLivePushKit->sendSeiMessage(var1, reinterpret_cast<const uint8_t *>(bufferPtr), size);
          env->ReleaseByteArrayElements(var2, bufferPtr, 0);
      }
      return (jint)result;
  }
  ```

- 像素格式转换桥接：`LibYuvBridge` 的 `i420ToAbgrInternal`（`:873-919`），全程走 `GetDirectBufferAddress` 拿堆外内存地址，Java/native 之间不发生数组拷贝：

  ```cpp
  JNIEXPORT void JNICALL
  Java_org_webrtc_effector_format_LibYuvBridge_i420ToAbgrInternal(
          JNIEnv *env, jobject obj,
          jobject dataYBuffer, jint strideY,
          jobject dataUBuffer, jint strideU,
          jobject dataVBuffer, jint strideV,
          jint width, jint height,
          jobject outRgbaBuffer)
  {
      uint8_t *data_y = (uint8_t*) env->GetDirectBufferAddress(dataYBuffer);
      uint8_t *data_u = (uint8_t*) env->GetDirectBufferAddress(dataUBuffer);
      uint8_t *data_v = (uint8_t*) env->GetDirectBufferAddress(dataVBuffer);
      uint8_t *out_rgba = (uint8_t *)(env->GetDirectBufferAddress(outRgbaBuffer));

      int dst_stride_rgba = width * 4;   // 每像素 4 字节（A/B/G/R）
      libyuv::I420ToABGR(data_y, strideY,
                 data_u, strideU,
                 data_v, strideV,
                 out_rgba, dst_stride_rgba,
                 width, height);
  }
  ```
  `abgrToI420Internal` 是反方向的 `libyuv::ABGRToI420`，同样走 DirectByteBuffer 零拷贝路径。其余观察者注册（`nativeSetPushObserver`/`nativeSetPlayerobserver`）、设备控制转发（对接 `AndroidDeviceManager`）都是同款"取指针→转发调用→转返回码"套路，不再逐一展开。
