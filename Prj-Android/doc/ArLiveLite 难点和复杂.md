# ArLiveLite 难点和复杂点（含已发现的 bug / workaround / 死代码）

> 这些是精读代码时实际发现的、值得重点关注的坑点，部分是真实生产事故留下的注释，部分是可复现的 bug，部分是设计上的技术债。按"值得投入时间程度"大致排序。
>
> 说明：本仓库部分 `.cc`/`.cpp` 文件按 GBK 保存，用 UTF-8 读取时中文注释会显示为乱码（如 `��ȡ���ݵ�ʱ��`）。下面引用到这类注释时按原样保留乱码，不做"看似合理"的猜测性还原，含义另用中文说明。

## 1. 【真 bug，可修复】`ARFFPlayer` 视频时间戳用错了 timebase

`player/ARFFPlayer.cpp` 的 `ReadThreadProcess()`（第 344-372 行）里，视频分支的 pts/dts 换算错误地使用了**音频**的 timebase `astream_timebase_`：

```cpp
344		if (packet->stream_index == n_video_stream_idx_) {
345			n_net_vid_band_ += packet->size;
346			int64_t pts = 0;
347			int64_t dts = 0;
348			if (packet->dts == AV_NOPTS_VALUE) {
349				dts = 0;
350			}
351			else {
352				dts = av_rescale_q(packet->dts, astream_timebase_, TIMEBASE_MS);   // bug：应为 vstream_timebase_
353			}
354			if (packet->pts == AV_NOPTS_VALUE) {
355				pts = 0;
356			}
357			else {
358				pts = av_rescale_q(packet->pts, astream_timebase_, TIMEBASE_MS);   // 同样错误
359			}
...
366			if (b_no_buffer_) {
367				OnBufferDecodeVideoData(packet);
368				av_packet_unref(packet);
369				delete packet;
370			}
371			else {
372				FFBuffer::RecvVideoData(packet, dts, pts, av_rescale_q(packet->duration, vstream_timebase_, TIMEBASE_MS));  // duration 这一行反而是对的
373			}
```

音频分支（紧接在这段代码之后，用同样的 `astream_timebase_` 处理音频包）是正确的，因为音频包本来就该用音频的 timebase；出问题的只有视频分支的 dts/pts 两行。

**对照：同一文件里"正确"的用法**——`OnBufferDecodeVideoData()`（约第 683-691 行，`b_no_buffer_` 为真时的低延迟路径）里，同样是从 `AVPacket` 换算视频 pts，这里用对了 `vstream_timebase_`：

```cpp
683	void ARFFPlayer::OnBufferDecodeVideoData(AVPacket* vid_packet)
684	{
685		{
686			int64_t pts = av_rescale_q(vid_packet->dts, vstream_timebase_, TIMEBASE_MS);
687			//printf("Time: %d pts:%lld \r\n", rtc::Time32(), pts);
688			if (vid_packet->flags & AV_PKT_FLAG_KEY) {
689				ParseVideoSei((char*)vid_packet->data, vid_packet->size, pts);
690			}
691		}
```

两处对比可以看出：`vstream_timebase_` 这个变量本身是存在且被正确使用过的，`ReadThreadProcess()` 里第 352/358 行纯粹是手误写错了变量名，不是概念性错误。当视频流和音频流的 `time_base` 不同时（最常见的情况——视频常见 `1/90000` 或跟随帧率，音频常见 `1/44100`/`1/48000`/`1/16000`），走 `b_no_buffer_ == false`（有缓冲）路径的视频包 dts/pts 会被错误缩放，直接污染 `FFBuffer` 里基于这个值做的缓存时长估算和音画同步判断，**会造成音画不同步、缓存时长误判、播放节奏错乱**。

**建议**：这是本仓库里性价比最高的一个"从阅读到修复"练手任务——影响明确、可复现、修复成本是把 352/358 两行的 `astream_timebase_` 改成 `vstream_timebase_`。

## 2. 【反直觉但不是 bug】FLV muxer 时间戳被"故意"重新覆盖

`pusher/ArFFWriter.cpp` 里，`SetAudioEncData()`（音频，第 315-321 行）和 `SetVideoEncData()`（视频，第 411-418 行）都是同一个套路：先调用 `av_packet_rescale_ts` 把毫秒时间戳转换到流的 timebase（音频流 `time_base` 由 FFmpeg 按采样率设置，视频流固定 `1/90000`），但紧接着如果目标格式是 `"flv"`，又直接用原始毫秒值覆盖刚才 rescale 的结果。

音频分支（`SetAudioEncData`，第 301-321 行）：

```cpp
301		// Make avpacket
302		AVPacket av_packet;
303		av_init_packet(&av_packet);	//@Eric - 20230323 - ...
304		av_packet.stream_index = aud_stream_->index;
305		av_packet.flags = 0;// (flag == MediaPacketFlag::Key) ? AV_PKT_FLAG_KEY : 0;
...
311		av_packet.pts = pts;
312		av_packet_rescale_ts(&av_packet, AVRational{ 1, 1000 }, aud_stream_->time_base);
313		//av_packet.duration = 0;
314	
315		if (strcmp(format_context_->oformat->name, "flv") == 0)
316		{
317			av_packet.size = nLen;
318			av_packet.data = (uint8_t*)pData;
319			av_packet.pts = pts;
320			av_packet.dts = dts;
321		}
```

视频分支（`SetVideoEncData`，第 407-418 行）写法完全一致，并且这里的注释是可读的（未乱码），明确点出了"故意覆盖"的意图：

```cpp
407		av_packet.pts = pts;
408		av_packet_rescale_ts(&av_packet, AVRational{ 1, 1000 }, vid_stream_->time_base);
409		//av_packet.duration = 0;
410		
411		if (strcmp(format_context_->oformat->name, "flv") == 0)
412		{
413			av_packet.size = nLen;
414			av_packet.data = (uint8_t*)pData;
415			//* 对于FLV的时间戳，用原始的就行（该行注释在 GBK 源文件中，UTF-8 读取显示为乱码）
416			av_packet.pts = pts;	
417			av_packet.dts = dts;
418		}
419		else if (strcmp(format_context_->oformat->name, "mp4") == 0)
```

这不是笔误，而是利用了 FFmpeg `libavformat/flvenc.c` 的特性——**FLV muxer 内部直接按"毫秒"解释 `pkt->dts` 写 FLV Tag Timestamp，不走标准的按 `stream->time_base` 折算的路径**。前面设置的 timebase 对 flv 输出其实没有意义，纯粹是为了让 mp4/mpegts 等其它分支复用同一段代码路径。

**风险点**：如果有人"清理代码"时删掉了这个看似多余的覆盖逻辑，或者把 muxer 从 flv 换成 mpegts 却没意识到需要去掉这个特殊处理，时间戳会直接错乱。这是一个非常容易被误删的 FFmpeg 细节，建议重点理解并在代码里保留清晰注释。

## 3. 【生产级 workaround】"影子编码器"造 SPS/PPS extradata

`ArFFWriter.cpp` 的 `Connect()`（第 479-800 行）为了填充 `AVCodecParameters->extradata`（RTMP/FLV 的 AVCDecoderConfigurationRecord 需要），专门用 FFmpeg 的 libx264 **新建一个完全独立、参数写死**的 `AVCodecContext`，打开后立刻从它的 `extradata` 里拷出 SPS/PPS，然后销毁。分辨率/码率写死为 1920x1080/1024kbps（第 510-513 行的 `w`/`h`/`nBitrate` 局部变量），并强制 `preset=ultrafast`/`tune=zerolatency`/`profile=baseline`：

```cpp
542			AVCodecContext* vidEncodeCtx = NULL;
543			vidEncodeCtx = avcodec_alloc_context3(codec);
544			if (vidEncodeCtx != NULL) {
...
565					vidEncodeCtx->width = w;      // 分辨率宽（写死 1920）
566					vidEncodeCtx->height = h;     // 分辨率高（写死 1080）
...
584					vidEncodeCtx->bit_rate = nBitrate * 1000; // 码率（写死 1024kbps）
```

```cpp
598				//编码器预设
599				AVDictionary* param = 0;
600				if (vidEncodeCtx->codec_id == AV_CODEC_ID_H264)
601				{
602					av_dict_set(&param, "preset", "ultrafast", 0);
603					//av_dict_set(&param, "preset", "superfast", 0);
604					av_dict_set(&param, "tune", "zerolatency", 0); //实现实时编码
605					av_dict_set(&param, "profile", "baseline", 0);
606				}
```

打开成功后，把这个临时编码器的 `extradata` 拷进真正要发送给 muxer 的 `codecpar->extradata`，随后立刻销毁临时 context：

```cpp
634					else {
635						if (vidEncodeCtx->extradata_size > 0 && vidEncodeCtx->extradata != NULL) {
636							codecpar->extradata_size = vidEncodeCtx->extradata_size;
637							codecpar->extradata = (uint8_t*)av_malloc(vidEncodeCtx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
638							if (codecpar->extradata == NULL) {
639								printf("could not av_malloc the video params extradata! \r\n");
640								return -1;
641							}
642							memcpy(codecpar->extradata, vidEncodeCtx->extradata, vidEncodeCtx->extradata_size);
643						}
644					}
645	
646					avcodec_free_context(&vidEncodeCtx);
647					vidEncodeCtx = NULL;
648				}
```

这个临时编码器和真正编码画面的 `V_H264Encoder`（openh264 软编码，参数由业务方实际设置）**完全无关**，两者的 profile/level 只是恰好都被强制成了 baseline 才没出问题。

若 `avcodec_open2` 失败（例如目标 FFmpeg 未编译 libx264），还有一份**硬编码的 SPS/PPS 字节数组兜底**（第 618-630 行），注释直接引用了一篇关于"B 站播放器兼容性"的博客，是有据可查的真实兼容性 workaround：

```cpp
615				if (res < 0) {
616					char buff[128] = { 0 };
617					//av_strerror(res, buff, 128);
618					if (vid_codec_type_ == CT_H264)
619					{
620						//* 这个可以搞定，B站不放了：https://www.cnblogs.com/subo_peng/p/7800658.html
621						unsigned char sps_pps[23] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x0a, 0xf8, 0x0f, 0x00, 0x44, 0xbe, 0x8,
622							  0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80 };
623						codecpar->extradata_size = 23;
624						codecpar->extradata = (uint8_t*)av_malloc(23 + AV_INPUT_BUFFER_PADDING_SIZE);
```

## 4. 【生产事故记录】`av_init_packet` 踩坑注释

同样的注释在 `ArFFWriter.cpp` 里出现两次——`SetAudioEncData()` 第 303 行、`SetVideoEncData()` 第 399 行，字面内容一致（GBK 源码在 UTF-8 下读出来是乱码）：

```cpp
301		// Make avpacket
302		AVPacket av_packet;
303		av_init_packet(&av_packet);	//@Eric - 20230323 - ����ʹ��av_packet = { 0 };��ʼ��������ʹ��av_init_packet��ʼ�� - ����¼�Ƶ��ļ������Բ�
304		av_packet.stream_index = aud_stream_->index;
```

```cpp
397		// Make avpacket
398		AVPacket av_packet;
399		av_init_packet(&av_packet);	//@Eric - 20230323 - ����ʹ��av_packet = { 0 };��ʼ��������ʹ��av_init_packet��ʼ��
400		av_packet.stream_index = vid_stream_->index;
```

按注释原文含义（作者本意）：不能用 `av_packet = { 0 }` 初始化，必须用 `av_init_packet` 初始化——否则录制的文件不能拖动。`AVPacket` 若不走 `av_init_packet` 会缺少某些默认字段初始化（例如 `pts`/`dts`/`pos` 应初始化为 `AV_NOPTS_VALUE`/`-1` 而不是 `0`），导致生成文件的索引/seek 信息异常。这是一条真实踩过的生产 bug 记录，值得作为"看似等价的初始化方式其实不等价"的案例。

## 5. 【技术债】渲染旋转/填充模式 API 存在但实现为空

`MgrRender::SetRotation`/`SetFillMode`（`MgrRender.cpp` 第 27-35 行）是空实现：

```cpp
27	void MgrRender::SetRotation(const char* pId, int nRotation)
28	{
29	
30	}
31	
32	void MgrRender::SetFillMode(const char* pId, int nMode)
33	{
34	
35	}
```

但上层 `ArLive2Player::setRenderRotation`（`ArLive2Player.cpp` 第 63-68 行）对外 API 是完整的，只是转发调用被主动注释掉了（第 66 行）：

```cpp
63	int32_t ArLive2Player::setRenderRotation(AR::ArLiveRotation rotation)
64	{
65		frameRotation = rotation;
66	//	ar_engine_->setVideoRenderRotation(str_local_play_id_.c_str(), rotation);
67		return AR::ArLIVE_OK;
68	}
```

对照的是 `setRenderFillMode`（紧邻的第 69-73 行），它是**真的**转发了调用（虽然最终落地到的还是上面那个空实现）：

```cpp
69	int32_t ArLive2Player::setRenderFillMode(AR::ArLiveFillMode mode)
70	{
71		ar_engine_->setVideoRenderFillMode(str_local_play_id_.c_str(), mode);
72		return AR::ArLIVE_OK;
73	}
```

也就是说**渲染层的旋转/填充设置目前是摆设**：`setRenderRotation` 连转发都没做，`setRenderFillMode` 转发了但落到的 `MgrRender::SetFillMode` 是空函数。实际画面旋转靠解码/渲染帧里的 `webrtc::VideoRotation` 字段处理。

## 6. 【技术债/未接线功能】变速播放是假的

`ArLive2Player::setSpeed`（`ArLive2Player.cpp` 第 140-153 行）有完整的合法值校验并调用 `ar_player_->SetSpeed(speed)`：

```cpp
140	int32_t ArLive2Player::setSpeed(float speed)
141	{
142		if (!main_thread_->IsCurrent()) {
143			return main_thread_->Invoke<int>(RTC_FROM_HERE, rtc::Bind(&ArLive2Player::setSpeed, this, speed));
144		}
145		if (ar_player_ == NULL) {
146			return AR::ArLIVE_ERROR_REFUSED;
147		}
148		if (speed != 0.5 && speed != 0.75 && speed != 1.0 && speed != 1.25 && speed != 1.5 && speed != 1.75 && speed != 2.0 && speed != 3.0) {
149			return AR::ArLIVE_ERROR_INVALID_PARAMETER;
150		}
151		ar_player_->SetSpeed(speed);
152		return AR::ArLIVE_OK;
153	}
```

但真正落地的 `ARFFPlayer::SetSpeed`（`player/ARFFPlayer.cpp` 第 521-523 行）函数体是空的：

```cpp
521	void ARFFPlayer::SetSpeed(float fSpeed)
522	{
523	}
```

`sonic`（本是为变速不变调设计的库）在这个代码库里**只被用来做音量缩放**，`sonicSetSpeed/sonicSetPitch/sonicSetRate` 从未被调用，`OnBufferGetSpeed()` 硬编码返回 `1.0`。上层校验做得很认真，底层却完全没接线，是一处典型的"看起来支持、实际不支持"的功能。

## 7. 【死代码】`ArFFWriter` 内部有一份从未被使用的重复 AAC 编码器

`ArFFWriter` 构造函数（`ArFFWriter.cpp` 第 191-207 行）自己又开了一个 AAC 编码器：

```cpp
191	ArFFWriter::ArFFWriter(void)
192		: vid_codec_type_(CT_H264)
...
201		, aac_enc_(NULL)
...
205	{
206		aac_enc_ = aac_encoder_open(2, 44100, 16, 64000, true);
207	}
```

但只有 `SetAudioRawData`（第 364-385 行）这条路径会用它：

```cpp
364	int ArFFWriter::SetAudioRawData(char* pData, int len, int sample_hz, int channels, int64_t pts, int64_t dts)
365	{
366		const int kMaxDataSizeSamples = 4196;
367		if (44100 != sample_hz || 2 != channels) {
368			return -1;
369		}
370		else {
371			int status = 0;
372			if (aac_enc_)
373			{
374				unsigned int outlen = 0;
375				uint32_t curtime = rtc::Time32();
376				uint8_t encoded[1024];
377				status = aac_encoder_encode_frame(aac_enc_, (uint8_t*)pData, len, encoded, &outlen);
378				if (outlen > 0)
379				{
380					SetAudioEncData((char*)encoded, outlen, pts, dts);
381				}
382			}
383		}
384		return 0;
385	}
```

而实际推流路径走的是 `ARFFPusher::setAudioData → SetAudioEncData`（已编码数据直传），从不调用 `SetAudioRawData`。生产路径的音频编码只在 `AvCodec.cc` 发生一次，这个 `aac_enc_` 从对象构造到析构（`~ArFFWriter` 里 `aac_encoder_close`）全程"活着"却没人调用它编码，属于纯粹的死代码。

## 8. 【死代码/孤儿模块】`ArNetClient`/`ArNetTcpClient`/`url.hpp` 完全未被引用

全仓库搜索确认，除了自己的 `.h/.cpp` 外没有任何文件调用它们——推流走 `avio_open2`（FFmpeg 内置 rtmp: protocol），拉流走 `avformat_open_input`，都不经过这两个自研网络类。很可能是为未来自研 RTMP（仓库里同样未编入 Android 构建的 `ArLiveLite/rtmp/` 目录）预留的传输层，或纯历史遗留。

其内部还有一处半成品：`ArNetTcpClient::doSendData`（`ArNetTcpClient.cpp` 第 73-78 行）直接调用 `Send()` 且完全不检查返回值：

```cpp
73	void ArNetTcpClient::doSendData(const char* pData, int nLen)
74	{
75		if (control_socket_ != NULL) {
76			control_socket_->Send(pData, nLen);
77		}
78	}
```

头文件（`ArNetTcpClient.h` 第 45-50 行）里声明的 `outbuf_`/`max_outsize_`（发送缓冲/背压控制）在 `.cpp` 里从未被引用：

```cpp
45	private:
46	    std::unique_ptr<rtc::AsyncSocket> control_socket_;
47	
48	    rtc::Buffer				outbuf_;
49	    size_t max_outsize_;
50	};
```

设计了流控字段但没接上实现（因整条路径未被使用，实际无影响）。

## 9. 【死代码】`BitmapUtil::bitmapToRgba` 没有调用点

`jni/util/BitmapUtil.cpp` 封装了完整的 NDK Bitmap 像素访问+格式转换模板：

```cpp
11	// bitmapToRgba：对外的 JNI 入口，把一个 Java 层 Bitmap 转换成 RGBA 字节数组。
12	// 内部只负责"定义转换策略"（一个 lambda），真正的加锁/取像素/解锁等 NDK 样板代码全部交给
13	// 模板函数 bitmapToByteArray 去做，二者通过"回调"的方式协作，实现关注点分离。
14	jbyteArray bitmapToRgba(JNIEnv *env,jobject jbitmap) {
15	    // 提前判空：Java 层传入 null Bitmap 时直接返回 NULL，避免后续对空指针调用 NDK Bitmap API。
16	    if (jbitmap == NULL) {
17	        return NULL;
18	    }
```

但 Java 侧 `ArLivePusherImpl.java:336` 用纯 Java 实现了同名方法（走 `Bitmap.getPixels()`），完全没有调用这个 native 版本；且这个函数既没有 `JNIEXPORT jobject JNICALL` 修饰，函数名 `bitmapToRgba` 也不符合 `Java_<包名>_<类名>_<方法名>` 的 JNI 导出命名规范（对照同文件所在工程里 `LiveEngine.cpp` 里真正的 JNI 导出函数，例如 `Java_io_anyrtc_live_internal_NativeInstance_nativeCreatePushKit`），本身也无法被 Java 直接 `native` 调用——它只是一个被其它 JNI 入口内部调用的普通 C++ 辅助函数，但目前没有任何地方调用它。

## 10. 【疑似内存泄漏 / 类型不匹配】`LiveEngine.cpp` 若干问题点

多处 `new` 找不到对应 `delete`，例如 `nativeSetPushObserver`（`LiveEngine.cpp` 第 118-128 行）：

```cpp
121	JNIEXPORT void JNICALL
122	Java_io_anyrtc_live_internal_NativeInstance_nativeSetPushObserver(JNIEnv *env, jobject obj,jobject observer,jlong nativePtr) {
123	    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr);
124	    if (arLivePushKit != NULL){
125	        LivePushEvent *pushEvent = new LivePushEvent(observer);
126	        arLivePushKit->setObserver(pushEvent);
127	    }
128	}
```

和 `nativeStartVirtualCamera`（第 334-352 行）：

```cpp
334	JNIEXPORT jint JNICALL
335	Java_io_anyrtc_live_internal_NativeInstance_nativeStartVirtualCamera(JNIEnv *env, jobject thiz,
336	                                                                     jlong nativePtr,jint type,
337	                                                                     jbyteArray bitmap, jint width,
338	                                                                     jint height) {
339	    jint result = -1;
340	    IArLivePusher* arLivePushKit = reinterpret_cast<IArLivePusher *>(nativePtr);
341	    if (arLivePushKit!= NULL){
342	        jbyte* bufferPtr=(env)->GetByteArrayElements(bitmap, NULL);
343	        ArLiveImage *image = new ArLiveImage();
344	        image->imageType = static_cast<ArLiveImageType>(type);
...
348		result = arLivePushKit->startVirtualCamera(image);
349		env->ReleaseByteArrayElements(bitmap, bufferPtr, 0);
350	    }
351	    return (jint)result;
352	}
```

两处 `new` 出来的对象（`pushEvent`、`image`）在函数结束后既不释放也看不到被调用方接管所有权的约定，是否泄漏取决于 `setObserver`/`startVirtualCamera` 内部实现是否负责其生命周期，代码里没有明确文档说明这一点，值得进一步跟踪。

`nativeCreatePushKit`/`nativeCreatePlayKit` 引擎为空时把 `(jint)-1` 错误地当 `jlong` 返回：

```cpp
106	JNIEXPORT jlong JNICALL
107	Java_io_anyrtc_live_internal_NativeInstance_nativeCreatePushKit(JNIEnv *env, jobject obj) {
108	    InstanceHolder *instance = getInstanceHolder(env, obj);
109	    if (instance->arLiveEngine == nullptr){
110	        return (jint)-1;
111	    }
112	    IArLivePusher *arLivePushKit = instance->arLiveEngine->createArLivePusher(NULL);
...
115	    return reinterpret_cast<jlong>(arLivePushKit);
116	}
```

```cpp
829	JNIEXPORT jlong JNICALL
830	Java_io_anyrtc_live_internal_NativeInstance_nativeCreatePlayKit(JNIEnv *env, jobject obj) {
831	    InstanceHolder *instance = getInstanceHolder(env, obj);
832	    if (instance->arLiveEngine == nullptr){
833	        return (jint)-1;
834	    }
835	    IArLivePlayer* player = instance->arLiveEngine->createArLivePlayer(NULL);
836	   return reinterpret_cast<jlong>(player);
837	}
```

函数声明返回类型是 `jlong`（64 位），失败路径却返回 32 位 `int` 字面量 `-1` 再隐式转换——在绝大多数平台上符号扩展后数值仍是全 1（即 64 位的 `-1`），实际不会造成可观察的行为差异，但类型上是名不副实的写法，属于"侥幸没出事"的隐患点。

## 11. 【风险点】DirectByteBuffer 生命周期

`jni/liveEngine/ArLivePlayEvent.cpp` 的 `onRenderVideoFrame`（第 138-188 行）用 `NewDirectByteBuffer(videoFrame->data, videoFrame->length)` 包装 native 内存直接暴露给 Java：

```cpp
138	void LivePlayEvent::onRenderVideoFrame(IArLivePlayer* player, const ArLiveVideoFrame* videoFrame){
139	    if (m_jJavaObj != NULL && m_jClass !=NULL) {
140	
141	        JNIEnv *jni = webrtc::AttachCurrentThreadIfNeeded();
142	        {
...
155	            jmethodID j_callJavaMId = jni->GetMethodID(m_jClass, "onRenderVideoFrame",
156	                                                               "(II[BLjava/nio/ByteBuffer;III)V");
157	                jbyteArray dataArray = jni->NewByteArray(videoFrame->length);
158	                jobject _buf = jni->NewDirectByteBuffer(videoFrame->data, videoFrame->length);
159	                jni->SetByteArrayRegion(dataArray, 0, videoFrame->length,
160	                                        reinterpret_cast<const jbyte *>(videoFrame->data));
161	
162	                jni->CallVoidMethod(m_jJavaObj,j_callJavaMId,videoFrame->pixelFormat,videoFrame->bufferType,dataArray,_buf,videoFrame->width,videoFrame->height,videoFrame->rotation);
...
175	                jni->DeleteLocalRef(_buf);
176	        }
177	    }
178	}
```

（行号以本函数内实际相对顺序为准，`NewDirectByteBuffer` 调用见上文第 158 行。）`_buf` 包装的是 `videoFrame->data` 指向的同一块 native 内存，没有发生拷贝（对照旁边的 `dataArray` 是通过 `SetByteArrayRegion` 真正拷贝了一份到 JVM 堆）。这块内存的生命周期完全依赖调用方在方法返回前用完——`DeleteLocalRef(_buf)` 只释放了 Java 层的 `ByteBuffer` 包装对象引用，并不会、也不能让底层 native 内存失效或延长其生命周期。若 Java 侧异步持有该 `ByteBuffer`（比如塞进队列稍后处理），一旦 `videoFrame` 背后的 native 缓冲区被上游解码器复用或释放，就会变成悬空内存访问。

## 12. 【疑点，未定论】H264 打包参数注释

`codec/AvCodec.cc` 的 `V_H264Encoder::NewVideoEncoder()` 开头（第 525-535 行）：

```cpp
525	void V_H264Encoder::NewVideoEncoder()
526	{
527		std::string strCodecName = "H264";
528		webrtc::SdpVideoFormat sdpParam(strCodecName);
529		sdpParam.parameters[cricket::kH264FmtpPacketizationMode] = "1";	//@Eric - 导致单帧太大?
530	
531		std::unique_ptr<VideoEncoder> extern_encoder = NULL;
532		if (video_encoder_factory_ != NULL)
533		{
534			extern_encoder = video_encoder_factory_->CreateVideoEncoder(sdpParam);
535		}
```

作者自己怀疑这个参数导致单帧过大（`packetization-mode=1` 即 non-interleaved 模式，FU-A 分片模式下通常不会让帧变大，更像是对 RTP 打包模式的误解），但注释反映这是一处曾被怀疑但未解决的问题，值得实测验证而非直接采信注释结论。这条注释是可读的（未乱码），说明确实是原作者留下的疑问标记而非工具误读。

## 13. 【已知未修复 bug 标记】SEI 解析里被注释掉的 return

`ArLive2Player::OnArPlySeiData`（`ArLive2Player.cpp` 第 535-561 行）里有一行被主动注释掉的 `return`（第 560 行）：

```cpp
535	void ArLive2Player::OnArPlySeiData(void* player, const char* pData, int nLen, int64_t pts)
536	{
537		int semiLen = 4;
538		int nType = pData[4] & 0x1f;
539		if (pData[2] == 1) {
540			nType = pData[3] & 0x1f;
541			semiLen = 3;
542		}
543		if (nType == 6) {
544			int nPayloadType = pData[semiLen + 1] & 0xff;
545			if (nPayloadType == 5 || nPayloadType == 242) {// self defined
546				int nPayloadLen = 0;
547				const char* sei = pData + semiLen + 2;
548				//数据长度
549				int sl = 0;
550				do {
551					sl = (*sei) & 0xff;
552					sei++;
553					nPayloadLen += sl;
554				} while (sl == 255);
555	
556				if (observer_ != NULL) {
557					observer_->onReceiveSeiMessage(this, nPayloadType, (uint8_t*)sei, nPayloadLen);
558				}
559			}
560			//return;	//@Eric - bug 可能包含乱花（花）
561		}
562	}
```

第 560 行的注释同样是可读的（未乱码）：`//return; //@Eric - bug 可能包含乱花（花）`。配合被主动注释掉的 `return`，说明作者曾经加过提前退出，但发现有 bug 后临时把这行注释掉了——当前 `nType == 6` 分支在处理完 `nPayloadType == 5/242` 的情况后，会继续往下执行（此处外层再无更多逻辑，实际影响有限），但这行被特意保留的注释是留在代码里的已知问题标记，说明该分支曾经存在过某种因为提前 return 导致的处理不完整问题。

## 14. 【架构层面的隐含约定】断线重连 = 完整重建，不是断线续传

`ArFFWriter::Release()`（`ArFFWriter.cpp` 第 801-823 行，写包出错或主动停止时触发）：

```cpp
801	void ArFFWriter::Release()
802	{
803		if (format_context_ != nullptr)
804		{
805			if (format_context_->pb != nullptr)
806			{
807				av_write_trailer(format_context_);
808				avformat_close_input(&format_context_);
809			}
810	
811			avformat_free_context(format_context_);
812	
813			format_context_ = nullptr;
814		}
815	
816		vid_stream_ = NULL;
817		aud_stream_ = NULL;
818	
819		if (avcc_data_ != NULL) {
820			delete avcc_data_;
821			avcc_data_ = NULL;
822		}
823	}
```

会 `av_write_trailer` + `avformat_close_input` + `avformat_free_context` 把 `format_context_`、`vid_stream_`、`aud_stream_` 全部清空归零，没有保留任何"续传"用的状态。下一次 `Connect()`（见第 3 条）等价于重新走一遍 `avformat_alloc_output_context2` → 新建 stream → 影子编码器重新生成 extradata → RTMP 的 connect/createStream/publish 三次握手。理解这一点后，"重连速度取决于握手耗时 + 等到下一个关键帧（最多 3 秒）"这个体感延迟就有了明确的代码依据。

## 15. 【平台差异】Android 大概率始终走软编码

`AvCodec.cc` 的 `V_H264Encoder::NewVideoEncoder()`（第 525-558 行完整实现）优先尝试外部注入的硬件编码器工厂，失败或未注入时才用软编码兜底：

```cpp
525	void V_H264Encoder::NewVideoEncoder()
526	{
527		std::string strCodecName = "H264";
528		webrtc::SdpVideoFormat sdpParam(strCodecName);
529		sdpParam.parameters[cricket::kH264FmtpPacketizationMode] = "1";	//@Eric - 导致单帧太大?
530	
531		std::unique_ptr<VideoEncoder> extern_encoder = NULL;
532		if (video_encoder_factory_ != NULL)
533		{
534			extern_encoder = video_encoder_factory_->CreateVideoEncoder(sdpParam);
535		}
536		if (extern_encoder != NULL)
537		{// Try to use encoder use H/W
538			if (extern_encoder->InitEncode(&h264_, 1, 0) == WEBRTC_VIDEO_CODEC_OK)
539			{
540				encoder_.reset(extern_encoder.release());
541			}
542			else
543			{
544				extern_encoder = NULL;
545			}
546		}
547	
548		if (encoder_ == NULL)
549		{// Use software codec
550			encoder_ = webrtc::H264Encoder::Create(cricket::VideoCodec(sdpParam));
551			if (encoder_->InitEncode(&h264_, 1, 0) != WEBRTC_VIDEO_CODEC_OK)
552			{
553				assert(false);
554			}
555		}
556	
557		encoder_->RegisterEncodeCompleteCallback(this);
558	}
```

逻辑分三步：①若 `video_encoder_factory_` 非空（外部注入过），调用其 `CreateVideoEncoder` 尝试拿到硬件编码器并 `InitEncode`；②只要硬件路径初始化成功就用它（`encoder_.reset(...)`）；③否则（没注入工厂，或注入了但 `InitEncode` 失败）落到 `webrtc::H264Encoder::Create()`（openh264 软编码）兜底。

问题在于 `video_encoder_factory_` 从哪来：Android 分支的引擎创建流程没有像 iOS 分支那样自动注入硬编码工厂（对照第 10 条 `LiveEngine.cpp` 里 `nativeCreatePushKit` 确实调用了 `AndroidDeviceManager::Inst().makeVideoEncoderFactory()` 并 `setExVideoEncoderFactory`——但这是走推流器创建路径设置的工厂，`AvCodec.cc` 里的 `video_encoder_factory_` 是否与之衔接、衔接到什么程度需要结合两侧的赋值路径进一步确认），需要上层 App 显式调用 `setExVideoEncoderFactory`。如果没调用或没衔接上，Android 上实际一直在用软编码，这对高分辨率/高帧率场景的 CPU 占用和发热是需要重点关注的点。

## 16. 【技术债/隐藏 bug】FLV 输出流的 `codecpar` 元数据与真实编码参数完全脱钩

这是在研究配套的 FFmpeg-API 文档时顺带发现的，和第 3 条"影子编码器"是同一段代码但问题点不同、值得单独列出。

`ArFFWriter.cpp` 的 `Connect()` 第 502-524 行，创建真正要写进输出容器的视频流 `vid_stream_` 时：

```cpp
502		{//2, 
503			if (vid_codec_type_ != CT_None)
504			{//* Video
505				AVCodecID codecId = (vid_codec_type_ == CT_H264) ? AV_CODEC_ID_H264 : (vid_codec_type_ == CT_H265) ? AV_CODEC_ID_H265
506					: AV_CODEC_ID_NONE;
507				const AVCodec* codec = avcodec_find_encoder(codecId);
508				vid_stream_ = avformat_new_stream(format_context_, codec);
509				AVCodecParameters* codecpar = vid_stream_->codecpar;
510				int w = 1920;
511				int h = 1080;
512				int nFps = 30;
513				int nBitrate = 1024;
514	
515				codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
516				codecpar->codec_id = codecId;
517	
518				codecpar->codec_tag = 0;
519				codecpar->bit_rate = nBitrate * 1000;
520				codecpar->width = w;
521				codecpar->height = h;
522				codecpar->format = AV_PIX_FMT_YUV420P;
523				codecpar->sample_aspect_ratio = AVRational{ 1, 1 };
```

注意这里的 `w`/`h`/`nBitrate` 局部变量（1920/1080/1024kbps）**不是**第 3 条里"影子编码器"专属的临时值——它们同时被用来设置了 `vid_stream_->codecpar->width`、`codecpar->height`、`codecpar->bit_rate`（第 519-521 行），而 `vid_stream_` 正是这个 `ArFFWriter` 实例真正对外输出、写进 FLV 容器的视频流对象，不是第 3 条里那个用完即扔的 `vidEncodeCtx`。也就是说：**无论业务方实际把 `V_H264Encoder` 配置成什么分辨率/码率，FLV 容器里 `codecpar` 级别的宽高/码率元数据永远是硬编码的 1920x1080/1024kbps**，与 `SetVideoEncData()` 实际写入的、来自真实编码器的 H264 裸流完全独立、互不联动。

**后果**：FLV/AVCDecoderConfigurationRecord 之外、容器层面直接暴露的 `codecpar->width/height/bit_rate`（例如某些 demuxer 在解析 SPS 之前就先读取的"流参数"，或做流探测/预览时可能读取的元信息）会是错的，且这个错误从连接建立那一刻起就存在，与后续实际编码画面的分辨率、码率完全脱钩。**多数播放器基本不受影响**，因为它们真正解析视频尺寸时会去读 H264 裸流里的 SPS（NAL type 7），而不是信任容器 `codecpar` 里的这份宽高/码率，这也是这个问题至今没有被明显察觉、一直潜伏在代码里的原因。但任何依赖 `codecpar` 级别元数据做前置判断（例如不解码只读 stream info 做分辨率过滤/码率统计/预览缩略图逻辑）的下游消费方，都会拿到错误数据。
