# ArLiveLite 学习计划

> 面向：已有 10 年 Android 开发经验、目标是吃透 C++ 层"编解码 + 音视频传输"的学习者。默认已了解 Java 层（org.webrtc / io.anyrtc.live），不再安排 Java 层学习。
> 配套文档：[技术架构](./ArLiveLite技术架构.md) · 
> [具体功能](./ArLiveLite具体功能.md) · 
> [C++技术点](./ArLiveLite用到的C++技术点.md) · 
> [难点和复杂](./ArLiveLite%20难点和复杂.md)

## 学习方式建议

这份代码是**真实编进 Android `.so` 的**（通过 `Prj-Android/liveplayer/src/main/cpp/CMakeLists.txt` 的 `ARLIVE_DIR` 引用），不是只能看头文件的预编译库，可以在 Android Studio 里对 `ArLiveLite/*.cc`/`*.cpp` 打断点单步调试（仓库里已有一次"修改编译配置，便于调试源码"的提交）。建议每个阶段都遵循：

1. **先读**：按顺序读指定文件，对照本计划标出的数据流走一遍代码
2. **再跑**：用 `Prj-Android/app` 的 `PushActivity`/`PullActivity` 实际跑起来，在关键函数打断点验证读代码时的理解
3. **再讲**：合上代码，口头或写下来复述一遍这一段的完整数据流（从输入到输出经过哪些函数/线程/回调），讲不出来说明没吃透
4. **可选：动手**：部分阶段配了具体的验证/修复任务，做完能确认自己真正理解了

---

## 阶段 0：环境与全局认知（0.5 天）

- 目标：搞清楚整体分层，避免在细节里迷路
- 阅读：[ArLiveLite技术架构.md](./ArLiveLite技术架构.md) 全文
- 关键认知（提前建立，避免走弯路）：
  - WebRTC 在本项目里只是"编解码器 + 线程库"，不做信令/传输
  - RTMP 传输走 FFmpeg avformat，不是仓库里另一套自研 `rtmp/` 目录
  - `Prj-Android/liveplayer/src/main/cpp/webrtc/` 是头文件壳，真正能读的实现在 `ArLiveLite/`
- 验证：跑通 `PushActivity` 推一路流到你自己搭的/公开的测试 RTMP 服务器，用 `ffplay`/VLC 拉流确认能看到画面

## 阶段 1：JNI 桥接层（1~1.5 天）

- 目标：理解 Java ↔ Native 数据/控制怎么跨界，这是后面所有阶段的地基
- 阅读顺序（按信息密度排序）：
  1. `Prj-Android/liveplayer/src/main/cpp/jni/util/ClassreferenceHolder.h/.cc` —— 先理解 `FindClass` 陷阱，这是所有内容的前提知识
  2. `Prj-Android/liveplayer/src/main/cpp/jni/LiveEngine.cpp`
     *  通读 `initWebRTC`、`makeNativeInstance`、`nativeCreatePushKit`/`nativeStartPush`
     * 这条创建/启动推流的调用链；重点精读 `:872-962` 的 `LibYuvBridge`（DirectByteBuffer 零拷贝范例）
  3. `Prj-Android/liveplayer/src/main/cpp/jni/liveEngine/ArLivePushEvent.cpp` + `NativePushObserver_JNI.h` —— 理解 native 线程回调 Java 的标准模型（全局引用 + AttachCurrentThreadIfNeeded + atomic jmethodID 缓存）
  4. `Prj-Android/liveplayer/src/main/cpp/jni/android/VideoCameraCapturer.cpp` + `AndroidContext.cpp` —— 对照第 2 步的句柄方向，理解"this 指针往返"的另一个方向
  5. `Prj-Android/liveplayer/src/main/cpp/jni/liveEngine/AndroidDeviceManager.cpp` —— 摄像头切换的"先销毁后重建"模式、`VideoTrackSourceProxy` 跨线程代理
- 对照材料：[ArLiveLite用到的C++技术点.md](./ArLiveLite用到的C++技术点.md) 第 1、2 节
- 验证：在 `LiveEngine.cpp` 的 `nativeStartPush` 和 `ArLivePushEvent::onPushStatusUpdate` 上各打一个断点，触发一次推流，确认自己能画出"Java 调 native 启动 → native 回调 Java 上报状态"的完整调用栈

## 阶段 2：视频编码管线（1.5 天）

- 目标：吃透"摄像头 YUV → H264 NALU"这条最核心的链路
- 阅读：`ArLiveLite/codec/AvCodec.h` + `AvCodec.cc` 的 `V_H264Encoder` 部分（`:208-559`）
- 重点关注：
  - 独立编码线程与采集线程的解耦（`AddToFrameList` vs `Run()`）
  - 缩放三模式 + 镜像的实现（`libyuv` 用法）
  - 编码器创建策略（硬编码优先、软编码兜底）与关键帧节奏控制
- 对照材料：[具体功能.md](./ArLiveLite具体功能.md) 第 3 节、[难点和复杂.md](./ArLiveLite%20难点和复杂.md) 第 12、15 条
- 验证：在 `V_H264Encoder::NewVideoEncoder` 打断点，确认当前 Android 路径实际走的是软编码还是硬编码（对照第 15 条的判断），这是一个可以直接用调试器验证"文档结论对不对"的练习

## 阶段 3：音频编码管线（0.5 天）

- 目标：理解与视频编码完全不同的"固定帧长编解码器"适配方式
- 阅读：`ArLiveLite/codec/AvCodec.cc` 的 `A_AACEncoder` 部分（`:1-79`）+ `ArLiveLite/codec/aacencode.cc`
- 重点关注：10ms PCM 输入如何被适配成 1024-sample 的 AAC 帧
- 验证：给 `aacencode.cc` 里的累积缓冲区逻辑加一行日志，打印每次实际喂给 `faacEncEncode` 的样本数，跑一次推流确认输出稳定在预期帧长

## 阶段 4：推流传输——本计划里价值最高的一段（2 天）

- 目标：吃透 FFmpeg avformat 是怎么把编码后的 NALU/AAC 变成 RTMP 包发出去的，以及两个反直觉的实现细节
- 阅读顺序：
  1. `ArLiveLite/pusher/ARFFPusher.cpp` —— 状态机、关键帧门控逻辑
  2. `ArLiveLite/pusher/ArFFWriter.cpp`（822 行，建议分两次读完）：
     - 第一遍：`Connect()` 整体流程，搞清 `avformat_alloc_output_context2` → 加 `AVStream` → `avio_open2` → `avformat_write_header` 的顺序
     - 第二遍：精读 [难点和复杂.md](./ArLiveLite%20难点和复杂.md) 第 2、3、4 条对应的代码位置（FLV 时间戳覆盖、影子编码器造 extradata、`av_init_packet` 坑）
- 对照材料：[C++技术点.md](./ArLiveLite用到的C++技术点.md) 第 4 节 FFmpeg 部分
- **动手任务（推荐）**：
  1. 用 Wireshark 或 `ffmpeg -loglevel debug` 抓一次推流的 RTMP 握手包，对照代码里 `tc_url`/`flashver` 的设置，验证自己对协议层的理解
  2. 尝试回答：如果要把 `ArFFWriter` 改造成支持 mpegts 输出，第 2 条里"FLV 专属的时间戳覆盖逻辑"该怎么改？（不用真的实现，能说清楚思路即可）

## 阶段 5：拉流/播放管线（1.5~2 天）

- 目标：理解一个直播播放器典型的三级缓冲（网络到达 → 解码节拍 → 渲染节拍）设计
- 阅读顺序：
  1. `ArLiveLite/player/ARFFPlayer.cpp` —— 读线程 + pts/dts 计算（**重点看这里，见下方动手任务**）
  2. `ArLiveLite/player/FFBuffer.cpp` —— 两级缓冲 + 自适应缓存增长 + 跳变检测
  3. `ArLiveLite/PlayBuffer.cpp` —— 第三级渲染/播放节奏，音频优先的丢帧同步策略
- 对照材料：[具体功能.md](./ArLiveLite具体功能.md) 第 7 节
- **动手任务（强烈推荐，本计划性价比最高的练习）**：
  1. 复现 [难点和复杂.md](./ArLiveLite%20难点和复杂.md) 第 1 条的 timebase bug——找一路视频/音频 timebase 明显不同的流（比如摄像头推流常见视频 timebase 与标准音频采样率不同），验证是否真的出现音画不同步
  2. 修复这一行 bug（`astream_timebase_` → `vstream_timebase_`），重新编译验证现象是否消失
  3. 这是一次完整的"读代码 → 发现 bug → 复现 → 修复 → 验证"闭环，比单纯读代码收获大得多

## 阶段 6：SEI 自定义消息全链路（0.5 天）

- 目标：完整走一遍"业务自定义信令"是怎么用 H264 SEI 藏在视频码流里传输的
- 阅读：`ArLiveLite/H264SeiPack.cpp`（推流侧打包）→ `ArLive2Pusher.cpp` 里 SEI 排队与关键帧等待逻辑 → `ARFFPlayer::ParseVideoSei` + `ArLive2Player::OnArPlySeiData`（播放侧解析）
- 对照材料：[难点和复杂.md](./ArLiveLite%20难点和复杂.md) 第 13 条（一处被注释掉的 return，标记了未修复的花屏 bug）
- 验证：用 `LiveEngine.cpp` 的 `nativeSendSeiMessage` 发一条自定义消息，在播放端断点确认能收到，并尝试理解第 13 条提到的潜在问题分支

## 阶段 7：查漏补缺（0.5~1 天，按兴趣选做）

通读 [难点和复杂.md](./ArLiveLite%20难点和复杂.md) 剩余条目，挑 1~2 个自己验证：
- 变速播放（第 6 条）：确认 `ARFFPlayer::SetSpeed` 确实是空实现
- 孤儿网络层（第 8 条）：全局搜索确认 `ArNetClient`/`ArNetTcpClient` 真的没有调用点
- 渲染旋转/填充（第 5 条）：跑一次真机验证画面旋转到底是靠什么机制生效的（既然 `MgrRender::SetRotation` 是空的）

## 时间预估汇总

| 阶段 | 内容 | 预估时间 |
|---|---|---|
| 0 | 全局架构认知 | 0.5 天 |
| 1 | JNI 桥接层 | 1~1.5 天 |
| 2 | 视频编码 | 1.5 天 |
| 3 | 音频编码 | 0.5 天 |
| 4 | 推流传输（FFmpeg RTMP） | 2 天 |
| 5 | 拉流播放（三级缓冲） | 1.5~2 天 |
| 6 | SEI 自定义消息 | 0.5 天 |
| 7 | 查漏补缺 | 0.5~1 天 |
| **合计** | | **约 8~10 天**（按每天 3~4 小时投入计） |

## 学完之后能达到的水平

- 能独立画出推流/拉流的完整数据流图（跨 Java/JNI/Native/FFmpeg 四层）
- 能看懂并评估任意一处 FFmpeg avformat 相关代码的正确性（时间戳、extradata、muxer 生命周期）
- 能识别并修复至少一个真实的时间戳类 bug（阶段 5 的动手任务）
- 对 JNI 互操作的几种核心模式（句柄传递、全局引用生命周期、零拷贝、方法 ID 缓存）有可迁移到其他项目的实操经验
