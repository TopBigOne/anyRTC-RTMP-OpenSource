# ArLiveLite 用到的 OpenGL 技术点

本文梳理 anyRTC-RTMP-OpenSource（ArLiveLite / Prj-Android）代码库中所有涉及 OpenGL（OpenGL ES / EGL）的代码位置，按模块整理，标注文件路径、核心类和用途，方便后续阅读和维护。

## 0. 结论先行

- SDK 中**真正可读、可维护的 OpenGL 实现代码全部在 Android Java 层**，位于 `Prj-Android/liveplayer/src/main/java/org/webrtc` 包下（从标准 WebRTC Android SDK 移植而来），以及其子包 `org.webrtc.effector`（美颜/滤镜效果器）。
- **native C++ 侧（ArLiveLite、third_lib、third_party）没有直接调用 GL API 的源码**。WebRTC 原生渲染部分以预编译库形式存在（头文件可见、实现不可见），native 层只是通过 JNI 反射拿 Java 层的共享 `EGLContext` 传给硬件编解码器使用。这与之前梳理的「ArLiveLite core vs WebRTC headers：源码 vs 头文件预编译库」的结论一致。
- **iOS 侧**（`Prj-iOS/WebRTC.framework/Headers/RTCEAGLVideoView.h`）同样只是预编译 framework 的头文件声明（基于 `EAGLContext`/`OpenGLES`），没有可读实现。

整条 OpenGL 使用链路可以概括为：

```
Camera/屏幕采集（SurfaceTexture, OES 纹理）
        │
        ▼
Shader 处理（美颜/滤镜, GPUImage 风格）
        │
        ├─► 直接渲染到屏幕（EglRenderer + SurfaceView/TextureView）
        │
        └─► YUV 转换（YuvConverter）─► 硬件/软件编码器
```

---

## 1. EGL 上下文管理

负责创建/管理 EGL Display、Context、Surface，是所有 GL 操作的基础。

| 文件 | 说明 |
|---|---|
| [EglBase.java](../liveplayer/src/main/java/org/webrtc/EglBase.java) | EGL 能力的抽象接口（`Context`/`create`/`createSurface`/`makeCurrent` 等），268 行 |
| [EglBase10.java](../liveplayer/src/main/java/org/webrtc/EglBase10.java) / [EglBase10Impl.java](../liveplayer/src/main/java/org/webrtc/EglBase10Impl.java) | 基于 `javax.microedition.khronos.egl`（EGL 1.0 API）的实现，兼容低版本 Android |
| [EglBase14.java](../liveplayer/src/main/java/org/webrtc/EglBase14.java) / [EglBase14Impl.java](../liveplayer/src/main/java/org/webrtc/EglBase14Impl.java) | 基于 `android.opengl.EGL14`（API 17+）的实现，支持 OpenGL ES 3.0 及共享上下文 |

**与硬件编解码器的联动**：[HardwareVideoEncoder.java](../liveplayer/src/main/java/org/webrtc/HardwareVideoEncoder.java) 等硬件编解码相关类需要与渲染共用同一个 `EGLContext`（零拷贝，避免纹理在 GPU/CPU 间搬运）。native 层的 [AndroidDeviceManager.cpp](../liveplayer/src/main/cpp/jni/liveEngine/AndroidDeviceManager.cpp) 通过 JNI 反射调用 Java 层 `VideoCapturerDevice` 的 `getSharedEGLContext()` 方法拿到这个共享上下文（第 106-146 行），再交给编码器/解码器做硬件加速渲染共享。这是 native 侧唯一与 GL 相关的代码，本身不直接调用任何 GL API。

---

## 2. Shader 编译与通用绘制

| 文件 | 说明 |
|---|---|
| [GlShader.java](../liveplayer/src/main/java/org/webrtc/GlShader.java) | 封装顶点/片元 shader 的编译（`glCreateShader`/`glCompileShader`）与程序链接（`glLinkProgram`），134 行 |
| [GlGenericDrawer.java](../liveplayer/src/main/java/org/webrtc/GlGenericDrawer.java) | 通用纹理绘制器，内置两套 shader：处理 Camera 直出的 `GL_TEXTURE_EXTERNAL_OES` 纹理，和普通 `GL_TEXTURE_2D` 纹理，460 行 |
| [GlUtil.java](../liveplayer/src/main/java/org/webrtc/GlUtil.java) | GL 错误检查（`checkNoGLES2Error`）、纹理创建等工具方法 |
| [GlTextureFrameBuffer.java](../liveplayer/src/main/java/org/webrtc/GlTextureFrameBuffer.java) | 离屏渲染用的 FBO（Framebuffer Object）封装 |

---

## 3. 视频帧渲染管线（渲染到屏幕）

| 文件 | 说明 |
|---|---|
| [EglRenderer.java](../liveplayer/src/main/java/org/webrtc/EglRenderer.java) | 核心渲染类，799 行。独立渲染线程 + `Handler`，管理 EGL Surface 生命周期、按需渲染/帧率控制、渲染统计、截图（`addFrameListener`）等 |
| [VideoFrameDrawer.java](../liveplayer/src/main/java/org/webrtc/VideoFrameDrawer.java) | 把 `VideoFrame`（不管是 YUV 数据还是纹理）按缩放/裁剪参数画到当前 GL 上下文对应的 Surface 上 |
| [SurfaceViewRenderer.java](../liveplayer/src/main/java/org/webrtc/SurfaceViewRenderer.java) | 基于 `SurfaceView` 承载渲染输出的 View |
| [TextureViewRenderer.java](../liveplayer/src/main/java/org/webrtc/TextureViewRenderer.java) | 基于 `TextureView` 承载渲染输出的 View（可与其他 View 做混合叠加/动画） |

---

## 4. 相机/屏幕采集纹理

| 文件 | 说明 |
|---|---|
| [SurfaceTextureHelper.java](../liveplayer/src/main/java/org/webrtc/SurfaceTextureHelper.java) | 创建 `SurfaceTexture` 并配一条独立 GL 线程，接收 Camera（或屏幕采集）输出的 OES 外部纹理，404 行 |
| [VideoCapturerDevice.java](../liveplayer/src/main/java/io/anyrtc/live/internal/VideoCapturerDevice.java) | 对外暴露 `getSharedEGLContext()`，供 native 层通过 JNI 反射获取共享 EGLContext |

---

## 5. YUV ↔ RGB 色彩转换

| 文件 | 说明 |
|---|---|
| [YuvConverter.java](../liveplayer/src/main/java/org/webrtc/YuvConverter.java) | 用 shader + FBO 把纹理转换成 I420 YUV 字节流供编码器使用，避免走 CPU 做颜色空间转换，248 行 |
| [YuvByteBufferReader.java](../liveplayer/src/main/java/org/webrtc/effector/format/YuvByteBufferReader.java) | 从纹理/Buffer 读取 YUV 数据（效果器管线内部使用） |
| [YuvByteBufferDumper.java](../liveplayer/src/main/java/org/webrtc/effector/format/YuvByteBufferDumper.java) | 把 YUV 数据落盘，调试/排查画质问题用 |

---

## 6. 美颜 / 滤镜（GPUImage 风格效果器）

`org.webrtc.effector` 子包实现了一套类似 GPUImage 的滤镜链，在采集之后、编码/渲染之前对纹理做 shader 级处理。

| 文件 | 说明 |
|---|---|
| [OpenGlUtils.java](../liveplayer/src/main/java/org/webrtc/effector/OpenGlUtils.java) | 效果器专用的 GL 工具方法（纹理/FBO 创建等） |
| [GPUImageFilter.java](../liveplayer/src/main/java/org/webrtc/effector/filter/GPUImageFilter.java) | 滤镜基类，封装标准的「绑定纹理 → 绑定 shader → 绘制」流程 |
| [GPUImageFilterWrapper.java](../liveplayer/src/main/java/org/webrtc/effector/filter/GPUImageFilterWrapper.java) | 滤镜包装类，便于滤镜的组合/替换 |
| [GPUImageBeautyFilter.java](../liveplayer/src/main/java/org/webrtc/effector/filter/GPUImageBeautyFilter.java) | 美颜滤镜实现（磨皮/美白类效果，基于 shader），186 行 |
| [MediaEffectFilter.java](../liveplayer/src/main/java/org/webrtc/effector/filter/MediaEffectFilter.java) | 效果链整合入口，串联多个滤镜 |

---

## 7. 关联文档

- [ArLiveLite用到的C++技术点.md](ArLiveLite用到的C++技术点.md)
- [ArLiveLite技术架构.md](ArLiveLite技术架构.md)
