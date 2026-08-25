# ArLiveLite 视频 PTS/DTS 时间戳详解

## 1. 基本概念

- **PTS (Presentation Timestamp)**：告诉播放器"这一帧什么时候显示"。
- **DTS (Decode Timestamp)**：告诉解码器"这一帧什么时候解码"。

对于**没有 B 帧**的编码（直播推流最常见，只用 I/P 帧），PTS 和 DTS 相等，因为解码顺序和显示顺序一致。只有存在 B 帧时，PTS 和 DTS 才会不同（B 帧需要参考后面的帧，解码顺序会被打乱）。

## 2. 视频编码时是否需要手动设置 pts/dts？

要看处于编码流程的哪一层：

### 场景 A：用 ffmpeg 自己的编码器（`avcodec_send_frame` / `avcodec_receive_packet`）

- **编码前（AVFrame）**：只需要设置 `frame->pts`，不需要设 dts。
- **编码后（AVPacket）**：编码器内部已经根据 GOP 结构（有没有 B 帧）自动算好了 `pkt->pts` 和 `pkt->dts`，不需要手动设置，直接用、rescale 后写文件/推流即可。

```c
frame->pts = pts_counter++;
avcodec_send_frame(enc_ctx, frame);

avcodec_receive_packet(enc_ctx, pkt);
// pkt->pts, pkt->dts 已经是对的，只需要 rescale 时间基
```

### 场景 B：已有编码好的裸流，只用 ffmpeg 做封装（mux）—— 本仓库的实际情况

视频是外部硬编码器（MediaCodec/VideoToolbox）编好的，直接拿到 H.264/H.265 字节流，**没有走 ffmpeg 的编码器**。这种情况下 **pts 和 dts 都必须自己手动设置**，因为没有编码器帮你算。

对照 [ArLiveLite/pusher/ARFFPusher.cpp:141-142](../../ArLiveLite/pusher/ARFFPusher.cpp)：

```cpp
int64_t pts = rtc::TimeUTCMillis() - n_push_time_;
ar_writer_->SetVideoEncData(pData, nLen, bKeyFrame, pts, pts);  // pts 和 dts 都手动填，直接相等
```

两者设成一样，是因为推流没用 B 帧（为了降延迟，直播场景几乎都不用 B 帧）。

### 总结表

| 情况 | pts | dts |
|---|---|---|
| ffmpeg 自己编码（有 B 帧） | 编码前你设 frame→pts | 编码器自动算，你不用管 |
| ffmpeg 自己编码（无 B 帧） | 编码前你设 frame→pts | 编码器自动算出 = pts |
| 只做 mux，外部已编码（无 B 帧，本仓库场景） | 必须自己设 | 必须自己设，直接等于 pts |
| 只做 mux，外部已编码（有 B 帧） | 必须自己设（显示顺序） | 必须自己设（解码/发送顺序，需自己维护） |

## 3. time_base 与 90000 的关系

在 ffmpeg（以及 RTP）里，`pts`/`dts` 本身不是时间，而是"时钟节拍数"（tick 计数）。换算成真实秒数的公式：

```
真实时间(秒) = pts值 × time_base.num / time_base.den
```

当 `time_base = {1, 90000}` 时：

```
真实时间(秒) = pts / 90000
```

例如 `pts = 90000` 代表第 1 秒的画面，`pts = 45000` 代表第 0.5 秒。

### 为什么是 90000

不是 ffmpeg 发明的，而是 **RFC 3550（RTP 标准）规定视频负载必须用 90kHz 时钟**，源头可追溯到 MPEG-2 标准。选 90000 是因为它能被几乎所有常见帧率整除，不产生小数：

```
90000 / 24  = 3750
90000 / 25  = 3600
90000 / 30  = 3000
90000 / 50  = 1800
90000 / 60  = 1500
```

如果用 1000（毫秒）做时间基，30fps 每帧间隔是 33.33ms，取整会有累积误差；用 90000 时每帧正好 3000 个 tick，没有舍入误差。这是行业统一选它做视频时间基的原因。

### 仓库中的印证

- [ArLiveLite/pusher/ArFFWriter.cpp:650](../../ArLiveLite/pusher/ArFFWriter.cpp) — `vid_stream_->time_base = AVRational{ 1, 90000 };`
- [ArLiveLite/include/webrtc/modules/rtp_rtcp/include/rtp_rtcp_defines.h:41](../../ArLiveLite/include/webrtc/modules/rtp_rtcp/include/rtp_rtcp_defines.h) — `const int kVideoPayloadTypeFrequency = 90000;`

## 4. 自己设置 pts：两种方法

### 方法一：固定步进法（强依赖 fps）

假设恒定帧率（CFR），每帧 pts 按固定间隔递增：

```
每帧 pts 增量 = time_base.den / fps = 90000 / fps
```

| fps | 每帧 pts 增量 |
|---|---|
| 25 | 3600 |
| 30 | 3000 |
| 60 | 1500 |

```c
int64_t pts_step = 90000 / fps;
pts = frame_index * pts_step;
frame_index++;
```

**特点**：pts 完全由帧号和 fps 决定，跟实际采集耗时无关。适合严格按固定帧率吐帧的场景。缺点是采集端偶尔丢帧/卡顿时，pts 和真实时间会逐渐漂移。

### 方法二：实际采集时间法（不依赖 fps，直播推流的主流做法）

不管 fps 是多少，直接拿"这一帧实际被采集到的时刻"换算成目标时间基的 tick：

```
pts = (当前采集时刻 - 推流起始时刻) 换算到目标 time_base
```

**特点**：能真实反映采集时的时间波动（VFR，变帧率），比如摄像头偶尔丢帧、编码耗时不均匀，pts 依然准确对应真实时间，不会累积漂移。这是直播推流场景的主流做法，因为实际采集间隔从来不是绝对均匀的。

### 硬性要求：pts 必须严格单调递增

不管哪种方式，pts（无 B 帧场景下等同于 dts）必须严格单调递增。如果两帧算出相同或变小的 pts，muxer 会报错甚至丢帧（常见错误如 `Application provided invalid, non monotonically increasing dts`）。

## 5. 结合 ffmpeg 的「实际采集时间法」伪代码

### 5.1 场景 A：用 ffmpeg 自己的编码器

```c
// ===== 初始化阶段 =====
int64_t start_time_us = -1;                  // 用微秒做采集时钟，精度更高
AVRational capture_tb = {1, 1000000};        // 采集时间戳的时间基：1/1000000（微秒）
enc_ctx->time_base = capture_tb;             // 编码器输入时间基设成和采集一致，最省事
AVRational out_tb = {1, 90000};              // 最终输出（比如RTMP流）想用的时间基
out_stream->time_base = out_tb;

// ===== 每帧采集回调 =====
void on_frame_captured(AVFrame* frame) {
    int64_t now_us = get_current_time_us();  // 系统时钟，比如 av_gettime()

    if (start_time_us < 0) {
        start_time_us = now_us;              // 第一帧作为时间原点
    }

    int64_t pts_us = now_us - start_time_us; // 相对时间戳（微秒）

    // 关键：保证严格单调递增，防止同一时刻多帧撞车
    if (pts_us <= last_frame_pts_us) {
        pts_us = last_frame_pts_us + 1;
    }
    last_frame_pts_us = pts_us;

    frame->pts = pts_us;   // 直接赋值，因为 frame 用的时间基就是微秒(capture_tb)

    // ===== 送入编码器 =====
    avcodec_send_frame(enc_ctx, frame);

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(enc_ctx, pkt) == 0) {
        // 编码器已经根据 GOP/B帧结构算好了 pkt->pts / pkt->dts（都在 enc_ctx->time_base 下）

        // 从编码器时间基 rescale 到输出流的时间基（1/90000）
        av_packet_rescale_ts(pkt, enc_ctx->time_base, out_stream->time_base);
        pkt->stream_index = out_stream->index;

        av_interleaved_write_frame(format_context, pkt);
        av_packet_unref(pkt);
    }
}
```

**要点**：编码器的 `time_base` 直接设成和采集时钟一致的单位（这里是微秒），`frame->pts` 赋值不需要任何换算，编码完拿到 `AVPacket` 后再统一 rescale 到目标输出时间基一次即可。

### 5.2 场景 B：已经硬编码好、只做封装（对应本仓库 `ArFFWriter` 的场景）

没有 `AVFrame`/`avcodec_send_frame` 这一步，直接拿已编码字节构造 `AVPacket`，pts/dts 全靠自己算：

```c
// ===== 初始化 =====
int64_t start_time_ms = -1;
AVRational src_tb = {1, 1000};       // 采集时间戳原始单位：毫秒
AVRational dst_tb = {1, 90000};      // 输出流声明的时间基
vid_stream->time_base = dst_tb;

int64_t last_pts_90k = -1;

// ===== 每次拿到硬编码器吐出的一帧数据时调用 =====
void on_encoded_video_data(uint8_t* data, int len, bool is_keyframe) {
    int64_t now_ms = get_current_time_ms();   // 例如 rtc::TimeUTCMillis()

    if (start_time_ms < 0) {
        start_time_ms = now_ms;
    }
    int64_t pts_ms = now_ms - start_time_ms;  // 相对毫秒时间戳

    // 换算到目标时间基 1/90000（用 av_rescale_q 而不是手动 ×90，更安全）
    int64_t pts_90k = av_rescale_q(pts_ms, src_tb, dst_tb);

    // 严格单调递增保护
    if (pts_90k <= last_pts_90k) {
        pts_90k = last_pts_90k + 1;
    }
    last_pts_90k = pts_90k;

    // 无 B 帧场景：dts 直接等于 pts
    int64_t dts_90k = pts_90k;

    // ===== 构造 AVPacket 并写入 =====
    AVPacket pkt = {0};
    av_new_packet(&pkt, len);
    memcpy(pkt.data, data, len);
    pkt.pts = pts_90k;
    pkt.dts = dts_90k;
    pkt.stream_index = vid_stream->index;
    if (is_keyframe) pkt.flags |= AV_PKT_FLAG_KEY;

    av_interleaved_write_frame(format_context, &pkt);
    av_packet_unref(&pkt);
}
```

**两种场景核心区别**：

- **A（ffmpeg 自编码）**：只需给 `frame->pts` 赋值（用编码器自己的 time_base），`AVPacket` 的 pts/dts 编码器自动算好，最后 rescale 一次即可。
- **B（外部已编码，只做 mux）**：`AVPacket` 的 pts 和 dts 都要手动算、手动 rescale、手动保证单调递增——没有编码器帮你兜底，更容易出错。

## 6. 本仓库需要重点排查的地方

### 6.1 FLV 分支绕过了 rescale

[ArLiveLite/pusher/ArFFWriter.cpp](../../ArLiveLite/pusher/ArFFWriter.cpp) 的 `SetVideoEncData`（约 386 行起）：

- 正常路径：`av_packet.pts = pts;` 后调用 `av_packet_rescale_ts()` 做时间基转换。
- 但对 FLV 格式专门做了特殊处理（约 416-417 行），绕过 rescale，直接把原始毫秒值塞回 `av_packet.pts/dts`：

```cpp
if (strcmp(format_context_->oformat->name, "flv") == 0) {
    av_packet.pts = pts;   // 未经 rescale 的原始毫秒值
    av_packet.dts = dts;
}
```

而 [ArFFWriter.cpp:650](../../ArLiveLite/pusher/ArFFWriter.cpp) 又把该输出流声明成 `time_base = {1, 90000}`。这里存在**声明时间基与实际填入值的单位不一致**的疑点：流声明是 90kHz tick，实际塞入的却是未经换算的毫秒数。是否是刻意针对 FLV 输出格式的绕过（FFmpeg 的 FLV muxer 内部对 tag timestamp 本身按毫秒处理），还是遗留的不一致 bug，建议实际抓包/打日志验证播放端时间戳是否正确。

### 6.2 独立的 CTS 计算逻辑未接入主链路

[ArLiveLite/rtmp/libflv/source/flv-muxer.c](../../ArLiveLite/rtmp/libflv/source/flv-muxer.c) 有一套独立的 FLV muxer 实现，正确计算了 composition time offset（用于支持 B 帧场景）：

- 第 283 行（`flv_muxer_h264`）：`video.cts = pts - dts;`
- 第 335 行（`flv_muxer_h265`）：`video.cts = pts - dts;`
- 第 266、318 行：序列头（sequence header）tag 设 `video.cts = 0`。

但仓库范围搜索显示，`flv_muxer_avc`/`flv_muxer_hevc`（这个文件的入口函数）**目前没有被实际推流路径（`ARFFPusher`/`ArFFWriter`）调用**——真正跑的推流链路走的是 libavformat 直接写 FLV，用的是 6.1 节中 pts==dts 的简化方案。这套带 CTS 计算的 muxer 属于当前未接入主链路的代码，如果未来要支持 B 帧编码提升压缩率，需要考虑是否启用这套逻辑。
