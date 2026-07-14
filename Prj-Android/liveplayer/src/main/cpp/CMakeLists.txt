# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.
# 声明本工程要求的最低 CMake 版本为 3.10.2，低于该版本的 CMake 可能无法解析后续用到的语法/命令
cmake_minimum_required(VERSION 3.10.2)
# 定义 CMake 工程名为 "anyLive"（工程级别的名字，与下面 add_library 生成的目标 anyLive 对应）
project("anyLive")
# 启用汇编语言（ASM）支持：webrtc/ffmpeg 等预编译依赖或本工程可能包含 .s/.asm 汇编源文件，需要 CMake 识别并能调用汇编器编译
enable_language(ASM)
# 把汇编编译器直接设为当前使用的 C 编译器（Android NDK 的 clang 同时具备编译 C 与汇编的能力，无需单独指定 as）
set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})
# 定义变量 STATIC_LIB_DIR：指向预编译库存放目录（liveplayer/src/main/cpp/lib/），README 中说明的、需要手动下载解压 webrtc/ffmpeg 库放入的目录
set(STATIC_LIB_DIR ${CMAKE_SOURCE_DIR}/lib/)
# 定义变量 ROOT_DIR：通过相对路径向上回溯 5 层，指向整个仓库的根目录（anyRTC-RTMP-OpenSource），便于引用同级的 third_party、VideoRender 等模块
set(ROOT_DIR ./../../../../../)
# 定义变量 ARLIVE_DIR：指向仓库根目录下的 ArLiveLite（Android/iOS/Windows 三端共用的引擎核心 C++ 源码目录）；行尾 #publish 标记这是"发布/正式"场景使用的路径配置
set(ARLIVE_DIR ./../../../../../ArLiveLite)#publish
# 被注释掉、未启用的备用配置：调试场景下可以改为引用 CMakeLists.txt 同级目录下自带的一份 ArLiveLite（例如本地临时拷贝，便于单独调试引擎代码）
#set(ARLIVE_DIR ./ArLiveLite)#debug
# 以下这一整段（本行到第 21 行）在语法上是同一条 set() 命令里的一个多行双引号字符串字面量，
# 用于追加设置 CMAKE_SHARED_LINKER_FLAGS（共享库链接参数）。
# 因为整段内容都被包在同一对双引号内，属于字符串字面量的一部分，中间任何一行都不能插入 "#" 行注释
# （"#" 在引号内没有注释语义，插入后会被当成链接参数字符串的一部分，导致链接命令被破坏），
# 因此这里只做整体说明，不在 13~21 行内部逐行加注：
#   -Wl,--gc-sections      链接时裁剪掉未被引用的函数/数据段，减小最终 so 体积；
#   -Wl,--exclude-libs,<列表>  声明后面列出的这些库（ffmpeg 各个 .so 以及 libwebrtc.a）中的符号
#                              不导出到最终产物的动态符号表，避免与宿主 APK 内其他模块用到的
#                              同名第三方库符号发生冲突（符号可见性隔离）。
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--exclude-libs,
${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libswscale.so,
${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libavformat.so,
${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libavcodec.so,
${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libavfilter.so,
${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libavutil.so,
${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libswresample.so,
${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libpostproc.so,
${STATIC_LIB_DIR}/webrtc/${ANDROID_ABI}/libwebrtc.a")

# 定义变量 NATIVE_LIB，值为 "anyLive"：后续 add_library/target_link_libraries 都通过这个变量引用最终产物目标名，
# 避免在多处硬编码重复的字符串（最终会生成 libanyLive.so）
set(NATIVE_LIB "anyLive")

# 声明一个名为 avutil 的"导入库"目标：SHARED 表示它是共享库，IMPORTED 表示这不是本工程要编译的源码，
# 而是一份已经编译好的现成库，只需要告诉 CMake 它在哪里、供后面链接使用
add_library(avutil SHARED IMPORTED)
# 为 avutil 这个导入库目标设置 IMPORTED_LOCATION 属性：指定其真实的 .so 文件路径（按 ANDROID_ABI 区分目录，如 arm64-v8a/armeabi-v7a）
set_target_properties(avutil PROPERTIES IMPORTED_LOCATION ${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libavutil.so)

# 声明导入库 avformat（ffmpeg 封装格式/解协议模块）
add_library(avformat SHARED IMPORTED)
# 指定 avformat 对应的预编译 .so 实际路径
set_target_properties(avformat PROPERTIES IMPORTED_LOCATION ${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libavformat.so)

# 声明导入库 avcodec（ffmpeg 编解码核心模块）
add_library(avcodec SHARED IMPORTED)
# 指定 avcodec 对应的预编译 .so 实际路径
set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION ${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libavcodec.so)

# 声明导入库 avfilter（ffmpeg 滤镜处理模块）
add_library(avfilter SHARED IMPORTED)
# 指定 avfilter 对应的预编译 .so 实际路径
set_target_properties(avfilter PROPERTIES IMPORTED_LOCATION ${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libavfilter.so)

# 声明导入库 swresample（ffmpeg 音频重采样模块）
add_library(swresample SHARED IMPORTED)
# 指定 swresample 对应的预编译 .so 实际路径
set_target_properties(swresample PROPERTIES IMPORTED_LOCATION ${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libswresample.so)

# 声明导入库 swscale（ffmpeg 图像缩放/像素格式转换模块）
add_library(swscale SHARED IMPORTED)
# 指定 swscale 对应的预编译 .so 实际路径
set_target_properties(swscale PROPERTIES IMPORTED_LOCATION ${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libswscale.so)

# 声明导入库 postproc（ffmpeg 后处理模块，如去噪、去块效应等画面后处理滤镜）
add_library(postproc SHARED IMPORTED)
# 指定 postproc 对应的预编译 .so 实际路径
set_target_properties(postproc PROPERTIES IMPORTED_LOCATION ${STATIC_LIB_DIR}/ffmpeg/${ANDROID_ABI}/libpostproc.so)

# 声明导入库 webrtc：注意类型是 STATIC（静态库 .a），而不是上面几个 ffmpeg 库使用的 SHARED
add_library(webrtc STATIC IMPORTED)
# 指定 webrtc 对应的预编译静态库 libwebrtc.a 的实际路径
set_target_properties(webrtc PROPERTIES IMPORTED_LOCATION ${STATIC_LIB_DIR}/webrtc/${ANDROID_ABI}/libwebrtc.a)

# 引入第三方 faac（AAC 音频编码库）的子工程 CMakeLists.txt：会把 faac 的源码编译规则并入本次构建，
# 并对外提供一个可供 target_link_libraries 引用的 faac 目标
include(${ROOT_DIR}/third_party/faac-1.28/CMakeLists.txt)
# 开始定义本工程最终要生成的原生库目标
add_library(
        # 目标名：引用前面定义的变量 NATIVE_LIB（即 "anyLive"），最终产物为 libanyLive.so
        ${NATIVE_LIB}
        # 库类型：SHARED 表示生成动态链接库（.so），供 Java 层通过 System.loadLibrary 加载
        SHARED
        # ------ 以下为参与编译的源文件列表 ------
        # Prj-Android 自身 jni 层源码：JNI 导出函数总入口（Java_io_anyrtc_live_internal_NativeInstance_* 等）
        ${CMAKE_SOURCE_DIR}/jni/LiveEngine.cpp
        # Prj-Android 自身 jni 层源码：共享的 media/worker 线程池管理
        ${CMAKE_SOURCE_DIR}/jni/StaticThreads.cpp
        # Prj-Android 自身 jni 层源码：封装持有 Java 端 VideoCapturerDevice 的平台上下文
        ${CMAKE_SOURCE_DIR}/jni/android/AndroidContext.cpp
        # Prj-Android 自身 jni 层源码：摄像头/屏幕采集会话的 JNI 封装
        ${CMAKE_SOURCE_DIR}/jni/android/VideoCameraCapturer.cpp
        # Prj-Android 自身 jni 层源码：播放回调事件转发到 Java 层
        ${CMAKE_SOURCE_DIR}/jni/liveEngine/ArLivePlayEvent.cpp
        # Prj-Android 自身 jni 层源码：推流回调事件转发到 Java 层
        ${CMAKE_SOURCE_DIR}/jni/liveEngine/ArLivePushEvent.cpp
        # Prj-Android 自身 jni 层源码：摄像头/美颜/编解码器工厂等设备能力管理
        ${CMAKE_SOURCE_DIR}/jni/liveEngine/AndroidDeviceManager.cpp

        # ------ 以下为 ArLiveLite（跨平台引擎核心）目录下参与编译的源文件 ------
        # Android 端渲染实现（把解码后的视频帧渲染到 Surface/纹理上）
       ${ARLIVE_DIR}/AndroidRenderer.cpp
       # 引擎主类实现：对应 IArLive2Engine 接口，负责创建/释放推流器与播放器
       ${ARLIVE_DIR}/ArLive2Engine.cpp
       # 平台相关能力的具体实现（不同平台差异化逻辑的落地实现）
       ${ARLIVE_DIR}/PlatformImpl.cpp
       # 播放器实现：对应 IArLivePlayer 接口，负责拉流播放的具体业务逻辑
       ${ARLIVE_DIR}/ArLive2Player.cpp
       # 推流器实现：对应 IArLivePusher 接口，负责推流采集/编码/发送的具体业务逻辑
       ${ARLIVE_DIR}/ArLive2Pusher.cpp
       # AAC 编码封装（对接 faac 库）
       ${ARLIVE_DIR}/codec/aacencode.cc
       # 音视频编解码器封装/调度
       ${ARLIVE_DIR}/codec/AvCodec.cc
       # 渲染管理器（管理本地预览/远端画面等多路渲染目标）
       ${ARLIVE_DIR}/MgrRender.cpp
       # 播放缓冲区实现（拉流时的音视频数据缓存/同步）
       ${ARLIVE_DIR}/PlayBuffer.cpp
       # 基于 ffmpeg 的拉流播放器具体实现（解封装/解码/音视频同步）
       ${ARLIVE_DIR}/player/ARFFPlayer.cpp
       # 拉流侧数据缓冲区辅助实现
       ${ARLIVE_DIR}/player/FFBuffer.cpp
       # sonic 变速不变调算法（用于播放端音频加速/减速处理），注意这是 .c 而非 .cpp
       ${ARLIVE_DIR}/player/sonic.c
       # 基于 ffmpeg 的推流采集/编码器具体实现
       ${ARLIVE_DIR}/pusher/ARFFPusher.cpp
       # RTMP/FLV 推流封装写入器（组装并写出 FLV Tag、推送到 RTMP 服务器）
       ${ARLIVE_DIR}/pusher/ArFFWriter.cpp
       # 通用网络客户端封装（网络连接/收发的基础抽象）
       ${ARLIVE_DIR}/ArNetClient.cpp
       # 基于 TCP 的网络客户端具体实现
       ${ARLIVE_DIR}/ArNetTcpClient.cpp
       # 心跳/定时轮询时钟工具，供推流器、播放器等做周期性任务调度
       ${ARLIVE_DIR}/RtcTick.cpp
       # H264 SEI（补充增强信息）打包/解析工具，用于自定义消息（如弹幕、时间戳）随视频流传输
       ${ARLIVE_DIR}/H264SeiPack.cpp
       # Prj-Android 自身 jni 层源码：预加载/缓存常用 Java 类的全局引用（WebRTC 风格 JNI 辅助类）
       ${CMAKE_SOURCE_DIR}/jni/util/ClassreferenceHolder.cc

# 结束 add_library 的源文件列表
)

# 调用 find_library 查找系统/NDK 提供的库
find_library( # Sets the name of the path variable.
        # 本次查找结果要保存到的 CMake 变量名：log-lib（后面 target_link_libraries 里通过 ${log-lib} 引用）
        log-lib

        # Specifies the name of the NDK library that
        # you want CMake to locate.
        # 要查找的实际系统库名："log"，即 Android NDK 提供的 liblog.so（对应 <android/log.h> 里的 __android_log_print 等日志接口）
        log)

# 为主目标 anyLive 配置需要链接的依赖库
target_link_libraries(
        # 链接目标：本工程生成的 libanyLive.so
        ${NATIVE_LIB}
        # 链接 faac AAC 编码库（来自第 49 行 include 进来的第三方子工程目标）
        faac
        # 链接器分组开始标记：--start-group 与 --end-group 之间的库如果存在互相的循环符号依赖，
        # 链接器会反复扫描这一组库直到所有符号都解析完成（静态库之间常见的相互引用场景）
        -Wl,--start-group
        # 以下为 ffmpeg 各功能模块库，按需参与上面这组循环依赖解析
        swscale
        avformat
        avcodec
        avfilter
        swresample
        postproc
        avutil
        # 链接器分组结束标记
        -Wl,--end-group
        # 之后的静态库启用"整体链接"：即使库中某些目标文件的符号当前没有被直接引用，也会被完整保留到最终产物中，
        # 常用于避免链接器把 WebRTC 内部依赖全局静态注册/构造函数的代码误裁剪掉
        -Wl,--whole-archive
        # 对 libwebrtc.a 启用整体链接
        webrtc
        # 关闭"整体链接"模式，恢复默认的按需链接（只对上面紧邻的 webrtc 生效）
        -Wl,--no-whole-archive
        # 链接 Android NDK 提供的 OpenSL ES 音频库，用于底层音频播放/录制
        OpenSLES
        # 链接前面 find_library 找到的系统日志库 liblog.so
        ${log-lib}
)



# 添加头文件搜索路径，供 #include 时查找对应目录

# cpp 根目录本身（liveplayer/src/main/cpp），一般用于以相对路径引用自身模块内的头文件
include_directories(${CMAKE_SOURCE_DIR})
# jni/ 目录：LiveEngine.cpp、StaticThreads.h 等 JNI 入口相关头文件
include_directories(${CMAKE_SOURCE_DIR}/jni/)
# jni/util/ 目录：BitmapUtil.h、ClassreferenceHolder.h 等工具类头文件
include_directories(${CMAKE_SOURCE_DIR}/jni/util/)
# jni/android/ 目录：AndroidContext.h、VideoCameraCapturer.h 等 Android 平台适配头文件
include_directories(${CMAKE_SOURCE_DIR}/jni/android/)
# webrtc/ 目录：vendored 精简版 libwebrtc 源码树的头文件根目录
include_directories(${CMAKE_SOURCE_DIR}/webrtc)
# webrtc/sdk/ 目录：WebRTC 对外 SDK 层头文件（如 native_api、android 相关封装）
include_directories(${CMAKE_SOURCE_DIR}/webrtc/sdk/)
# webrtc/sdk/android/src/jni/ 目录：WebRTC 内部 JNI 辅助头文件（如 jni_helpers.h、jvm.h 等）
include_directories(${CMAKE_SOURCE_DIR}/webrtc/sdk/android/src/jni/)
# webrtc/third_party/abseil-cpp/ 目录：WebRTC 依赖的 Google Abseil 基础库头文件
include_directories(${CMAKE_SOURCE_DIR}/webrtc/third_party/abseil-cpp/)
# lib/ffmpeg/include/ 目录：预编译 ffmpeg 库对应的公开头文件
include_directories(${CMAKE_SOURCE_DIR}/lib/ffmpeg/include/)
# ArLiveLite 引擎核心源码根目录本身，供其内部源码之间以相对路径 #include 彼此
include_directories(${ARLIVE_DIR})
# ArLiveLite/include/ 目录：引擎对外接口头文件（IArLivePusher.hpp、IArLivePlayer.hpp、ArLiveDef.hpp 等）
include_directories(${ARLIVE_DIR}/include/)
# ArLiveLite/codec/ 目录：音视频编解码相关头文件
include_directories(${ARLIVE_DIR}/codec/)
# ArLiveLite/player/ 目录：拉流播放相关头文件
include_directories(${ARLIVE_DIR}/player/)
# ArLiveLite/pusher/ 目录：推流相关头文件
include_directories(${ARLIVE_DIR}/pusher/)
# 仓库根目录下的 VideoRender/ 模块头文件（跨平台视频渲染相关）
include_directories(${ROOT_DIR}/VideoRender/)
# 仓库根目录下的 third_party/ 总目录（各类第三方依赖的父目录）
include_directories(${ROOT_DIR}/third_party/)
# third_party/libyuv/include/ 目录：libyuv 图像格式转换库头文件（I420/ABGR 等颜色空间转换）
include_directories(${ROOT_DIR}/third_party/libyuv/include/)
# third_party/rapidjson/ 目录：rapidjson 头文件库（用于 JSON 解析/生成，纯头文件库无需编译源文件）
include_directories(${ROOT_DIR}/third_party/rapidjson/)




# 定义预处理宏 WEBRTC_POSIX：WebRTC 源码中大量使用该宏区分 POSIX 系（Linux/Android/iOS/Mac）与 Windows 平台分支代码
add_definitions("-DWEBRTC_POSIX")
# 定义预处理宏 WEBRTC_ANDROID：在 POSIX 基础上进一步细分到 Android 平台专属的代码分支
add_definitions("-DWEBRTC_ANDROID")
# 定义预处理宏 WEBRTC_USE_H264：启用 WebRTC 内部对 H264 编解码器的支持代码路径
add_definitions("-DWEBRTC_USE_H264")
# 定义预处理宏 NDEBUG：关闭标准库 assert 断言，通常用于 Release 构建以获得更好性能，同时会影响部分库中 DCHECK 的行为
add_definitions("-DNDEBUG")
# 定义 WebRTC 内部构建标志宏 BUILDFLAG_INTERNAL_EXPENSIVE_DCHECKS_ARE_ON：开启"高开销 DCHECK"相关代码路径的编译
# （具体影响需要结合 WebRTC 源码中该宏的实际使用位置来看）
add_definitions("-DBUILDFLAG_INTERNAL_EXPENSIVE_DCHECKS_ARE_ON")
# 定义预处理宏 RW_LOCK：供工程内部条件编译选择读写锁相关实现（具体见使用该宏的源码位置）
add_definitions("-DRW_LOCK")
