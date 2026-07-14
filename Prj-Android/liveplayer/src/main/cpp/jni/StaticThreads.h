#pragma once

// cstddef: 提供 size_t 类型，用于线程池大小等无符号长度参数。
#include <cstddef>
// memory: 提供 std::shared_ptr / std::unique_ptr 等智能指针，
// 本文件用 shared_ptr 来做“借出的 Threads 对象”与线程池之间的引用计数管理。
#include <memory>

namespace rtc {
    // 前向声明，避免在头文件里包含 rtc_base/thread.h 的完整定义，减少编译依赖。
    class Thread;

    // 智能指针模板，类似于 std::shared_ptr，但通常用于引用计数。
    // 这里同样只做前向声明，具体实现来自 WebRTC 源码 rtc_base 目录。
    template<class T>
    class scoped_refptr;
}
namespace webrtc {
    // 前向声明。SharedModuleThread 是 WebRTC 提供的、可在多个模块间共享的
    // ProcessThread 封装，避免每个 PeerConnection/Call 都各自起一条处理线程。
    class SharedModuleThread;
}

namespace arlive {

    // Threads 是本文件对外暴露的抽象接口：
    // 一个 Threads 实例代表“一组可复用的后台线程”（一条媒体线程 + 一条工作线程 +
    // 一个按需创建的共享 ProcessThread），供推流器/播放器等上层对象使用，
    // 从而避免每创建一个推流/拉流实例就新起一整套线程。
    class Threads {
    public:
        // 虚析构，允许通过基类指针/智能指针安全地销毁派生类 ThreadsImpl。
        virtual ~Threads() = default;

        // 获取“媒体线程”指针，调用方一般用它来投递音视频采集/编解码等相关任务。
        // 返回的是裸指针，生命周期由持有 Threads 实例的 shared_ptr 管理，
        // 调用者不应该自己 delete 它。
        virtual rtc::Thread *getMediaThread() = 0;

        // 获取“工作线程”指针，通常用于承载网络/信令等不直接做媒体处理的任务。
        // 注意该线程在 ThreadsImpl 构造时被调用了 DisallowAllInvokes()，
        // 意味着不允许再对它做同步 Invoke() 调用（只能 Post 异步任务）。
        virtual rtc::Thread *getWorkerThread() = 0;

        // 获取（必要时惰性创建）与本组线程绑定的 SharedModuleThread，
        // 供 WebRTC 内部各模块（如统计、拥塞控制等）共用一条 ProcessThread。
        // 返回值是带引用计数的 scoped_refptr，最后一个引用释放后会自动销毁。
        virtual rtc::scoped_refptr<webrtc::SharedModuleThread> getSharedModuleThread() = 0;

        // it is not possible to decrease pool size
        // 设置底层线程池的目标大小（只能增大，不能减小，见 .cpp 中 set_pool_size_locked 的实现，
        // 它只会在当前 entries_ 数量小于 size 时追加新条目）。
        // 由需要预先“热身”多套线程的上层调用，一般在应用初始化阶段调用一次。
        static void setPoolSize(size_t size);

        // 从全局线程池中借出一个 Threads 实例（引用计数 +1）。
        // 返回的 shared_ptr 析构时会自动执行自定义删除器，把该 Entry 的 refcnt -1，
        // 并不会真正销毁底层线程，从而实现多个推流器/播放器实例共享少量线程对象。
        // 这是本文件对外的“真正线程池版”入口，由业务代码按需调用。
        static std::shared_ptr<Threads> getThreads();
    };

    // 注意：下面这个 StaticThreads 命名空间和上面 Threads 类里的 getThreads()
    // 是两套完全不同的机制！
    // - Threads::getThreads() 走的是 Pool<Threads, ThreadsCreator> 池化逻辑（见 .cpp），
    //   可以有多个槽位、按引用计数复用。
    // - 下面 StaticThreads::getThreads() 则是一个更简单的“全局单例”，永远只用
    //   下标 0 的一份 ThreadsImpl（见 .cpp 中的 function-local static），
    //   不经过 Pool。当前项目里 AndroidDeviceManager.cpp 实际调用的正是
    //   arlive::StaticThreads::getThreads()->getMediaThread()/getWorkerThread()，
    //   即走的是这套单例路径，而不是上面的池化路径。
    namespace StaticThreads {
        // 返回全局单例 ThreadsImpl 的媒体线程指针。内部实现为
        // getThreads()->getMediaThread()，即先拿到本命名空间的单例 Threads，
        // 再转调其虚函数。
        rtc::Thread *getMediaThread();

        // 头文件中声明了该函数，但 StaticThreads.cpp 中并未提供其定义，
        // 属于未使用的“死声明”（当前没有任何地方直接调用
        // arlive::StaticThreads::getWorkerThread()；实际都是通过
        // getThreads()->getWorkerThread() 这条路径访问工作线程）。
        // 保留声明但不做修复/删除，避免影响其他可能的外部引用。
        rtc::Thread *getWorkerThread();

        // 同样只有声明没有定义的“死声明”；注意函数名里的 "Moudle" 是一个
        // 拼写错误（应为 Module），但按要求不做改名/修复，仅在此说明。
        // 当前也没有代码调用它。
        rtc::scoped_refptr<webrtc::SharedModuleThread> getSharedMoudleThread();

        // 返回本命名空间维护的全局单例 Threads 的引用（注意返回的是
        // std::shared_ptr<Threads>& 的引用，而不是值），下标恒为 0，
        // 与 Threads::getThreads() 的池化逻辑互不影响。
        // 这是当前项目里 AndroidDeviceManager.cpp 实际使用的入口。
        std::shared_ptr<Threads> &getThreads();
    }

};
