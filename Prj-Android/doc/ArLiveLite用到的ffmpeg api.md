# ArLiveLite 用到的 FFmpeg API

> 范围：`ArLiveLite/pusher/ArFFWriter.cpp`（推流封装/发送）+ `ArLiveLite/player/ARFFPlayer.cpp`（拉流解封装/解码）。`ArLiveLite/player/FFBuffer.cpp` 不直接调用任何 FFmpeg API（纯业务缓冲逻辑）。所有代码片段均为源文件原文摘录，行号可直接跳转核对；源文件按 GBK 保存、当前以 UTF-8 读取，中文注释部分会显示为乱码，摘录时已尽量避开纯乱码注释行。

## 0. API 一览表

| 类别 | API | 出现文件:行 |
|---|---|---|
| 全局初始化 | `avformat_network_init/deinit` | ARFFPlayer.cpp:43,53 |
| 全局初始化（已废弃，未调用） | `av_register_all` | ARFFPlayer.cpp:42（注释掉） |
| 封装器创建 | `av_guess_format`、`avformat_alloc_output_context2` | ArFFWriter.cpp:484,490 |
| 解封装器创建 | `avformat_alloc_context`、`avformat_open_input`、`avformat_find_stream_info` | ARFFPlayer.cpp:891,922,938 |
| 流/编解码器参数 | `avformat_new_stream`、`avcodec_parameters_to_context`、`av_find_best_stream` | ArFFWriter.cpp:508,659 / ARFFPlayer.cpp:96,71 |
| 编解码器生命周期 | `avcodec_find_encoder/decoder`、`avcodec_alloc_context3`、`avcodec_open2`、`avcodec_free_context`、`avcodec_close` | 两文件均有，见下 |
| 编解码（现代 API） | `avcodec_send_packet`、`avcodec_receive_frame` | ARFFPlayer.cpp:606,611,699,704 |
| 编解码（旧 API，`#if 0` 死代码） | `avcodec_decode_audio4`、`avcodec_decode_video2` | ARFFPlayer.cpp:604,697 |
| I/O | `avio_open2` | ArFFWriter.cpp:764 |
| 收发包 | `av_interleaved_write_frame`、`av_read_frame` | ArFFWriter.cpp:337,450 / ARFFPlayer.cpp:341 |
| 容器生命周期 | `avformat_write_header`、`av_write_trailer`、`avformat_close_input`、`avformat_free_context` | ArFFWriter.cpp:775,807,808,811 / ARFFPlayer.cpp:940,1031 |
| Packet 工具 | `av_init_packet`、`av_packet_rescale_ts`、`av_packet_unref` | ArFFWriter.cpp:303,312 / ARFFPlayer.cpp:335,368 |
| 时间基转换 | `av_rescale_q`、`av_rescale`、`av_q2d` | 两文件均大量使用 |
| 选项设置 | `av_dict_set`、`av_dict_set_int`、`av_opt_set`（未启用） | 两文件均有 |
| 重采样 | `swr_alloc`、`swr_alloc_set_opts`、`swr_init`、`swr_convert`、`swr_free` | ARFFPlayer.cpp:632-646,990-1040 |
| Frame/Packet 分配 | `av_frame_alloc`、`av_frame_free`、`av_frame_unref`、`av_malloc` | 两文件均有 |
| 调试/信息 | `av_dump_format`、`av_strerror`、`av_get_media_type_string` | 两文件均有 |
| Seek | `av_seek_frame` | ARFFPlayer.cpp:230,291 |

---

## 1. 推流侧：`ArFFWriter.cpp` —— 用 avformat 手搓一个 RTMP/FLV Muxer

### 1.1 创建输出上下文

```cpp
// ArFFWriter.cpp:479-500
bool ArFFWriter::Connect()
{
	AVOutputFormat* output_format = nullptr;
	output_format = av_guess_format(str_format_.c_str(), nullptr, nullptr);  // "flv" / "rtp_mpegts" / "rtsp"
	if (output_format == nullptr) { Release(); return false; }

	int error = avformat_alloc_output_context2(&format_context_, output_format, nullptr, str_url_.c_str());
	if (error < 0) { /* av_strerror + Release() */ }
}
```
`str_format_` 由 `ARFFPusher::startTask`（见"具体功能"文档）按 URL scheme 传入 `"flv"`/`"rtp_mpegts"`/`"rtsp"`，`av_guess_format` 就是 FFmpeg 里"按短名找 `AVOutputFormat`"的标准写法。

### 1.2 添加视频流并生成 extradata（SPS/PPS）—— 本仓库最复杂的一段 FFmpeg 代码

```cpp
// ArFFWriter.cpp:507-522
const AVCodec* codec = avcodec_find_encoder(codecId);      // AV_CODEC_ID_H264 / H265
vid_stream_ = avformat_new_stream(format_context_, codec); // 加一路视频流
AVCodecParameters* codecpar = vid_stream_->codecpar;
int w = 1920, h = 1080, nFps = 30, nBitrate = 1024;         // ⚠️ 硬编码，与真实编码参数无关，见下

codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
codecpar->codec_id = codecId;
codecpar->bit_rate = nBitrate * 1000;
codecpar->width = w;
codecpar->height = h;
codecpar->format = AV_PIX_FMT_YUV420P;
```

**⚠️ 值得单独指出的细节**：这里被赋值的 `codecpar` 是**真正输出流** `vid_stream_->codecpar`（不是后面临时编码器的），但 `width`/`height`/`bit_rate` 用的是本函数局部写死的 `1920/1080/1024kbps`，与 `V_H264Encoder` 实际编码使用的分辨率/码率毫无关系。也就是说**推流生成的 FLV 容器里视频流的 codecpar 元数据（`onMetaData` 里的宽高信息来源之一）从一开始就是错的**，只是大多数播放器最终显示尺寸看的是 SPS 里的真实宽高而不是这个 codecpar，所以现象上不容易被发现。

紧接着为了填 `extradata`（AVCDecoderConfigurationRecord 需要的 SPS/PPS），又单独造了一个**完全独立、只为了拿 extradata**的编码器：

```cpp
// ArFFWriter.cpp:542-556, 590-614, 634-647
AVCodecContext* vidEncodeCtx = avcodec_alloc_context3(codec);
vidEncodeCtx->pix_fmt = AV_PIX_FMT_YUV420P;
vidEncodeCtx->width = w;              // 同样是写死的 1920
vidEncodeCtx->height = h;             // 写死的 1080
vidEncodeCtx->time_base = { 1, nFps };
vidEncodeCtx->gop_size = 3 * nFps;
vidEncodeCtx->max_b_frames = 0;       // 无 B 帧，PTS==DTS 的前提

AVDictionary* param = 0;
if (vidEncodeCtx->codec_id == AV_CODEC_ID_H264) {
	av_dict_set(&param, "preset", "ultrafast", 0);
	av_dict_set(&param, "tune", "zerolatency", 0);
	av_dict_set(&param, "profile", "baseline", 0);
}
vidEncodeCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // 关键：不设这个 flag，extradata 不会被填充
int res = avcodec_open2(vidEncodeCtx, codec, &param);
if (res < 0) {
    // 硬编码兜底 SPS/PPS 字节数组（23 字节），来源见下方"兜底"小节
} else if (vidEncodeCtx->extradata_size > 0) {
    codecpar->extradata_size = vidEncodeCtx->extradata_size;
    codecpar->extradata = (uint8_t*)av_malloc(vidEncodeCtx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(codecpar->extradata, vidEncodeCtx->extradata, vidEncodeCtx->extradata_size);
}
avcodec_free_context(&vidEncodeCtx);   // 拿到 extradata 后立刻销毁，这个编码器从未真正编码过一帧画面
```

**要点**：`AV_CODEC_FLAG_GLOBAL_HEADER` 是这段代码能工作的关键——设置这个 flag 后，libx264（或平台默认 H264 encoder）在 `avcodec_open2` 时就会把 SPS/PPS 写入 `AVCodecContext::extradata`，不需要真正编码一帧就能拿到。这是 FFmpeg "只要 extradata、不要编码"场景的标准取巧写法。

### 1.3 兜底：硬编码 SPS/PPS

```cpp
// ArFFWriter.cpp:618-630
if (res < 0) {
    if (vid_codec_type_ == CT_H264) {
        // 引用了一篇 B站播放器兼容性博客: https://www.cnblogs.com/subo_peng/p/7800658.html
        unsigned char sps_pps[23] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x0a, 0xf8, 0x0f, 0x00, 0x44, 0xbe, 0x8,
              0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80 };
        codecpar->extradata_size = 23;
        codecpar->extradata = (uint8_t*)av_malloc(23 + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(codecpar->extradata, sps_pps, 23);
    }
}
```
如果目标 FFmpeg 编译时没有 libx264（`avcodec_open2` 失败），就直接塞一份写死的 SPS(`0x67 0x42 0x00 0x0a...`profile=0x42=66=baseline) + PPS(`0x68 0xce 0x38 0x80`)。这份 extradata 与真实编码画面的 SPS/PPS 完全无关，只是为了让 muxer/播放器"有 extradata 可用不报错"。

### 1.4 音频流参数（AAC/MP3/OPUS/G711A）

```cpp
// ArFFWriter.cpp:655-677
AVCodecID codecId = (aud_codec_type_ == CT_AAC) ? AV_CODEC_ID_AAC :
    (aud_codec_type_ == CT_MP3) ? AV_CODEC_ID_MP3 :
    (aud_codec_type_ == CT_OPUS) ? AV_CODEC_ID_OPUS :
    (aud_codec_type_ == CT_G711A) ? AV_CODEC_ID_PCM_ALAW : AV_CODEC_ID_NONE;
aud_stream_ = avformat_new_stream(format_context_, avcodec_find_encoder(codecId));
codecpar->bit_rate = 64000;
if (codecId == AV_CODEC_ID_PCM_ALAW) {
    codecpar->channels = 1; codecpar->channel_layout = AV_CH_LAYOUT_MONO; codecpar->sample_rate = 8000;
} else {
    codecpar->channels = 2; codecpar->channel_layout = AV_CH_LAYOUT_STEREO; codecpar->sample_rate = 44100;
}
```
extradata（AAC 的 AudioSpecificConfig）生成方式与视频完全一致：造一个独立 `AVCodecContext` 走 `avcodec_open2` 拿 extradata 再销毁（`:689-729`）。

### 1.5 协议相关的 `av_dict_set` 选项（按 scheme 分支）

```cpp
// ArFFWriter.cpp:736-760
AVDictionary* options = nullptr;
if (是 rtmp:// 或 rtmps://) {
    char tc_url[1024];
    sprintf(tc_url, "%.*s", strrchr(str_url_.c_str(), '/') - str_url_.c_str(), str_url_.c_str());
    av_dict_set(&options, "rtmp_tcurl", tc_url, 0);
    av_dict_set(&options, "fflags", "flush_packets", 0);
    av_dict_set(&options, "rtmp_flashver", "FMLE/3.0 (compatible; FMSc/1.0)", 0);  // 伪装 Adobe FMLE，兼容部分 CDN 的白名单校验
}
else if (是 rtsp://) {
    av_dict_set(&options, "buffer_size", "4096000", 0);
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0);   // 单位微秒 = 5s
    av_dict_set(&options, "max_delay", "500000", 0);
}
else if (是 rtp://) {  /* 同上，rtp_transport=tcp */ }
else {
    av_dict_set(&options, "fflags", "flush_packets", 0);
}
```

### 1.6 打开 I/O、写 Header、写包、写 Trailer

```cpp
// ArFFWriter.cpp:762-785
if (!(format_context_->oformat->flags & AVFMT_NOFILE)) {
    avio_open2(&format_context_->pb, format_context_->url, AVIO_FLAG_WRITE, nullptr, nullptr);
}
avformat_write_header(format_context_, &options);   // 真正发起 RTMP connect/createStream/publish 握手
av_dump_format(format_context_, 0, format_context_->url, 1);  // 调试信息打到 stderr

// SetVideoEncData / SetAudioEncData 内部（:303-337,399-450）：
AVPacket av_packet;
av_init_packet(&av_packet);   // ⚠️ 不能用 av_packet = {0}，否则生成文件不可拖动（作者踩坑注释 20230323）
av_packet_rescale_ts(&av_packet, {1,1000}, stream->time_base);
if (str_format_ == "flv") {
    // 对 FLV 特殊处理：直接用原始毫秒覆盖掉刚 rescale 的结果
    av_packet.pts = pts_ms; av_packet.dts = dts_ms;
}
av_interleaved_write_frame(format_context_, &av_packet);

// Release()（:801-823）
av_write_trailer(format_context_);
avformat_close_input(&format_context_);
avformat_free_context(format_context_);
```

---

## 2. 拉流侧：`ARFFPlayer.cpp` —— avformat 解封装 + avcodec 解码 + swresample 重采样

### 2.1 全局初始化（Meyer's Singleton 包装）

```cpp
// ARFFPlayer.cpp:38-55
class FFContex {
protected:
    FFContex(void) {
        //av_register_all();  // 新版本已经不需要（旧 FFmpeg 需要，新版本内部自动注册所有 codec/format）
        avformat_network_init();
    };
public:
    static FFContex& Inst() { static FFContex gInst; return gInst; }
    virtual ~FFContex(void) { avformat_network_deinit(); };
};
```
用 C++11 function-local static 保证 `avformat_network_init()`（初始化网络协议模块，如 TLS/OpenSSL）全进程只调用一次，`ARFFPlayer` 构造函数里 `FFContex::Inst()` 触发。

### 2.2 打开输入、按协议设置网络选项

```cpp
// ARFFPlayer.cpp:887-927
fmt_ctx_ = avformat_alloc_context();
fmt_ctx_->interrupt_callback.callback = custom_interrupt_callback;  // 见 2.3，用于超时/主动中断阻塞 I/O
fmt_ctx_->interrupt_callback.opaque = this;

AVDictionary* options = NULL;
av_dict_set(&options, "nobuffer", "1", 0);
if (是 rtmp://) { av_dict_set(&options, "timeout", NULL, 0); }
if (是 rtsp://) {
    av_dict_set(&options, "rtsp_transport", b_use_tcp_ ? "tcp" : "udp", 0);
    av_dict_set(&options, "stimeout", "3000000", 0);   // 3s 超时
}
if (是 http:// 或 https://) {
    av_dict_set(&options, "timeout", "3000000", 0);
    av_dict_set_int(&options, "multiple_requests", 1, 0);
    av_dict_set_int(&options, "read_ahead_limit", INT_MAX, 0);
}
avformat_open_input(&fmt_ctx_, str_play_url_.c_str(), NULL, &options);

fmt_ctx_->probesize = 128 * 1024;              // 减小探测数据量，加快起播
fmt_ctx_->max_analyze_duration = 1 * AV_TIME_BASE;
avformat_find_stream_info(fmt_ctx_, NULL);
```
`probesize`/`max_analyze_duration` 调小是直播场景的典型优化——默认值是为点播文件设计的，用在直播流上会让"首次拿到流信息"的等待时间明显变长。

### 2.3 中断回调（超时机制的真正实现）

```cpp
// ARFFPlayer.cpp:115-118
static int custom_interrupt_callback(void *arg) {
    ARFFPlayer* splayer = (ARFFPlayer*)arg;
    return splayer->Timeout();   // 返回非 0，FFmpeg 会主动中断当前阻塞的 I/O 调用（如 avformat_open_input / av_read_frame）
}
```
这是 FFmpeg 提供的标准"从外部打断阻塞网络调用"机制——`AVFormatContext::interrupt_callback` 会被 FFmpeg 内部 I/O 层周期性调用，一旦返回非 0 就立即以 `AVERROR_EXIT` 返回，避免了自己起线程操作 socket 才能实现超时的麻烦。

### 2.4 查找并打开解码器（`open_codec_context`，视频/音频复用同一份逻辑）

```cpp
// ARFFPlayer.cpp:59-113
static int open_codec_context(int *stream_idx, AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type)
{
    int stream_index = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);  // 自动挑选"最佳"流
    AVStream* st = fmt_ctx->streams[stream_index];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    *dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(*dec_ctx, st->codecpar);   // 把 AVStream 的参数拷进独立的解码 context

    AVDictionary *opts = NULL;
    av_dict_set(&opts, "refcounted_frames", "1", 0);         // 用引用计数管理 AVFrame 内存，避免 decode 时被覆盖
    avcodec_open2(*dec_ctx, dec, &opts);
    *stream_idx = stream_index;
    return 0;
}
```

### 2.5 解码：现代 API 是活代码，旧 API 是 `#if 0` 保留对照

```cpp
// ARFFPlayer.cpp:600-619（音频）/ 696-712（视频，逻辑对称）
#if 0
    int ret = avcodec_decode_audio4(audio_dec_ctx_, avframe_, &frameFinished, &pkt);  // 旧 API，已弃用
#else
    int ret = avcodec_send_packet(audio_dec_ctx_, &pkt);
    if (ret >= 0) {
        ret = avcodec_receive_frame(audio_dec_ctx_, avframe_);
        frameFinished = (ret >= 0) ? 1 : 0;
    }
#endif
```
`avcodec_decode_audio4`/`avcodec_decode_video2` 是 FFmpeg 3.1 之前的老解码接口（一次调用完成"送包+取帧"，且不支持一个包解出多帧的场景），`avcodec_send_packet`/`avcodec_receive_frame`（3.1 起引入）是现在推荐的异步风格接口。这里用 `#if 0` 把旧写法整段保留在代码里作为对照，而不是删掉——是阅读这类跨版本维护代码时常见的"保留旧实现供切换/调试"的写法。

### 2.6 音频重采样（`libswresample`）

```cpp
// ARFFPlayer.cpp:990-998（初始化）
audio_convert_ctx_ = swr_alloc();
audio_convert_ctx_ = swr_alloc_set_opts(audio_convert_ctx_,
    av_get_default_channel_layout(n_out_channels_), AV_SAMPLE_FMT_S16, n_out_sample_hz_,   // 目标：立体声/S16/48000（固定输出格式）
    audio_dec_ctx_->channel_layout, audio_dec_ctx_->sample_fmt, audio_dec_ctx_->sample_rate, // 源：跟随实际解码出的流参数
    0, NULL);
swr_init(audio_convert_ctx_);
n_resmap_size_ = av_samples_get_buffer_size(NULL, av_get_channel_layout_nb_channels(...), frame_size, audio_dec_ctx_->sample_fmt, 1);
p_resamp_buffer_ = (uint8_t*)av_malloc(n_resmap_size_);

// OnBufferDecodeAudioData 里（:624-646）：源参数变化时重建 SwrContext
if (avframe_->sample_rate != n_sample_hz_ || avframe_->channels != n_channels_) {
    swr_free(&audio_convert_ctx_);
    audio_convert_ctx_ = swr_alloc_set_opts(...);  // 用新的源参数重新初始化
    swr_init(audio_convert_ctx_);
}
int samples = swr_convert(audio_convert_ctx_, &p_resamp_buffer_, n_resmap_size_,
    (const uint8_t **)avframe_->data, avframe_->nb_samples);
```
**要点**：把"统一转成固定输出格式（48000Hz/立体声/S16）"和"源格式可能中途变化（比如流重新协商）需要重建 SwrContext"这两个问题一起处理了——直播场景里源音频参数理论上是可能变的，这里做了防御。

### 2.7 时间戳换算（对照"难点和复杂"文档的 bug）

```cpp
// 正确写法示例——OnBufferDecodeVideoData (:686)，用的是 vstream_timebase_
int64_t pts = av_rescale_q(vid_packet->dts, vstream_timebase_, TIMEBASE_MS);

// 错误写法——ReadThreadProcess (:352,358)，视频分支却用了 astream_timebase_
dts = av_rescale_q(packet->dts, astream_timebase_, TIMEBASE_MS);  // bug，应为 vstream_timebase_
```
同一个 `av_rescale_q(int64_t ts, AVRational tb_src, AVRational tb_dst)` API，在文件的两处调用点，一处用对了 timebase，一处用错了——具体分析见 [难点和复杂.md](./ArLiveLite%20难点和复杂.md) 第 1 条。

### 2.8 收尾

```cpp
// ARFFPlayer.cpp:1014-1031 CloseFFDecode
avcodec_close(video_dec_ctx_);
avcodec_close(audio_dec_ctx_);
avformat_close_input(&fmt_ctx_);
```

---

## 3. 两侧对照小结

| 关注点 | 推流 `ArFFWriter` | 拉流 `ARFFPlayer` |
|---|---|---|
| 打开方式 | `avformat_alloc_output_context2` + 手动 `avio_open2` | `avformat_open_input` 一步到位（协议自动探测） |
| 编解码器角色 | 只用来**生成 extradata**，不真正编码画面 | 真正用来解码收到的画面/声音 |
| 解码/编码 API 版本 | 不涉及解码 | 新旧 API 并存（`#if 0` 保留旧接口对照） |
| 超时机制 | 靠 `av_dict_set` 的 `stimeout`/`timeout` 选项 | 额外加了 `interrupt_callback` 主动中断 |
| 时间戳特殊处理 | FLV 分支覆盖 rescale 结果（有意为之） | 存在一处 timebase 用错的 bug（无意） |
| 协议差异化配置 | 按 scheme 设置 `rtmp_tcurl`/`rtsp_transport`/`rtp_transport` 等 | 按 scheme 设置 `rtsp_transport`/`multiple_requests` 等 |
