# ArLiveLite 技术架构

> 分析范围：`Prj-Android/liveplayer/src/main/cpp/jni/`（JNI 桥接层）+ `ArLiveLite/`（跨平台 C++ 引擎核心，仅覆盖经 `Prj-Android/liveplayer/src/main/cpp/CMakeLists.txt` 确认参与 Android 编译的文件）。
>
> 本文档每一条架构结论后面都配了从源码原文摘出的代码片段（路径:行号），不是转述——建议对照片段验证结论，发现和源码不一致的地方欢迎回来改这份文档。

## 1. 一句话定位

anyLive 不是一个 WebRTC P2P 通话 SDK，而是一个**用 WebRTC 的编解码器/线程模型 + FFmpeg 的封装/RTMP 传输**拼出来的推拉流（RTMP push/pull）引擎。`PeerConnection`/ICE/SDP 那套信令建连体系在这个项目里完全没有被使用。

## 2. 分层结构

```
Java 层
  org.webrtc.*                    ← 官方 WebRTC Android API（Camera1/2Capturer、EglRenderer、DefaultVideoEncoderFactory...）
  io.anyrtc.live.*                ← anyRTC 业务封装（ArLiveEngine/ArLivePusher/ArLivePlayer/ArDeviceManager）
        │  JNI 调用（native 方法）
        ▼
JNI 桥接层  Prj-Android/liveplayer/src/main/cpp/jni/
  LiveEngine.cpp                  ← 唯一的 JNI 总入口，1043 行，几乎所有 native 方法都在这
  liveEngine/ArLivePushEvent.*    ← Native→Java 回调转发（推流事件）
  liveEngine/ArLivePlayEvent.*    ← Native→Java 回调转发（播放事件）
  liveEngine/AndroidDeviceManager.* ← 摄像头/设备管理落地实现
  android/VideoCameraCapturer.*   ← 单路摄像头采集会话的 native 封装
  android/AndroidContext.*        ← 平台 Context 抽象的 Android 实现
  util/ClassreferenceHolder.*     ← 全局 jclass 缓存（解决 FindClass 陷阱）
  util/BitmapUtil.*               ← Bitmap 像素访问工具（当前是死代码，见"难点和复杂"文档）
  StaticThreads.*                 ← 全局共享的媒体/工作线程
        │  直接函数调用 / 虚接口调用
        ▼
跨平台引擎核心  ArLiveLite/（Android/iOS/Win 三端共用同一份源码）
  ArLive2Engine        ← 引擎单例，持有音频设备、WebRTC PeerConnectionFactory（仅借壳用其线程/编解码器工厂）
  ArLive2Pusher        ← 一路推流会话：采集回调 → 编码 → 推流状态机
  ArLive2Player        ← 一路拉流会话：解封装 → 解码 → 渲染/播放
  codec/AvCodec         ← V_H264Encoder/V_H264Decoder（包装 webrtc::VideoEncoder/Decoder）、A_AACEncoder
  pusher/ARFFPusher + ArFFWriter ← 推流状态机 + FFmpeg avformat RTMP 封装发送
  player/ARFFPlayer + FFBuffer  ← 拉流读线程 + 两级网络缓冲
  PlayBuffer            ← 解码后帧的第三级渲染/播放缓冲（音视频同步）
  H264SeiPack           ← H264 SEI 自定义消息比特流打包/解析
  RtcTick               ← 轻量协作式 tick 调度器
  PlatformImpl / AndroidRenderer / MgrRender ← 平台抽象与渲染管理
  ArNetClient / ArNetTcpClient / url.hpp     ← 自研网络传输层（⚠️ 当前完全未被引用，孤儿代码）
        │
        ▼
第三方库（预编译）
  libwebrtc.a (WebRTC-93)  ← 仅提供：VideoEncoder/Decoder 接口与实现、rtc::Thread、libyuv、音频设备模块
                              （pc/、p2p/ 目录在本仓库只有头文件，实际未被使用/未编入本引擎）
  FFmpeg *.so              ← avformat/avio 做 RTMP 封装、发送、拉流解封装、解码
  libfaac (third_party)    ← AAC 音频编码（供 A_AACEncoder 使用）
```

上面这份"参与 Android 编译的文件清单"不是猜的，是 `Prj-Android/liveplayer/src/main/cpp/CMakeLists.txt` 里 `add_library` 的源文件列表实际截出来的：

```cmake
# Prj-Android/liveplayer/src/main/cpp/CMakeLists.txt
add_library(
        ${NATIVE_LIB}
        SHARED
        ${CMAKE_SOURCE_DIR}/jni/LiveEngine.cpp
        ${CMAKE_SOURCE_DIR}/jni/StaticThreads.cpp
        ...
        # ------ 以下为 ArLiveLite（跨平台引擎核心）目录下参与编译的源文件 ------
        ${ARLIVE_DIR}/AndroidRenderer.cpp
        ${ARLIVE_DIR}/ArLive2Engine.cpp
        ${ARLIVE_DIR}/PlatformImpl.cpp
        ${ARLIVE_DIR}/ArLive2Player.cpp
        ${ARLIVE_DIR}/ArLive2Pusher.cpp
        ${ARLIVE_DIR}/codec/aacencode.cc
        ${ARLIVE_DIR}/codec/AvCodec.cc
        ${ARLIVE_DIR}/MgrRender.cpp
        ${ARLIVE_DIR}/PlayBuffer.cpp
        ${ARLIVE_DIR}/player/ARFFPlayer.cpp
        ${ARLIVE_DIR}/player/FFBuffer.cpp
        ${ARLIVE_DIR}/player/sonic.c
        ${ARLIVE_DIR}/pusher/ARFFPusher.cpp
        ${ARLIVE_DIR}/pusher/ArFFWriter.cpp
        ${ARLIVE_DIR}/ArNetClient.cpp
        ${ARLIVE_DIR}/ArNetTcpClient.cpp
        ${ARLIVE_DIR}/RtcTick.cpp
        ${ARLIVE_DIR}/H264SeiPack.cpp
        ${CMAKE_SOURCE_DIR}/jni/util/ClassreferenceHolder.cc
)
```

注意点：清单里确实没有 `ArLiveLite/rtmp/`（自研 librtmp+libflv）也没有 `pc/`、`p2p/` 下的任何 .cc；`ArNetClient.cpp`/`ArNetTcpClient.cpp` 虽然**被编进了 .so**，但全仓库搜索没有任何地方 `new` 它们或调用其接口——编译进去 ≠ 被使用，这是这份清单容易被误读的地方。

链接阶段也能看出 `libwebrtc.a` 是被"整体链接"（`--whole-archive`）进来的，这通常是为了保留 WebRTC 内部靠全局静态构造函数做自注册的编解码器/线程逻辑，而不是因为用到了 pc/ 里的符号：

```cmake
target_link_libraries(
        ${NATIVE_LIB}
        faac
        -Wl,--start-group
        swscale avformat avcodec avfilter swresample postproc avutil
        -Wl,--end-group
        -Wl,--whole-archive
        webrtc
        -Wl,--no-whole-archive
        OpenSLES
        ${log-lib}
)
```

## 3. 核心类关系与生命周期

- `ArLive2Engine` 是**进程内单例**，本身继承 `rtc::Thread` + `MThreadTick` + `webrtc::AudioTransport`：

```cpp
// ArLiveLite/ArLive2Engine.h:35
class ArLive2Engine : public AR::IArLive2Engine, public rtc::Thread, public MThreadTick, webrtc::AudioTransport
```

  单例的创建方式非常朴素，没有加锁、也没有用 `std::call_once`——一个全局裸指针 + 判空 new：

```cpp
// ArLiveLite/ArLive2Engine.cpp:47
static ArLive2Engine* gInstance = NULL;

AR::IArLive2Engine* V2_CALL AR::createArLive2Engine()
{
	if (gInstance == NULL) {
		gInstance = new ArLive2Engine();
	}
	return gInstance;
}
```

- 它内部持有一个 `webrtc::PeerConnectionFactory`，但**只用来复用其线程模型和内置编解码器工厂**，不创建任何 `PeerConnection`——`InitPeerConnection()` 全文都在造 ADM 和 Factory，没有一行涉及 `CreatePeerConnection`：

```cpp
// ArLiveLite/ArLive2Engine.cpp:418
void ArLive2Engine::InitPeerConnection()
{
	...
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
	...
}
```

  注意第二个参数：`this`（即 `ArLive2Engine` 自身，一个 `rtc::Thread`）被塞进去当 `worker_thread`——这就是"借壳复用线程模型"的直接证据；`network_thread`/`signaling_thread` 都传 `nullptr`，因为根本用不到信令。传给 Factory 的 ADM 是 `kDummyAudio` 占位类型，跟下面真正干活的 ADM 是两个对象。

- `ArLive2Pusher`（推流）、`ArLive2Player`（播放）**都不自己起线程跑循环**，而是通过 `RtcTick::RegisteRtcTick` 挂到 `ArLive2Engine::Run()` 的固定节拍循环上。先看引擎的主循环，每次迭代都会先调用 `MThreadTick::DoProcess()`：

```cpp
// ArLiveLite/ArLive2Engine.cpp:268
void ArLive2Engine::Run()
{
	while (b_running_)
	{
		MThreadTick::DoProcess();
		...
		rtc::Thread::ProcessMessages(1);
		rtc::Thread::SleepMs(1);
	}
}
```

  `DoProcess()` 本体是一个 `map<void*, RtcTick*>` 的轮询器，还带了"先标记 unAttach、下一轮才真正摘除"的延迟移除逻辑，避免在遍历过程中修改 map：

```cpp
// ArLiveLite/RtcTick.h:36
class MThreadTick
{
public:
	void RegisteRtcTick(void* ptr, RtcTick* rtcTick);
	void UnRegisteRtcTick(void* ptr);
	void UnAttachRtcTick(void* ptr);
	void DoProcess();
private:
	typedef std::map<void*, RtcTick*> MapRtcTick;
	rtc::RecursiveCriticalSection cs_rtc_tick_;
	MapRtcTick		map_rtc_tick_;
};
```

```cpp
// ArLiveLite/RtcTick.cpp:38
void MThreadTick::DoProcess()
{
	std::list<RtcTick*> lstUnAttach;
	{
		rtc::CritScope l(&cs_rtc_tick_);
		MapRtcTick::iterator iter = map_rtc_tick_.begin();
		while (iter != map_rtc_tick_.end()) {
			if (iter->second->unAttach) {
				lstUnAttach.push_back(iter->second);
				iter = map_rtc_tick_.erase(iter);
			}
			else {
				iter->second->OnTick();   // 逐个驱动挂载的 pusher/player
				iter++;
			}
		}
	}
	std::list<RtcTick*>::iterator itor = lstUnAttach.begin();
	while (itor != lstUnAttach.end()) {
		(*itor)->OnTickUnAttach();
		itor++;
	}
}
```

  播放端会话生命周期里能直接看到"挂载"和"摘除"的调用点：

```cpp
// ArLiveLite/ArLive2Player.cpp:82 (startPlay 节选)
ar_engine_->RegisteRtcTick(this, this);
ar_engine_->AttachAudSpeaker(this);
```

```cpp
// ArLiveLite/ArLive2Player.cpp:104 (stopPlay 节选)
ar_engine_->UnRegisteRtcTick(this);
ar_engine_->DetachAudSpeaker(this);
```

- 音频设备是**全局单例路径**：`InitAudDevice()` 里创建的是**唯一一个**真实的 `webrtc::AudioDeviceModule`（`kPlatformDefaultAudio`，采集/播放硬件），跟上面喂给 `PeerConnectionFactory` 的 `kDummyAudio` 占位 ADM 是两个不同对象：

```cpp
// ArLiveLite/ArLive2Engine.cpp:372
void ArLive2Engine::InitAudDevice()
{
	RTC_CHECK(audio_device_ptr_ == NULL);
	...
	audio_device_ptr_ = webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kPlatformDefaultAudio, task_queue_factory_.get());
	audio_device_ptr_->Init();
	...
	audio_device_ptr_->RegisterAudioCallback(this);   // this = ArLive2Engine，实现了 webrtc::AudioTransport
}
```

  多路 pusher/player 通过引用计数的 map 共享同一份硬件采集通道——`AttachAudCapture` 只有在 map 从 0 变成非 0 时才真正调用 `StartRecording()`：

```cpp
// ArLiveLite/ArLive2Engine.cpp:462
void ArLive2Engine::AttachAudCapture(AudDevCaptureEvent* pEvent)
{
	...
	bool needStartCaptuer = false;
	{
		rtc::CritScope l(&cs_aud_capture_);
		if (map_aud_dev_capture_.find(pEvent) == map_aud_dev_capture_.end()) {
			if (map_aud_dev_capture_.size() == 0) {
				needStartCaptuer = true;   // 只有第一个订阅者才真正拉起硬件采集
			}
			map_aud_dev_capture_[pEvent] = pEvent;
		}
	}
	if (needStartCaptuer) {
		if (!audio_device_ptr_->Recording()) {
			audio_device_ptr_->InitRecording();
			...
		}
	}
}
```

  `DetachAudCapture` 是对称逻辑：map 减到 0 才真正 `StopRecording()`。`AttachAudSpeaker`/`DetachAudSpeaker` 用的是另一张 map（`map_aud_dev_speaker_`），机制相同。

- `V_H264Encoder`/`V_H264Decoder` 各自是**独立的 `rtc::Thread`**，与采集/播放线程解耦：

```cpp
// ArLiveLite/codec/AvCodec.h:81
class V_H264Encoder : public rtc::Thread, public EncodedImageCallback
{
public:
	...
	void Encode(const webrtc::VideoFrame& frame);   // 采集线程调用：只管入队
	virtual void Run();                              // 编码线程自己的循环：按节拍出队编码

protected:
	void AddToFrameList(webrtc::VideoFrame& frame);  // 内部帧队列
private:
	rtc::RecursiveCriticalSection buffer_critsect_;
	std::unique_ptr<VideoRenderFrames> render_buffers_;  // 队列实体
};
```

  `Encode()` 只做拷帧/转正（处理旋转）+ `AddToFrameList` 入队，立刻返回；真正调用底层 `webrtc::VideoEncoder` 的地方在编码线程自己的 `Run()` 里，通过 `FrameToRender()` 从队列取帧：

```cpp
// ArLiveLite/codec/AvCodec.cc:311
void V_H264Encoder::Encode(const webrtc::VideoFrame& frame)
{
	if (!running_) return;
	if (frame.video_frame_buffer()->GetI420() != NULL && frame.rotation() == webrtc::kVideoRotation_0) {
		webrtc::VideoFrame copy_frame(frame);
		copy_frame.set_timestamp_us(rtc::TimeMicros());
		AddToFrameList(copy_frame);       // 采集线程在这里就返回了
	}
	...
}
```

```cpp
// ArLiveLite/codec/AvCodec.cc:428 (Run 节选)
void V_H264Encoder::Run()
{
	while (running_)
	{
		absl::optional<webrtc::VideoFrame> frame_to_render;
		{
			rtc::CritScope cs(&buffer_critsect_);
			frame_to_render = render_buffers_->FrameToRender();   // 编码线程自己按节拍出队
		}
		if (frame_to_render) {
			...
			if (encoder_)
				/* 真正调用 webrtc::VideoEncoder::Encode(...) 在这之后 */;
		}
	}
}
```

## 4. 三条主数据流

**推流（视频）**：
```
Camera(JNI VideoCameraCapturer) / 自定义帧
  → ArLive2Pusher::OnFrame → MgrRender 本地预览
  → (b_live_pushed_ 且 !b_video_paused_ 且 b_client_connected_ 才继续) → V_H264Encoder::Encode → 内部帧队列
  → 编码线程 Run() → webrtc::VideoEncoder::Encode（软编码 H264Encoder 或外部硬编码器工厂）
  → OnEncodedImage 回调 → ArLive2Pusher::OnEncodeDataCallback（关键帧则插入 SEI）
  → ARFFPusher::setVideoData（关键帧门控）→ ArFFWriter::SetVideoEncData
  → FFmpeg av_interleaved_write_frame → FFmpeg 内置 rtmp: avio 协议 → TCP
```

`OnFrame` 的门控条件实测有三层（比"仅 b_client_connected_"更严格，本次核对后订正）——本地预览是**无条件**渲染的，编码才受推流状态门控：

```cpp
// ArLiveLite/ArLive2Pusher.cpp:834
void ArLive2Pusher::OnFrame(const webrtc::VideoFrame& frame)
{
	...
	ar_engine_->GetMgrRender().DoRenderFrame(str_local_push_id_.c_str(), frame);  // 本地预览：无条件渲染

	if (b_live_pushed_) {                                    // 第一层：调用过 startPush
		if (!b_video_paused_ && b_client_connected_) {       // 第二层+第三层：没暂停 且 RTMP 已连上
			webrtc::MutexLock l(&cs_h264_encoder_);
			if (h264_encoder_ != NULL) {
				h264_encoder_->Encode(frame);
			}
		}
		...
	}
}
```

`b_client_connected_` 由推流状态机回调驱动，不是在 `OnFrame` 里自己判断连接状态：

```cpp
// ArLiveLite/ArLive2Pusher.cpp:909
void ArLive2Pusher::onPushStatusUpdate(ArLivePushStatus state, const char* msg, void* extraInfo)
{
	if (state == ArLivePushStatus::ArLivePushStatusConnectSuccess) {
		b_client_connected_ = true;
	}
	else {
		b_client_connected_ = false;
	}
	...
}
```

关键帧插 SEI 的逻辑在 `OnEncodeDataCallback` 里，取的是一个先入先出的 SEI 消息队列（`lst_sei_msg_`），只有关键帧才会消费队首消息并调用 `h264_insert_sei` 拼一份新的、更长的帧数据：

```cpp
// ArLiveLite/ArLive2Pusher.cpp:934
void ArLive2Pusher::OnEncodeDataCallback(bool audio, bool bKeyFrame, const uint8_t *pData, uint32_t nLen, uint32_t ts)
{
	...
	if (bKeyFrame) {
		SeiMsg* seiMsg = NULL;
		if (lst_sei_msg_.size() > 0) {
			seiMsg = lst_sei_msg_.front();
			lst_sei_msg_.pop_front();
		}
		if (seiMsg != NULL) {
			int nKeySize = nLen + seiMsg->nLen + 16;
			char* pKeyData = new char[nKeySize];
			int nKeyLen = h264_insert_sei(pKeyData, (char*)pData, nLen, seiMsg->pMsg, seiMsg->nLen, seiMsg->ePayloadType);
			ar_pusher_->setVideoData((char*)pKeyData, nKeyLen, bKeyFrame, ts);
			delete[] pKeyData;
			delete seiMsg;
			return;
		}
	}
	ar_pusher_->setVideoData((char*)pData, nLen, bKeyFrame, ts);   // 无待发 SEI 消息时的普通路径
}
```

**推流（音频）**：
```
麦克风 PCM → ArLive2Engine::RecordedDataIsAvailable(WebRTC ADM 回调)
  → ArLive2Pusher::RecordedDataIsAvailable（10ms 帧切片）
  → A_AACEncoder::Encode → aac_encoder_encode_frame 内部 PCM 缓冲区攒够一帧（faacEncOpen 协商出的 nInputSamples，AAC-LC 常见为 1024 samples）才真正调用 faacEncEncode
  → OnEncodeDataCallback(audio=true) → ARFFPusher::setAudioData → ArFFWriter::SetAudioEncData → FFmpeg mux
```

10ms 切片是显式按 `samplesPerSec / 100` 算出来的步长，循环喂给编码器：

```cpp
// ArLiveLite/ArLive2Pusher.cpp:792
void ArLive2Pusher::RecordedDataIsAvailable(const void* audioSamples, const size_t nSamples,
	const size_t nBytesPerSample, const size_t nChannels, const uint32_t samplesPerSec, const uint32_t totalDelayMS)
{
	...
	if (b_live_pushed_) {
		int nAud10msLen = (samplesPerSec / 100);     // 10ms 对应的采样点数
		int nAudUsed = 0;
		while (nAudUsed + nAud10msLen <= nSamples) {
			const void* ptr = (char*)audioSamples + nAudUsed*nChannels*sizeof(short);
			...
			aac_encoder_->Encode(ptr, nSamples, nBytesPerSample, nChannels, samplesPerSec, totalDelayMS);
			nAudUsed += nAud10msLen;
		}
	}
}
```

"攒够一帧才真正编码"发生在更底层的 C 接口里，不是 `A_AACEncoder` 自己攒的：`aac_encoder_encode_frame` 把每次传入的 10ms 小块拷进内部 `pPCM` 缓冲区，只有累计长度达到 `nPcmSize`（由 `faacEncOpen` 协商出的帧长决定）才调用一次 `faacEncEncode`，多余部分挪到下一轮继续攒：

```cpp
// ArLiveLite/codec/aacencode.cc:105
int aac_encoder_encode_frame(void*pHandle, unsigned char* inbuf, unsigned int inlen, unsigned char* outbuf, unsigned int* outlen)
{
	AacENC* pEnc = (AacENC*)pHandle;
	if (pEnc->nPcmALen + inlen < pEnc->nPcmSize) {
		memcpy(pEnc->pPCM + pEnc->nPcmALen, inbuf, inlen);
		pEnc->nPcmALen += inlen;
		return 0;                                     // 没攒够，先不编码
	}
	else {
		memcpy(pEnc->pPCM + pEnc->nPcmALen, inbuf, pEnc->nPcmSize - pEnc->nPcmALen);
		int nRet = faacEncEncode(pEnc->hEncoder, (int*)pEnc->pPCM, pEnc->nInputSamples, pEnc->pOutput, pEnc->nMaxOutputBytes);
		...
		// 把这次多出来、下一帧才用得上的尾巴挪到缓冲区开头
		memcpy(pEnc->pPCM, inbuf + (pEnc->nPcmSize - pEnc->nPcmALen), inlen - (pEnc->nPcmSize - pEnc->nPcmALen));
		pEnc->nPcmALen = inlen - (pEnc->nPcmSize - pEnc->nPcmALen);
	}
}
```

**拉流/播放**：
```
avformat_open_input（FFmpeg 自动探测 RTMP/RTSP/HTTP-FLV/本地文件协议）
  → ARFFPlayer 独立读线程 → av_read_frame
  → FFBuffer 第一级队列（网络到达顺序）→ DoTick() 按 DTS 节拍搬入第二级"待解码"队列
  → ARFFPlayer::RunOnce（引擎 tick 驱动）→ avcodec_send_packet/receive_frame
    → 音频：swr_convert 重采样 → PlayBuffer::PlayAudioData
    → 视频：V_H264Decoder / 或直接 avcodec 解码 → PlayBuffer::PlayVideoData
  → PlayBuffer 第三级缓冲，按挂钟时间 + pts 决定渲染/播放节奏（音频优先，视频跟随/丢帧）
```

"引擎 tick 驱动"不是比喻——`ArLive2Player` 本身就是一个 `RtcTick`，`OnTick()` 直接调用 `ARFFPlayer::RunOnce()`：

```cpp
// ArLiveLite/ArLive2Player.cpp:304
void ArLive2Player::OnTick()
{
	if (ar_player_ != NULL) {
		ar_player_->RunOnce();
	}
	PlayBuffer::DoVidRender(b_video_paused_);
}
```

`RunOnce()` 的三步正好对应"第一级→第二级→解码"：先 `DoTick()` 把按 DTS 排好的包从第一级队列搬进待解码队列，再分别拉音频/视频出来解码，直到播放端回调说"不需要更多数据"为止：

```cpp
// ArLiveLite/player/ARFFPlayer.cpp:443
void ARFFPlayer::RunOnce()
{
	FFBuffer::DoTick();
	while (callback_.OnArPlyNeedMoreAudioData(this)) {
		if (!FFBuffer::DoDecodeAudio()) break;
	}
	while (callback_.OnArPlyNeedMoreVideoData(this)) {
		if (!FFBuffer::DoDecodeVideo(callback_.OnArPlyAppIsBackground(this))) break;
	}
}
```

音频解码走的确实是新版 `avcodec_send_packet`/`avcodec_receive_frame` API（旧的 `avcodec_decode_audio4` 调用被 `#if 0` 掉了，留作历史痕迹），解码完立刻 `swr_convert` 重采样成播放端要的采样率/声道数：

```cpp
// ArLiveLite/player/ARFFPlayer.cpp:593 (OnBufferDecodeAudioData 节选)
int ret = avcodec_send_packet(audio_dec_ctx_, &pkt);
if (ret >= 0) {
	ret = avcodec_receive_frame(audio_dec_ctx_, avframe_);
	...
	int samples = swr_convert(audio_convert_ctx_, &p_resamp_buffer_, n_resmap_size_,
		(const uint8_t **)avframe_->data, avframe_->nb_samples);
}
```

## 5. 关键架构事实（容易先入为主搞错的地方）

1. **WebRTC 在这个项目里不做信令、不做传输**，只是被当作"编解码器 + 线程库 + YUV 工具箱"使用。`ArLiveLite/include/webrtc/pc`、`p2p` 目录只有头文件，且 CMakeLists.txt 的源文件清单里也没有任何一个 `pc/`、`p2p/` 下的 .cc——不参与编译（见第 2 节引用的清单）。
2. **RTMP 推拉流走的是 FFmpeg 的 avformat/avio**，不是仓库里另一套自研的 `ArLiveLite/rtmp/`（librtmp + libflv，纯 C 实现，未编入 Android 构建）。也不是仓库里同样存在的 `ArNetClient`/`ArNetTcpClient`（自研 TCP 传输层——虽然 `ArNetClient.cpp`/`ArNetTcpClient.cpp` 确实在 CMakeLists.txt 的编译清单里，但全仓库搜索确认没有任何调用点，是"编了但没人用"的孤儿代码）。
3. **平台差异被封装在 `PlatformImpl.cpp` 的条件编译分发层**：Android 分支全部转发到 `AndroidDeviceManager::Inst()`，这是"公共代码只认基类接口，平台差异塞进一个 .cpp"的桥接模式。以视频采集的创建/启停为例，每个平台函数体都是清一色的 `#ifdef` 分支+转发：

```cpp
// ArLiveLite/PlatformImpl.cpp:13
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

bool startPlatformVideoCapture(void*ptrCap)
{
	if (ptrCap == NULL) return false;
#if (defined(WEBRTC_WIN) || defined(WEBRTC_MAC)) && !defined(WEBRTC_IOS)
	VcmCapturer* vCap = (VcmCapturer*)ptrCap;
	return vCap->Start();
#elif (defined(WEBRTC_ANDROID))
	return AndroidDeviceManager::Inst().startCapture();   // Android 分支：纯转发，零业务逻辑
#elif defined(WEBRTC_IOS)
    ObjcVCMCapturer *capturer = (ObjcVCMCapturer *)ptrCap;
    capturer->StartCapture();
    return true;
#endif
}
```

   同文件里 `createPlatformVideoCapture`/`stopPlatformVideoCapture`/`destoryPlatformVideoCapture`/`switchPlatformVideoCapture` 全部是同一种形状——这是判断"某个跨平台行为在 Android 上到底是谁实现的"的最快入口：直接来这个文件搜函数名，Android 分支永远只有一行转发。
4. **Android 路径大概率始终走 WebRTC 软编码（openh264）**，因为硬件编码器工厂需要上层显式调用 `setExVideoEncoderFactory` 注入，而 `ArLive2Engine::createArLivePusher` 的 Android 分支没有像 iOS 分支那样自动注入。

## 6. 延伸阅读

- 具体功能清单 → [ArLiveLite具体功能.md](./ArLiveLite具体功能.md)
- 用到的 C++/JNI 技术点 → [ArLiveLite用到的C++技术点.md](./ArLiveLite用到的C++技术点.md)
- 已发现的坑/bug/死代码 → [ArLiveLite 难点和复杂.md](./ArLiveLite%20难点和复杂.md)
- 建议的学习顺序 → [ArLiveLite学习计划.md](./ArLiveLite学习计划.md)
