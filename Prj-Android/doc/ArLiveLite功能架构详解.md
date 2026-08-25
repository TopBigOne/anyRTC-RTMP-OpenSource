# ArLiveLite 功能架构详解

> 分析范围：`ArLiveLite/`（Android/iOS/Windows 三端共用的跨平台 C++ 引擎核心），基于对全量源码的逐行阅读，并与 Android CMakeLists、iOS Xcode 工程、Windows vcxproj 三份构建脚本及 git 历史交叉核对整理而成。
>
> 与已有的 [ArLiveLite技术架构.md](./ArLiveLite技术架构.md)、[ArLiveLite具体功能.md](./ArLiveLite具体功能.md) 视角互补：那两篇更偏"分层结构 + 逐条功能配代码片段"，本文更偏"数据流全貌 + 历史演进 + 技术债/精心设计清单"，并补充了此前未记录的几处发现（`ARRtmpPusher` 删除历史、`rtmp/librtmp` 来源考古、iOS 采集类型强转疑点等）。
>
> 配套架构图（推流/拉流数据流、线程模型对比、三端构建矩阵）见发布的 Artifact：<https://claude.ai/code/artifact/b68c73b7-e28f-406c-b51f-5e68b9717086>

## 0. 总体定位

一句话概括：**ArLiveLite 不是一个纯自研引擎，而是"WebRTC 采集/编码接口 + FFmpeg 封装/传输 + 三端原生渲染胶水 + 自研业务逻辑"拼装而成的直播 SDK 核心**。四种"来源"的代码交织在一起：

- **WebRTC 基础设施**：`rtc::Thread` 消息循环、`AudioDeviceModule`、`PeerConnectionFactory`、`VideoTrackSourceInterface`、`webrtc::VideoEncoder/VideoDecoder` 接口、`rtc_base` 的锁/日志/socket 封装。这套东西撑起了引擎的线程模型和采集/编码的"骨架"，但**推拉流走的完全不是 WebRTC 的 RTP/SRTP 传输栈**。
- **FFmpeg**：真正的推流封装（FLV mux）、真正的拉流解复用+解码（RTMP/FLV demux + avcodec 软解），是数据真正"上网"和"下网"的引擎。
- **自研业务胶水**：`ArLive2Engine/Pusher/Player`、两级缓冲 `FFBuffer`/`PlayBuffer`、SEI 消息、AAC 编码封装（基于第三方 FAAC 库）、渲染分发 `MgrRender`。
- **三端原生适配**：Android 的 JNI 桥接、iOS 的 Objective-C++ 桥接到 Metal、Windows 的 D3D 渲染 + VCM 采集。

这四层不是均匀分布的，每一层都有自己的技术债和"精心设计"，下面逐层展开。

## 1. 引擎核心：ArLive2Engine

### 1.1 单例与生命周期

`ArLive2Engine` 是一个**模块级裸指针单例**，不是严格的 Meyer's Singleton：

```cpp
static ArLive2Engine* gInstance = NULL;
AR::IArLive2Engine* createArLive2Engine() {
    if (gInstance == NULL) gInstance = new ArLive2Engine();
    return gInstance;
}
```
（`ArLive2Engine.cpp:47-55`）无锁、无 `call_once`，但因为引擎创建通常发生在 App 主线程初始化阶段，实践中竞态窗口很小。析构时才清空 `gInstance`（`ArLive2Engine.cpp:70-74`），意味着这是"可重建的单例"——`release()` 之后可以再 `createArLive2Engine()` 拿到一个全新实例。

`initialize()` 的关键设计（`ArLive2Engine.cpp:77-92`）：先 `rtc::Thread::Start()` 启动自身线程，**再**用 `Invoke<void>` 把 `InitAudDevice()`、`InitPeerConnection()` 分两步同步切进这条线程去执行，而不是在调用者（App 主线程）里直接跑。这是因为 WebRTC 的 `AudioDeviceModule`、`PeerConnectionFactory` 要求"创建时的线程"就是后续的工作线程——`CreatePeerConnectionFactory` 会把 `this`（`ArLive2Engine` 自己）同时当作 `worker_thread` 传进去。

`InitAudDevice()`（`ArLive2Engine.cpp:372-406`）创建**真实**的 `AudioDeviceModule`（`kPlatformDefaultAudio`，对接系统麦克风/扬声器），并**显式关闭**内建 AEC/AGC/NS：

```cpp
if (audio_device_ptr_->BuiltInAECIsAvailable()) audio_device_ptr_->EnableBuiltInAEC(false);
if (audio_device_ptr_->BuiltInAGCIsAvailable()) audio_device_ptr_->EnableBuiltInAGC(false);
if (audio_device_ptr_->BuiltInNSIsAvailable())  audio_device_ptr_->EnableBuiltInNS(false);
```
说明 ArLiveLite 选择不依赖系统 3A 算法（这在纯推流/播放场景下常见，因为 AEC 主要为双向通话服务）。

`InitPeerConnection()`（`ArLive2Engine.cpp:418-455`）里有一个非常隐蔽的设计：**创建了第二个 ADM**——`rtc_adm_ = webrtc::AudioDeviceModule::Create(kDummyAudio, ...)`，是个哑设备，只用来满足 `PeerConnectionFactory` 构造时"必须传一个 ADM"的形式要求；真正对接硬件的是 `audio_device_ptr_`。二者数据靠 `RecordedDataIsAvailable`/`NeedMorePlayData` 手工搬运打通（`ArLive2Engine.cpp:321-369`）。iOS 分支用 `RTCVideoEncoderFactoryH264`（VideoToolbox 硬编），其它平台用 WebRTC 内建软编码器工厂；**只有 `worker_thread` 被复用为引擎自身**，`network_thread`/`signaling_thread` 传 `nullptr` 由 WebRTC 内部另起线程。这里还藏着一个**顺序依赖坑**：源码注释明确写着"必须先建好 `peer_connection_factory_`，`rtc_adm_` 的播放/录音才能正常启动"，但代码里没有任何断言保护这个顺序，纯靠 `initialize()` 里两次 `Invoke` 的调用顺序人工保证。

`release()`（`ArLive2Engine.cpp:94-118`）严格按初始化的倒序释放，但对"还有 Pusher/Player 未释放"的情况只有一个空的 `//Warn` 注释、没有真正的告警或阻断逻辑——上层若忘记先释放 Pusher/Player，引擎会照样反初始化，可能留下悬空引用。`releaseArLivePusher`/`releaseArLivePlayer` 里还有一处经典 C++ 反模式：函数体最后 `pusher = NULL;`，这是按值传递的形参，对调用者手里的指针毫无影响（`ArLive2Engine.cpp:178-192`）。

### 1.2 线程模型：`Run()` 是一个 1ms 级忙轮询

`ArLive2Engine` 直接继承 `rtc::Thread`（而不是"持有"一个线程），意味着引擎对象本身就是一条线程。它重写了 `Run()`（`ArLive2Engine.cpp:267-318`）：

```cpp
while (b_running_) {
    MThreadTick::DoProcess();                 // 驱动所有 RtcTick 心跳
    // ...一段10ms定时器"空转"死代码...
    if (n_timer_100ms_ <= now) {               // 每100ms检查一次
        // 音频采集/播放异常则自动 InitRecording/StartRecording 重试
    }
    rtc::Thread::ProcessMessages(1);           // 处理 Invoke/Post 任务队列
#ifdef WIN32
    w32_thread.ProcessMessages(1);             // Windows 额外泵一次 Win32 消息
#endif
    rtc::Thread::SleepMs(1);
}
```

这不是标准 `rtc::Thread` 默认的"纯粹靠消息队列阻塞唤醒"模型，而是一个**近似 1ms 粒度的主动忙轮询**。这个设计取舍很关键：`PeerConnectionFactory` 的 `worker_thread` 就是这条线程，意味着**引擎主循环体的执行时长会直接影响所有 PeerConnection worker 任务（编解码调度等）的及时性**——这是"为什么引擎主循环要保持轻量"这条隐性约束的来源。同时它换来了两个好处：①所有注册了 `RtcTick` 的对象（Pusher/Player 内部状态机）都能以接近毫秒级的频率被驱动；②每 100ms 自动检查并重启异常的音频设备，是一种"自愈"机制。

### 1.3 RtcTick 心跳：一处值得单独表扬的锁设计

`MThreadTick` 维护一个 `std::map<void*, RtcTick*>`，`DoProcess()`（`RtcTick.cpp:38-60`）遍历调用每个对象的 `OnTick()`。它区分了两种反注册：

- `UnRegisteRtcTick`：立即从表里摘除（适合"对象马上要销毁，且能保证没有并发 OnTick"的场景）。
- `UnAttachRtcTick`：只是把该对象的 `unAttach` 标记置真，真正的摘除延迟到下一轮 `DoProcess()`，并且摘除后统一在锁外调用一次 `OnTickUnAttach()` 收尾回调。

这个设计的精巧之处在于**把"持锁遍历/擦除 map"和"调用虚函数收尾"这两件事分开**——遍历时加锁范围很短，`OnTickUnAttach()` 被挪到锁外的二次遍历里执行，避免了在临界区里长时间持锁调用虚函数可能引发的重入/死锁风险。

### 1.4 AudioTransport：音频驱动权在 ADM 线程手里，不在引擎主循环

`ArLive2Engine` 还实现了 `webrtc::AudioTransport` 接口并注册给 `audio_device_ptr_`。这意味着音频采集/播放的驱动权在 **ADM 内部的音频回调线程**（系统级音频线程，如 Android AAudio 回调线程）手里，而不是 `Run()` 主循环：

- `RecordedDataIsAvailable`（`ArLive2Engine.cpp:321-338`）：每次麦克风采到一帧 PCM 就广播给所有 `AudDevCaptureEvent` 订阅者（Pusher 的音频编码通道），同时把数据塞进 Dummy ADM。
- `NeedMorePlayData`（`ArLive2Engine.cpp:340-369`）：ADM 反向索要待播放 PCM 时，遍历 `AudDevSpeakerEvent` 混音；但从 Dummy ADM 取出的"WebRTC 侧要播放的数据"从未被真正混入最终输出——这是一处**半成品代码**，暗示"房间模式"下远端音频播放链路可能存在功能缺口。

`AttachAudCapture`/`AttachAudSpeaker` 等四个方法都用了同一个"自动线程亲和"模式：任何线程调用都先判断自己是否在引擎线程上，不是则 `Invoke` 回引擎线程重新执行一次，从而免去对 WebRTC 对象额外加锁。同时它们都遵循"**引用计数式**"启动：第一个订阅者才真正触发硬件采集开启，最后一个退出才真正停止——多个 Pusher 可以共享同一份麦克风硬件。

### 1.5 IArLive2Engine 对外接口 & 类型/事件体系

对外核心方法就七八个：`initialize`/`release`/`createArLivePusher`/`createArLivePlayer`/`releaseArLivePusher`/`releaseArLivePlayer`/`setAppInBackground`。值得注意 `setAppInBackground` 只广播给所有 Player，不广播给 Pusher（`ArLive2Engine.cpp:210-221`），是个不对称设计。每个 Pusher/Player 创建时分配一个 16 位随机字符串 ID，存入 `map_arlive2_pusher_`/`map_arlive2_player_`，支持同一进程内**多路推流/多路播放并存**。iOS 分支额外给每个 Pusher 单独注入一份 `RTCVideoEncoderFactoryH264`（而不是复用引擎级工厂），暗示视频编码路径存在"引擎全局一份+每个 Pusher 独立一份"的双重工厂结构。

`ArLiveSDKTypeDef.h`/`ArLiveSDKEventDef.h`/`ArAVCode.h` 三个文件构成了对外回调协议的"三层结构"：`ArAVCode.h` 定义**原始**枚举值（`ArAVError`/`ArAVWarning`/`ArAVEvent`），`ArLiveSDKEventDef.h` 给它们套一层 `PUSH_EVT_*`/`PLAY_ERR_*` 风格的"外部命名"别名。而 `ArAVCode.h` 里的枚举**注释里大量残留腾讯 TRTC 的专有名词**——`TRTCCloudDelegate##onEnterRoom()`、"腾讯云账号是否欠费"、`official.opensso.tencent-cloud.com:443`、甚至一行"QQ 3268519604"的联系方式——这几乎是直接从腾讯云 TRTC SDK 头文件移植改造而来的铁证，很多错误码字段对 anyRTC 自己的架构而言是"僵尸常量"。`ArLiveSDKTypeDef.h` 定义的 `onNetStatus` 字段名（CPU 占用/分辨率/码率/丢帧/音画同步偏差等）风格也与腾讯云直播 SDK 高度一致。个别宏还直接硬编码数字而不引用 `ArAVCode.h` 的枚举（如 `PUSH_ERR_AUDIO_SYSTEM_NOT_WORK=-1308`，恰好与 `ERR_SCREEN_CAPTURE_START_FAIL` 撞车），是历史维护留下的技术债。

### 1.6 PlatformImpl：三端采集能力的分发中枢

`PlatformImpl.cpp` 用 `#ifdef WEBRTC_WIN/WEBRTC_ANDROID/WEBRTC_IOS` 把"创建视频源"、"创建/启停采集器"、"美颜开关"、"前后摄像头切换"这几件事分派到三端各自实现：

- `createPlatformVideoSouce()`（注意拼写少了个 r，是历史笔误）：Windows 用 `WinVideoTrackSource`，Android 委托 `AndroidDeviceManager::Inst()` 单例，iOS 直接用 WebRTC 官方 `ObjCVideoTrackSource`——**iOS 是三个分支里唯一"零自定义、纯复用官方实现"的**。这个函数**没有 `WEBRTC_MAC` 分支**，而下面的采集器创建函数却明确认可 Mac 桌面平台，是条件编译维护"改了一半"的痕迹。
- 美颜/前后摄像头切换只在 `TARGET_PLATFORM_PHONE` 宏下才存在，且**同一个"美颜开关"在 Android 上是"设备管理器全局设置"、在 iOS 上是"采集器对象自身设置"**，挂载点完全不同。前后摄像头切换上，Android 接受"显式切到哪一个"的参数，iOS 的 `SwitchCapture()` 却不接受该参数、只是"翻转当前状态"——这是一个潜在的**跨平台行为不一致**：如果调用方假设两端语义相同，iOS 上可能因为已处于目标状态而被再翻转一次。

### 1.7 MgrRender：一个干净的"渲染器注册表+分发器"

`MgrRender` 本身极薄——`std::map<std::string, webrtc::VideoRenderer*>`+一把递归锁，四个方法：`SetRender`（注册/替换，旧值自动 delete，即 MgrRender 拥有渲染器对象的所有权）、`SetRotation`/`SetFillMode`（**均为空实现**，接口占位但未落地）、`DoRenderFrame`（按 ID 查表分发帧）。**本地预览和远端拉流画面走的是同一套 `MgrRender` 逻辑**，区别只是 `renderId` 字符串不同（`str_local_push_id_` vs `str_local_play_id_`）——这是复用做得比较漂亮的一处。真正的平台渲染实现通过 `webrtc::VideoRenderer::CreatePlatformRenderer(canvas.view, 640, 480)` 这个跨平台静态工厂方法接入，**宽高被硬编码成 640x480**，配合 `SetRotation`/`SetFillMode` 的空实现，说明这块渲染绑定目前是一个功能不完整的骨架。

## 2. 推流链路：ArLive2Pusher 全链路

### 2.1 三段式混合架构

推流是"**WebRTC 采集/编码接口 + FFmpeg avformat 封装**"的混合体：

1. **采集层**：走 WebRTC 的 `VideoTrackSourceInterface`/`VideoSinkInterface<VideoFrame>` 体系。
2. **编码层**：视频走 `webrtc::V_H264Encoder`（内部可插外部硬编工厂，否则退化到 WebRTC 自带软编）；音频走自研 FAAC 封装 `webrtc::A_AACEncoder`。两者都**不经过 WebRTC 的 RTP/RTCP/SRTP 栈**。
3. **封装发送层**：`ARFFPusher` 是控制面壳子，真正干活的是 `ArFFWriter`，直接调 FFmpeg `avformat_alloc_output_context2`/`av_interleaved_write_frame`，用 `"flv"` muxer 封装成 RTMP 流发出去。**完全不用 WebRTC 的 RTP 传输栈**，是一条自建的推流通道。

`ArLive2Pusher` 本身是个典型的"上帝对象"：同时实现 `IArLivePusher`、`RtcTick`、`AudDevCaptureEvent`、`VideoSinkInterface<VideoFrame>`、`ArLivePusherObserver`、`AVCodecCallback` 六个身份，把采集、编码、SEI 插入、推流状态、统计、虚拟摄像头全部揉在一个类里。

### 2.2 完整数据流（逐跳）

**视频链路**：

```
摄像头硬件 → VcmCapturer::OnFrame(采集线程)
  → WinVideoTrackSource::FrameCaptured → AdaptedVideoTrackSource::OnFrame(帧率/分辨率自适应+分发)
  → ArLive2Pusher::OnFrame(仍在采集线程!非引擎线程)
      ├→ MgrRender.DoRenderFrame(本地预览,与推流并行)
      └→ 若 b_client_connected_==true: h264_encoder_->Encode(frame)
  → V_H264Encoder::Encode(采集线程,只是把帧塞进队列)
  → V_H264Encoder::Run()(编码器自己的独立线程,真正编码发生在这)
  → V_H264Encoder::OnEncodedImage(webrtc回调,转发裸H264 NALU)
  → ArLive2Pusher::OnEncodeDataCallback(关键帧则插SEI)
  → ARFFPusher::setVideoData(首关键帧门控)
  → ArFFWriter::SetVideoEncData → av_interleaved_write_frame
  → FFmpeg flv muxer → RTMP协议层 → 网络
```

**音频链路**：

```
麦克风 → ArLive2Engine广播 → ArLive2Pusher::RecordedDataIsAvailable(采集线程,按10ms切片)
  → A_AACEncoder::Encode(同步内联,不开线程!)
  → OnEncodeDataCallback → ARFFPusher::setAudioData → ArFFWriter::SetAudioEncData → 网络
```

**关键的不对称性**：视频编码是异步、有独立线程的；音频编码是同步内联的——耗时短的音频省去了线程切换和队列管理的复杂度。而且**只有视频编码触发依赖 `b_client_connected_`（RTMP 已握手成功）**，音频编码只依赖 `b_live_pushed_`（推流会话已开始）——断线期间视频完全不编码省 CPU，音频仍在编码只是被下游按连接状态丢弃，是两层不同粒度的"节流"。

### 2.3 ARFFPusher / ArFFWriter 的分工

`ARFFPusher` 是业务壳：负责线程封送（`main_thread_->Invoke`）、URL 协议分发（`rtmp://` 走 flv，`rtsp://` 走 rtsp 且强制音频类型为 G711A——暗示这套壳子是从更通用的多协议推流场景裁剪而来）、首个关键帧门控（等到第一个视频关键帧才放行音视频数据写入，防止推流起始就是 P 帧导致花屏）、状态回调翻译（`ArWriterState`→对外 `ArLivePushStatus`）。

`ArFFWriter` 是真正和 FFmpeg 打交道的执行体。它的"影子编码器"手法是一处**巧妙但脆弱的技巧**：建连时临时 `avcodec_open2` 打开一个真实的 FFmpeg H264/AAC 编码器，只为了取出它生成的 `extradata`（SPS/PPS、AudioSpecificConfig）写进 `AVStream` 元数据，随即释放，不参与真实编码——真正编码画面/声音的是 `V_H264Encoder`/`A_AACEncoder`。如果"影子编码器"打开失败，会**回退到一段硬编码的 SPS/PPS 字节数组**，注释直接引用了 B 站博主博客链接，与实际推流分辨率无关，是"来路不明但据说管用"的历史偏方。这个手法最脆弱的地方在于：**依赖 FFmpeg 内置 AAC 编码器和自研 FAAC 编码器对同一采样率/声道数生成完全一致的 AudioSpecificConfig 字节**，两条代码路径完全独立，没有运行时一致性校验。

数据写入上还有一处值得警惕的细节：`SetVideoEncData`（视频，在编码器线程调用）和 `SetAudioEncData`（音频，在采集线程调用）**分别在两条不同线程上调用同一个 `AVFormatContext`** 的 `av_interleaved_write_frame`，`ARFFPusher` 的锁只保护指针生命周期、不保护并发写帧本身——这是潜在的跨线程竞态风险。此外 `SetVideoEncData` 里"先 `av_packet_rescale_ts` 换算 time_base，又立即用原始毫秒值覆盖"是多格式通用代码没有为 FLV 场景清理干净留下的冗余逻辑。

断线重连策略是"推倒重来"：固定 1500ms 延迟、**无指数退避、无重试上限**（写死不可配置），每次重试都完整重新 `Connect()`（重新分配 AVFormatContext、重新握手）。

### 2.4 H264SeiPack：自定义消息与关键帧的联动

SEI 插入遵循标准 Annex-B 格式，新 SEI 永远插在 SPS/PPS 之后、已有 SEI 之后、IDR 之前。**只有关键帧才有机会携带自定义 SEI**，每次只消费队列头部一条消息。因为关键帧默认按 `V_H264Encoder` 内部固定 3 秒间隔强制刷新，`OnTick()` 里有一段专门的"加速关键帧"逻辑：**只要 SEI 队列非空且距上一关键帧超过 250ms，就主动请求关键帧**——这是用增大关键帧频率（进而增大码率开销）换取自定义消息低延迟送达的设计取舍。SEI 缓冲区固定 `+16` 字节余量，但 `sendSeiMessage` 对外接口**没有对 payload 大小做上限校验**，理论上大 payload 存在缓冲区溢出风险。

### 2.5 VcmCapturer 采集线程模型

`VcmCapturer` 有一条独立的"VCM 线程"专门跟底层采集设备打交道（很多平台采集 API 要求在创建它的同一线程上下文操作），并显式 `AllowInvokesToThread` 放开死锁保护——是有意为之的跨线程同步调用。真正的 `OnFrame` 回调发生在设备驱动自己的采集线程上，既非 VCM 线程也非引擎线程。镜像/裁剪等画面处理**全部下放到 `V_H264Encoder::AddToFrameList`**，VcmCapturer 本身只是纯粹的"数据搬运工"。

## 3. 播放链路：ArLive2Player 全链路

### 3.1 类关系：多重继承代替组合的"能力混入"

```
ArLive2Player (对外API层)
   继承: RtcTick / PlayBuffer(第二级播放缓冲) / AudDevSpeakerEvent / ARPlayerEvent
   持有: ARPlayer* ar_player_  (实际是 ARFFPlayer 实例)

ARFFPlayer (拉流具体实现,基于FFmpeg)
   继承: ARPlayer / FFBuffer(第一级网络缓冲) / rtc::Thread(独立解封装线程)
```

`ArLive2Player` 用多重继承"拥有" `PlayBuffer`、`ARFFPlayer` 用多重继承"拥有" `FFBuffer`，让上层类天生具备缓冲能力，同时通过覆写虚函数把事件"泄漏"回自己——省去大量样板代码，但也带来强耦合。

### 3.2 完整数据流

```
avformat_open_input(RTMP URL) → ARFFPlayer::Run()(独立线程,循环av_read_frame)
  → 按流类型分流,pts/dts换算成毫秒 → FFBuffer::RecvVideoData/RecvAudioData(网络抗抖动第一级缓冲)
  → [由引擎OnTick驱动] FFBuffer::DoTick()(时间门控,把该解码的包从recv队列搬进decode队列)
  → [由引擎OnTick驱动] ARFFPlayer::RunOnce() → avcodec_send_packet/avcodec_receive_frame(纯FFmpeg软解)
  → OnBufferDecodeVideoData/AudioData → 回调 ArLive2Player::OnArPlyVideo/OnArPlyAudio
  → PlayBuffer::PlayVideoData(pts有序插入)/PlayAudioData(渲染平滑第二级缓冲)
  → [由引擎OnTick驱动] PlayBuffer::DoVidRender → ar_engine_->GetMgrRender().DoRenderFrame(视频渲染出口)
  → [由ADM播放线程主动拉取] MixAudioData → PlayBuffer::DoAudRender(音频播放出口,10ms节拍)
```

**这里必须纠正一个常见误解**：拉流解码走的是**纯 FFmpeg 软解**（`avcodec_send_packet`/`avcodec_receive_frame`），完全不使用 codec 层的 `webrtc::V_H264Decoder`——两套解码体系并存，但只有一套真正在跑（详见第 4 节）。

### 3.3 两级缓冲的分工：FFBuffer vs PlayBuffer

这是整个播放器最容易混淆、也最值得理解的设计：

| | FFBuffer | PlayBuffer |
|---|---|---|
| 存储 | 压缩包 `AVPacket`（编码前） | PCM/`webrtc::VideoFrameBuffer`（解码后） |
| 归属 | ARFFPlayer（FFmpeg 私有实现细节） | ArLive2Player（通用播放契约层） |
| 职责 | **网络抗抖动**：recv 队列→decode 队列的时间门控，缓冲状态机 `PS_Init/Caching/Playing` | **解码-渲染平滑+AV 排序**：pts 有序插入，音频满则丢半联动丢视频 |
| 量级 | 秒级（最长可到 60 秒） | 百毫秒级（音频 20 帧≈200ms，视频 10 帧） |

为什么要两层：FFBuffer 处理"网络层不确定性"（RTMP 包到达抖动可能是秒级的），需要一个可以从 1 秒动态增长到 60 秒的大缓冲做抗抖动；PlayBuffer 处理"本地解码/渲染层的微观不同步"（FFmpeg 软解是批量的，尤其视频有 B 帧要等 `best_effort_timestamp` 重排，且下游音频设备是精确 10ms 节拍拉取的）。两层解耦还带来一个好处：未来如果要换一个不用 FFmpeg 的播放后端，只需要新写一个 `ARPlayer` 子类去生产 `PcmData`/`VideoData` 喂给同一个 `PlayBuffer` 即可。

**一处只增不减的缺陷**：`FFBuffer` 的起播缓冲阈值 `n_cache_to_play_time_` 每次欠载都 +1000ms 且**代码里找不到任何降低它的路径**，对长时间挂播、偶发网络抖动的直播场景，意味着每次抖动都会让端到端延迟阶梯式增长且永不恢复。

### 3.4 sonic 变速库：被大材小用

`sonic.c` 是一套专为"语速变速不变调"设计的库，但在 ArLiveLite 里**只被用来做音量增益处理**——`GotAudioFrame()` 里只有当 `f_aud_volume_ != 1.0` 才创建 sonic 流，调用 `sonicSetVolume`。而它真正擅长的变速能力（对应外部 API `setSpeed`）完全没接上：`ArLive2Player::setSpeed()` 有完整参数校验并转发，但 `ARFFPlayer::SetSpeed()` 是**空函数体**，`OnBufferGetSpeed()` 硬编码返回 1.0。这既是功能未完工（变速没接上），也是资源浪费（为了调音量而运行整套 sinc 插值变速引擎状态机）。

### 3.5 音视频同步：隐式的"音频主时钟"模型

没有教科书式的"计算 A-V diff、按 diff 丢帧插帧"算法，而是"共享虚拟时钟（锚定优先音频）门控解码 + 独立墙钟节拍渲染 + 音频驱动的联动丢帧"组合方案：

- `FFBuffer::DoTick()` 维护一个共享的解码虚拟时钟，重新锚定时**优先取音频队首包的 dts 作为锚点**（音频没有才退而求其次用视频）。
- 断流检测：队首包 dts 与上次的间隔超过 2 倍 duration 即认为跳变，触发重新锚定——防止时间轴走飞的自愈机制。
- `PlayBuffer::DoVidRender()` 用视频自己的墙钟锚点，"提前到达就等待"，但**没有"迟到就丢帧"的显式逻辑**——真正的丢帧只发生在 `PlayAudioData()` 触发的联动丢弃（按音频丢弃到的 dropPts 位置清理更旧的视频帧）。
- `PlayBuffer::DoAudRender()` 完全没有时间判断，被动响应 ADM 设备回调节奏，有数据就吐、没数据就记日志。

结论：音频链路（锚点优先权+丢弃触发条件都以它为准+被硬件采样时钟驱动）实质上是主导方，视频是跟随音频节奏被动同步/被动丢弃的一方，是隐式的 audio master clock 模型，只是没有显式的 `get_master_clock()` 函数，而是通过两级时间锚定架构间接达成。

## 4. 编解码层：codec/AvCodec.cc & aacencode.cc

### 4.1 V_H264Encoder：硬编优先、软编兜底

编码参数（`h264_.startBitrate = h264_.maxBitrate`）是**恒定码率**而非区间自适应，没有单独设置 GOP（通过软件逻辑每 3 秒强制一次关键帧实现），没有设置 H264 Profile/Level（交由外部决定）。硬件编码器判定路径是两级尝试：

```cpp
if (video_encoder_factory_ != NULL)
    extern_encoder = video_encoder_factory_->CreateVideoEncoder(sdpParam);
if (extern_encoder && extern_encoder->InitEncode(...) == WEBRTC_VIDEO_CODEC_OK)
    encoder_.reset(extern_encoder.release());
else
    encoder_ = webrtc::H264Encoder::Create(...);  // 软编 fallback
```

只有移动端（`TARGET_PLATFORM_PHONE`）才会尝试注入外部硬编工厂，桌面端目前恒定走软件 OpenH264 编码。SDP 层设置了 `packetization-mode=1`，这行代码自带原作者疑问式注释"导致单帧太大?"——说明这个配置本身是否合适，连原作者自己都存疑。

libyuv 在这一层承担镜像/三种缩放策略（Fill 裁剪铺满/Fit 留黑边/Auto 按目标宽高比缩放不留黑边）的全部画面处理，是整条推流链路里唯一集中处理画面的地方。`UpdateBitrate()`（动态调整码率）**没有任何调用方**——当前推流没有基于网络反馈的码率自适应闭环，码率一旦推流开始就恒定不变，对弱网表现不利。

### 4.2 V_H264Decoder：全仓库唯一无调用方的完整实现

设计和 Encoder 对称（硬解优先/软解 fallback、懒创建、关键帧等待、解码失败即重建），但 `video_decoder_factory_` **有存储无注入接口**（没有对应 Encoder 侧的 `SetExVideoEncoderFactory` 那样的 setter），恒定走软件解码。更关键的是——**全仓库检索 `V_H264Decoder`，除了它自己的定义外没有任何文件实例化它**。实际的拉流解码走 `ARFFPlayer.cpp` 的纯 FFmpeg 路径。这可能是历史上（早期基于纯 webrtc 架构）遗留、后来切换为 FFmpeg 播放器架构后未清理的废弃实现。

还有一处更独立的发现：`IArVidCodec.h` 定义的 `IArVidEncoder`/`createArVidEncoder` 是**另一套跟 `AvCodec.h` 平行、互不相关的编码器抽象**（接受 Y/U/V 裸平面数据的"主动喂数据"模式），项目里可能同时存在"webrtc 风格编码器封装"与"更早期/更通用的裸 YUV 编码器接口"两套并行遗产。

### 4.3 音频编码：自研 FAAC 封装

`A_AACEncoder` 底层是 `aac_encoder_open/encode_frame` 这套 C 风格 FAAC 封装，固定用 AAC-LC（`objectType=LOW`），`outputFormat` 固定传 `mp4=true`（裸 AAC 不带 ADTS 头，符合 FLV Audio Tag 规范）。采样率/声道数/码率**不是来自设备采集能力协商，而是产品预设的三档质量档位硬编码值**（Speech 16kHz 单声道 16kbps / Default 48kHz 单声道 50kbps / Music 48kHz 双声道 128kbps）。`aac_encoder_encode_frame` 内部有一个手写的"环形累积再切帧"逻辑，巧妙地解决了"上层固定 10ms 喂数据 vs FAAC 固定帧长（通常 1024 采样点）"两者不对齐的问题。

### 4.4 谁在用这一层

只有 `ArLive2Pusher`（推流侧）直接使用 `V_H264Encoder`/`A_AACEncoder`。`V_H264Decoder` 在全仓库范围没有被任何文件实例化；`ArLive2Engine` 本身不直接引用 `AvCodec.h` 任何类，只是通过持有 Pusher 实例、驱动 `RtcTick` 间接影响编码节奏。

一处真实的代码缺陷：`ArLive2Pusher::exVideo_encoder_factory`（外部硬编码器工厂指针）在构造函数初始化列表里**没有被赋值**，如果外部忘记调用 `setExVideoEncoderFactory`，移动端分支 `if (exVideo_encoder_factory != NULL)` 判断的是一个未定义值，存在极小概率把野指针当工厂用的隐患。

## 5. 网络层与遗留 RTMP/FLV 协议栈

这是六路精读里信息量最大、也最能体现这个项目"演进痕迹"的一块。

### 5.1 ArNetClient / ArNetTcpClient：编译了但零调用的死代码

`INetClient` 定义了 `NOT_CONNECTED→RESOLVING→CONNECTTING→CONNECTED` 状态机接口，`ArNetClient` 用模板方法模式把"业务调用"和"网络实现细节"拆开，`ArNetTcpClient` 是唯一具体实现（基于 WebRTC 的 `rtc::AsyncSocket`，Windows 用 `Win32Socket`、POSIX 用 `socketserver()->CreateAsyncSocket()`）。**但全仓库检索，`createArNetTcpClient()` 这个唯一工厂函数除了自己的声明和定义外没有任何调用点**——真正的推拉流网络收发完全交给 FFmpeg 内部的 avformat/avio。它被 Android/iOS/Windows 三端**都编译**进去了，却没有一处执行到。

### 5.2 url.cpp/hpp：一个孤立的通用 URL 解析器

支持完整 RFC 3986 风格 URI 解析（scheme/user_info/host 含 IPv6/port/path/query/fragment），协议无关、可以解析任意 scheme 的 URL，不是 RTMP 专用。**同样是零调用点**——之前误以为的"使用者"逐一核实后都是变量名恰好包含"Url"子串的假阳性。而且它连"三端一致的死代码"都算不上：**只有 iOS 工程编译了 `url.cpp`，Android 和 Windows 都没有**。

### 5.3 rtmp/librtmp + libflv：一段完整的历史考古

这是本次最有价值的发现。目录规模约 9600 行 C 代码，来源判断（文件命名、目录分层、注释风格）与知名开源项目 **ireader/media-server** 高度吻合，几乎可以确定是"整体拷贝+裁剪"而来。内容覆盖：

- **RTMP 协议**：握手（C0/C1/C2, S0/S1/S2）、Chunk 分片（4 种 Chunk Type）、协议控制消息（Set Chunk Size/Abort/Ack/Window Ack Size/Set Peer Bandwidth 全套）、AMF0 命令编解码、客户端状态机（`rtmp-client.c` 575 行，完整实现 connect→createStream→publish/play 状态流转）。**客户端方向完整成熟**，但**服务端方向不完整**——`rtmp-server.h` 有声明，`rtmp-server.c` 源码在整个 git 历史里从未存在过，只剩一个孤立的 `.o` 目标文件。`aio/` 异步 IO 封装因缺少 `aio-socket.h` 依赖而**无法独立编译**。
- **FLV/AMF**：完整的 FLV mux/demux、AMF0/AMF3 编解码、H.264/H.265/AV1/AAC/MP3/Opus/G711 裸流转换，编解码覆盖面明显超出标准 FLV 规范（标准只到 H.264/AAC），是经过扩展的现代化版本。

**关键的构建证据链**（这一段推翻了此前"从未编译过"的假设，是更准确、更有故事性的结论）：

- Android 的 CMakeLists.txt **明确没有引用**这套代码。
- iOS 工程**同样没有引用**。
- **Windows 的 `ArLiveLite.vcxproj` 显式编译了整套 rtmp/librtmp+libflv**，推流侧引用的是 `pusher/ARRtmpPusher.cpp/.h`——但这两个文件**在当前磁盘上已经不存在**。`git log` 精确定位到删除它们的提交：

  ```
  commit 5c2c59b8 "优化推流稳定性 优化拉流稳定性" (2023-08-23)
  delete: ArLiveLite/pusher/ARRtmpPusher.cpp (-348行) / .h (-107行)
  create: ArLiveLite/pusher/ARFFPusher.cpp / ArFFWriter.cpp
  同步修改: Android CMakeLists.txt
  ```

  这一个提交，就是整个引擎从"自研 RTMP 推流器（ARRtmpPusher，基于 rtmp/librtmp+libflv+ArNetTcpClient+url.hpp）"切换到"FFmpeg avformat 推流器（ARFFPusher/ArFFWriter）"的确切迁移点。**它同步更新了 Android 的 CMakeLists，却唯独没有碰 Windows 的 `.vcxproj`**——这份工程文件里的 36 个残留 `.o` 文件，也是当年随源码一次性以二进制形式提交进 git 的（`f051d7e "Add new code"`），几乎可以断定整个 `rtmp/` 目录是从别处整包拷贝进来的。

**结论**：这套自研协议栈**当前没有任何一个平台能够实际编译链接进最终产物**——Windows 理论上曾经是唯一使用方，但现在直接用这份 vcxproj 编译会因为找不到 `ARRtmpPusher.cpp/.h` 而失败，处于"断链"状态。它留在仓库里的价值：①学习 RTMP 协议底层实现细节的素材（客户端方向的实现相当完整，是比 FFmpeg 源码更易读的参考）；②Windows 端"曾经"用过，如果未来想摆脱对 FFmpeg avformat 的依赖可以重新捡起来；③目前是名副其实、有明确历史脉络的技术债。

`rtmp/CMakeLists.txt` 本身还是一份**孤立**的构建脚本（自己能构建成一个静态库，但没有任何外部 CMakeLists 通过 `add_subdirectory` 拉入它），唯一合理解释是供开发者手动单独构建/单测这个模块用。

## 6. 三端渲染/采集适配层

### 6.1 构建关系的第一手核实（纠正一个容易犯的错）

先说一个重要前提：**源码目录里存在的文件，不代表它真的被编译进对应平台的产物**。核实结果：

| 文件 | Android | iOS | Windows |
|---|---|---|---|
| `AndroidRenderer.cpp/.h` | ✓ | — | — |
| 根目录 `IosRenderer.cpp/.h` | — | **✗（未引用）** | — |
| `iOS/IosRenderer.mm` | — | ✓ | — |
| `WinVideoTrackSource.cpp/.h` | — | **✓（意外）** | ✓ |

**根目录的 `IosRenderer.cpp/.h` 是死代码**——iOS 工程完全没把它列入 Sources，真正参与编译的是 `iOS/IosRenderer.mm` 里定义的另一个类 `IOSRender`（注意类名不同）。这份死代码的 `OnFrame` 实现里 `video_sink_` 从未被赋值、恒为空指针，即便被编译进某个假设的工程也是彻底的空操作——很可能是早期"先写跨平台骨架、Android 走完了对应的 `AndroidRenderer.cpp`，iOS 后来另起炉灶写了 `iOS/IosRenderer.mm`，旧骨架文件忘记删除"的产物。

而 `WinVideoTrackSource.cpp/.h` 令人意外地**同时被 Windows 和 iOS 工程编译**——这不是共享良好设计，而是下面第 6.3 节要讲的一处类型强转疑点。

### 6.2 三平台视频渲染实现方式

三平台渲染器都实现同一个抽象基类 `webrtc::VideoRenderer`（本质是 `rtc::VideoSinkInterface<VideoFrame>` 加一个跨平台静态工厂 `CreatePlatformRenderer`），但具体实现"形似神不似"：

- **Android**：`AndroidRenderer` 本身**完全不碰 EGL/OpenGL ES**，只是一层极薄的 JNI 桥接——通过 `webrtc::JavaToNativeVideoSink()` 把上层传入的 Java 对象（通常是 `org.webrtc.SurfaceViewRenderer` 内部的 sink）包装成 C++ 适配器，`OnFrame` 收到帧后直接转发。**真正把 YUV/纹理帧上屏的工作全部发生在 Java 层**（WebRTC Android AAR 内部），不在 ArLiveLite 源码范围内。
- **iOS**：`iOS/IosRenderer.mm` 里的 `IOSRender` 是 Objective-C++ 桥接，最终通过 `__bridge` 转换到 `id<RTCVideoRenderer>` 协议对象，实际视图是 WebRTC 官方基于**Metal**（MTKView）实现的 `RTCMTLVideoView`（在 `Prj-iOS/ARLiveKit/ARLivePlayer.mm` 里能确认这个实际调用）。C++ 层只做帧格式转换和转发，不直接操作 Metal API。内存管理上，`IOSRender` 对 `RTCMTLVideoView` 是**弱引用式持有**（`__bridge` 非拥有型转换），生命周期完全依赖上层 ARC 环境保证视图存活。
- **Windows**：`WinVideoTrackSource` **不是渲染器**，是纯粹的采集 TrackSource（继承 `rtc::AdaptedVideoTrackSource`，`remote()` 恒为 false）。Windows 真正的渲染实现在 `VideoRender/d3d_renderer.cc`（基于 Direct3D），与 `ArLiveLite/WinVideoTrackSource` 完全无关，属于推流采集路径和拉流渲染路径两个不同子系统。

三者共享的唯一公共契约只有 `CreatePlatformRenderer` 这一个静态工厂签名+`OnFrame` 虚方法，除此之外没有任何统一的"帧预处理/旋转镜像"抽象层——`MgrRender::SetRotation/SetFillMode` 和 `IOSRender::Mirror()/DoScale()` 目前都是空实现，统一控制面这层的抽象骨架画好了但没有收敛实现。

### 6.3 iOS 采集里的一处类型强转疑点

`ObjcVCMCapturer::OnFrame` 把 `video_source_` 强转成 `WinVideoTrackSource*`：

```cpp
((WinVideoTrackSource *)video_source_.get())->FrameCaptured(frame);
```

而 iOS 实际创建的 TrackSource 是 `webrtc::ObjCVideoTrackSource`（在 `PlatformImpl.cpp` 里能确认），**两者是完全不同的类、不在同一继承体系**。这段代码之所以能编译通过，是因为 `WinVideoTrackSource.h/.cpp` 被显式加入了 iOS Xcode 工程的编译列表——对比 Windows 端 `VcmCapturer.cpp` 里同样的强转写法（那里是正确的，因为 Windows 确实创建的就是 `WinVideoTrackSource`），可以确定这是**"跨平台代码借用后忘记适配类型"的技术债**：iOS 的采集回调代码大概率是从 Windows 的 `VcmCapturer.cpp` 复制过来，但没把类型换成 iOS 实际使用的 `ObjCVideoTrackSource`。是否真的会出问题取决于 `ObjCVideoTrackSource` 是否恰好提供了签名/内存布局兼容的 `FrameCaptured` 方法，这是需要在后续维护中重点核实的风险点。

顺带一提，`ObjcVCMCapturer` 还继承了 `MgrRender`——但全文没有调用任何继承来的方法，是明显的复制粘贴遗留（容易让人误以为采集器和渲染管理器之间存在某种协作关系，实际没有）。

### 6.4 美颜滤镜：插在采集环节，不是渲染环节

`ARCustomVideoFilter`（iOS）基于 `CoreVideo` 的 `CVPixelBufferRef` 做磨皮/美白/红润参数化处理。它的介入点是**摄像头原始采集帧→美颜滤镜处理→得到处理后帧→再交给下游编码/预览**，即采集管线里的前处理环节，而不是拉流播放端的渲染后处理。

### 6.5 iOS 的 HTTP 客户端：一个已实现但未被调用的补充能力

`ArHttpClient`（纯 C API，三平台导出宏都准备好了）+`ObjcNetWorkManager`（基于 `NSURLSession`）是**只在 iOS 落地**的 HTTP 短连接封装，和 `ArNetClient`/`ArNetTcpClient`（全平台的长连接信令客户端）服务于完全不同的场景，彼此没有包装关系。全仓库搜索 `ArHttpClientDoRequest` 没有找到调用方，是"接口已声明、iOS 已实现，但尚未被上层业务接入"的半成品能力。

## 7. 跨平台构建总览

- **Android**（`Prj-Android/liveplayer/src/main/cpp/CMakeLists.txt`）：`ARLIVE_DIR` 变量指向仓库根目录 `ArLiveLite/`，把引擎核心 `.cpp/.cc` 直接编译进 `libanyLive.so`，预编译的 WebRTC（`libwebrtc.a`）、FFmpeg（各 `.so`）通过 `IMPORTED` 方式链接。
- **iOS**（`Prj-iOS/ARLiveLibrary.xcodeproj`）：用 Xcode group 直接引用 `../../ArLiveLite` 同一份源码，Header Search Paths 里加了 `../ArLiveLite` 及其 `include/webrtc/...` 等路径。
- **Windows**（`ArLiveLite/ArLiveLite.vcxproj`）：独立 VS 工程，就地编译同批核心源码，但如第 5.3 节所述，这份工程文件本身因为引用了已删除的 `ARRtmpPusher.cpp/.h` 而处于**编译不通**的状态，是一份未随代码演进同步维护的过时快照。

| 文件 / 子系统 | Android | iOS | Windows | 备注 |
|---|---|---|---|---|
| ArLive2Engine/Pusher/Player.cpp | ✓ | ✓ | ✓ | 核心业务逻辑，三端共用 |
| codec/AvCodec.cc, aacencode.cc | ✓ | ✓ | ✓ | 视频编码走此层，解码分支未被使用 |
| ARFFPusher / ArFFWriter.cpp | ✓ | ✓ | — | 现行 FFmpeg 推流实现 |
| pusher/ARRtmpPusher.cpp/.h | 已删除 | 已删除 | vcxproj 仍引用（断链） | 2023-08-23 提交删除源文件，Windows 工程未同步 |
| rtmp/librtmp + rtmp/libflv | — | — | vcxproj 列出但断链 | ~9600 行，随 ARRtmpPusher 一起成为孤儿 |
| ArNetClient / ArNetTcpClient | 编译无调用 | 编译无调用 | 编译无调用 | 自研 TCP 信令客户端，实际网络走 FFmpeg avio |
| url.cpp / url.hpp | — | 编译无调用 | — | 通用 URL 解析器，仅 iOS 编译且孤立 |
| AndroidRenderer.cpp/.h | ✓ | — | — | JNI 转发到 Java SurfaceViewRenderer |
| 根目录 IosRenderer.cpp/.h | — | 死代码，未入工程 | — | OnFrame 内 video_sink_ 恒为空 |
| iOS/IosRenderer.mm (IOSRender) | — | ✓ | — | 桥接 RTCMTLVideoView，Metal 渲染 |
| WinVideoTrackSource.cpp/.h | — | ✓（被复用） | ✓ | iOS 采集回调里被强转使用，类型不匹配疑点 |
| VideoRender/d3d_renderer.cc | — | — | ✓ | Windows 拉流渲染，与推流采集无关 |
| MgrRender.cpp/.h | ✓ | ✓ | ✓ | 推流预览与拉流播放共用的渲染分发表 |

## 8. 汇总：精心设计 vs 技术债

**做得漂亮的地方**：

- `MThreadTick` 的"延迟反注册+锁外收尾回调"模式，避免持锁期间调用虚函数。
- 软硬编解码器"能力探测+优雅降级"的统一 fallback 模式（Encoder/Decoder 构造对称）。
- 两级缓冲（FFBuffer 网络抗抖动 + PlayBuffer 渲染平滑）职责边界清晰，量级刻意错开。
- `MgrRender` 让本地预览和远端拉流共用同一套渲染分发逻辑。
- SEI 消息与关键帧生命周期的联动（"250ms 未等到自然关键帧就主动请求"）。
- `ArFFWriter` 的"影子编码器"手法，借 FFmpeg 生成 SPS/PPS/AudioSpecificConfig 而不用自己手写编解码逻辑。

**技术债/历史包袱**（按影响力排序）：

1. `setSpeed` 变速播放全链路是空实现，sonic 库大材小用成音量控制器。
2. `FFBuffer` 起播缓冲阈值只增不减，长时间运行延迟会阶梯式恶化。
3. `V_H264Decoder`/`IArVidEncoder` 定义完整但全仓库无调用方，是两套"死掉"的编解码抽象。
4. `ArLive2Pusher::exVideo_encoder_factory` 构造时未初始化，存在读到垃圾指针的风险。
5. 根目录 `IosRenderer.cpp/.h` 是死代码骨架标本；`ObjcVCMCapturer` 里的类型强转疑似照搬 Windows 代码未适配。
6. `rtmp/librtmp+libflv`（约 9600 行，源自 ireader/media-server）曾支撑 Windows 的自研推流器 `ARRtmpPusher`，2023-08-23 切换到 FFmpeg 后被清理不彻底，导致 Windows 工程现在实际编译不通。
7. `ArNetClient/ArNetTcpClient`、`url.cpp` 是同一批历史遗留，编译进产物但零调用点。
8. 错误码/事件码体系直接照搬腾讯云 TRTC，大量僵尸常量和真实的腾讯内部联系方式残留在注释里。
9. `ArFFWriter` 里音频 extradata 依赖两个独立 AAC 实现"恰好"生成一致配置字节，没有运行时校验。
10. `av_interleaved_write_frame` 在视频编码线程和音频采集线程上并发调用同一个 `AVFormatContext`，缺少互斥保护。

---

**方法论说明**：以上内容基于对 `ArLiveLite/` 全量源码（约 9700 行核心业务代码 + `rtmp/` 目录约 9600 行第三方协议栈代码）的逐行阅读，以及 Android CMakeLists、iOS Xcode 工程、Windows vcxproj 三份构建脚本的交叉核对与 git 历史核实整理而成。
