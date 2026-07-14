#include "StaticThreads.h"

// rtc::Thread 的完整定义：创建/命名/启动线程、DisallowAllInvokes 等都来自这里。
#include "rtc_base/thread.h"
// webrtc::SharedModuleThread / webrtc::ProcessThread 的完整定义，
// 用于实现 getSharedModuleThread()。
#include "call/call.h"

// std::mutex / std::unique_lock，给 Pool 内部的 entries_ 提供互斥保护。
#include <mutex>
// std::min_element，用于在 Pool::get() 中挑选“当前引用计数最少”的 Entry。
#include <algorithm>

namespace arlive {

// 通用的“引用计数对象池”模板。
// ValueT：池中存放的对象类型（这里是 Threads 接口）。
// CreatorT：一个可调用对象（函数对象），签名近似 std::unique_ptr<ValueT>(size_t index)，
//           用于按需创建第 index 个槽位的 ValueT 实例（见下面 ThreadsCreator）。
// 继承 std::enable_shared_from_this，使得 Pool 自身可以在成员函数里安全地拿到
// 指向自己的 shared_ptr（this->shared_from_this()），从而延长自身生命周期。
template <class ValueT, class CreatorT>
class Pool : public std::enable_shared_from_this<Pool<ValueT, CreatorT>> {
  // 池中的一个槽位：持有实际对象 + 当前被借出的引用计数。
  struct Entry {
    std::unique_ptr<ValueT> value; // 该槽位实际持有的 ValueT 对象（如一组线程）。
    size_t refcnt;                 // 当前有多少个外部 shared_ptr<ValueT> 借用了它。

    // 重载 operator<，仅比较 refcnt。
    // 这样 std::min_element 就能直接在 entries_ 容器上找到“当前引用计数最小”
    // （即最空闲/负载最低）的那个 Entry，从而实现简单的负载均衡式复用。
    bool operator < (const Entry &other) const {
      return refcnt < other.refcnt;
    }
  };

public:
  // 构造函数保存创建者（通常是 ThreadsCreator 的实例），
  // 用 std::move 避免不必要的拷贝。
  explicit Pool(CreatorT creator) : creator_(std::move(creator)) {
  }

  // 从池中借出一个 ValueT 实例（这里是 Threads*），返回带自定义删除器的 shared_ptr。
  std::shared_ptr<ValueT> get() {
    // 加锁保护 entries_ 的读写，保证多线程并发调用 get() 时是线程安全的。
    std::unique_lock<std::mutex> lock(mutex_);
    // 确保池中至少有 1 个 Entry（懒加载：第一次调用时才真正创建线程组）。
    set_pool_size_locked(1);
    // 在所有 Entry 中找到 refcnt 最小的一个下标 i（负载最低，优先复用它）。
    auto i = std::min_element(entries_.begin(), entries_.end()) - entries_.begin();
    // 构造一个“别名式”shared_ptr：
    // - 指向的对象是 entries_[i].value.get()（裸指针，真正生命周期由 Pool 管理，
    //   不由这个 shared_ptr 控制析构）；
    // - 自定义删除器捕获了 i（借出的槽位下标）和
    //   self = this->shared_from_this()（指向 Pool 自身的 shared_ptr）；
    //   捕获 self 是关键：只要还有一个借出的 Threads 没被归还，
    //   Pool 对象本身就不会被析构（因为它的引用计数被这个 shared_ptr 拉高了），
    //   从而避免“线程还在被使用，但线程池已经被销毁”的悬空访问问题。
    // - 当外部持有者释放这个 shared_ptr（引用计数归零）时，会调用删除器，
    //   删除器只是把对应槽位的 refcnt 减一（不真正删除线程对象），
    //   实现“归还但不销毁”的复用效果。
    return std::shared_ptr<ValueT>(entries_[i].value.get(),
      [i, self = this->shared_from_this()](auto *ptr) {
        self->dec_ref(i);
      });
  }

  // 设置池的目标大小（只能增大）。加锁后转调内部的 _locked 版本。
  void set_pool_size(size_t size) {
    std::unique_lock<std::mutex> lock(mutex_);
    set_pool_size_locked(size);
  }

  // 归还槽位 i：将其引用计数减一。由 get() 里生成的自定义删除器调用。
  // 独立加锁，保证与 get()/set_pool_size 互斥执行。
  void dec_ref(size_t i) {
    std::unique_lock<std::mutex> lock(mutex_);
    entries_.at(i).refcnt--;
  }

private:
  std::mutex mutex_;            // 保护 entries_ 的互斥锁。
  std::vector<Entry> entries_;  // 池中所有槽位（每个槽位是一组 Threads）。

  CreatorT creator_;             // 用于创建新槽位对象的“工厂”可调用对象。

  // 内部辅助函数：假定调用者已持锁。
  // 只有当 size 大于当前 entries_.size() 时才会追加新槽位，
  // 因此“不支持缩小池子”，这与头文件中 setPoolSize 的注释
  // “it is not possible to decrease pool size”一致。
  // 注意这里传给 creator_ 的参数是 i + 1（从 1 开始编号），
  // 对应 ThreadsImpl 构造函数里用来生成线程名后缀的下标。
  void set_pool_size_locked(size_t size) {
    for (size_t i = entries_.size(); i < size; i++) {
      entries_.emplace_back(Entry{creator_(i + 1), 0});
    }
  }
};

// Threads 接口的具体实现：真正持有并管理一对 rtc::Thread（媒体线程 + 工作线程），
// 以及一个惰性创建的 SharedModuleThread。
// 池化机制（Pool）里的每个 Entry.value 实际就是一个 ThreadsImpl 实例；
// StaticThreads 命名空间里的全局单例也是直接 new 出的一个 ThreadsImpl(0)。
class ThreadsImpl : public Threads {
  using Thread = std::unique_ptr<rtc::Thread>; // 局部别名，简化下面成员声明。
public:
  // i：槽位下标（从 0 开始）。i==0 时线程名不加后缀（对应“第一份”/单例场景），
  // 否则加上 "#<i>" 后缀，避免同一进程内出现重名线程，方便日志/调试区分。
  explicit ThreadsImpl(size_t i) {
    auto suffix = i == 0 ? "" : "#" + std::to_string(i);
    // 创建并启动名为 "arlive-media[#i]" 的媒体线程。
    media_ = create("arlive-media" + suffix);
    // 创建并启动名为 "arlive-work[#i]" 的工作线程。
    worker_ = create("arlive-work"  + suffix);
    // DisallowAllInvokes()：WebRTC rtc::Thread 提供的调试/保护手段，
    // 调用后如果有人再对 worker_ 线程执行同步的 Invoke()（阻塞等待其他线程
    // 在该线程上执行任务并返回结果），会触发断言/失败，而不是静默地
    // 阻塞或造成潜在死锁。这里把 worker_ 标记为“只允许 Post 异步任务，
    // 不允许同步 Invoke”，用于提前发现/防止跨线程阻塞调用的误用。
    worker_->DisallowAllInvokes();
  }

  // 返回媒体线程裸指针，供上层投递媒体相关任务。
  rtc::Thread *getMediaThread() override {
    return media_.get();
  }
  // 返回工作线程裸指针，供上层投递非媒体（如信令/网络）相关任务。
  rtc::Thread *getWorkerThread() override {
    return worker_.get();
  }
  // 获取共享的 ProcessThread 封装，按需（第一次调用时）创建。
  rtc::scoped_refptr<webrtc::SharedModuleThread> getSharedModuleThread() override {
    // This function must be called from a single thread because of SharedModuleThread implementation
    // So we don't care about making it thread safe
    // （原注释：由于 SharedModuleThread 自身实现的限制，本函数必须只在单一线程上调用，
    //  所以这里不做线程安全处理，即不会加锁保护 shared_module_thread_。）
    if (!shared_module_thread_) {
      // 用一个新建的 "arlive-module" ProcessThread 创建 SharedModuleThread，
      // 并传入一个“自清空”回调：当 SharedModuleThread 的最后一个引用被释放、
      // 内部资源被销毁时，会调用这个 lambda 把 shared_module_thread_ 置空，
      // 这样下次调用 getSharedModuleThread() 就会重新创建一个新的实例，
      // 而不会一直持有一个已失效的引用。
      shared_module_thread_ = webrtc::SharedModuleThread::Create(
          webrtc::ProcessThread::Create("arlive-module"),
          [=] { shared_module_thread_ = nullptr; });
    }
    return shared_module_thread_;
  }

private:
  Thread media_;   // 媒体线程实例。
  Thread worker_;  // 工作线程实例（已禁止同步 Invoke）。
  // 惰性创建的共享 ProcessThread 封装；为空表示尚未创建或已被清空。
  rtc::scoped_refptr<webrtc::SharedModuleThread> shared_module_thread_;

  // 创建一个新的 rtc::Thread 并完成初始化（命名 + 启动）。
  static Thread create(const std::string &name) {
    return init(std::unique_ptr<rtc::Thread>(rtc::Thread::Create()), name);
  }
  // 给线程设置名称（第二个参数 nullptr 表示不附带 obj 指针）并启动线程的消息循环。
  static Thread init(Thread value, const std::string &name) {
    value->SetName(name, nullptr);
    value->Start();
    return value;
  }
};

// 供 Pool<Threads, ThreadsCreator> 使用的“工厂”仿函数：
// operator() 接收槽位下标 i，返回一个新建的 ThreadsImpl(i)，
// 以 std::unique_ptr<Threads> 的形式交给 Pool::Entry 持有。
class ThreadsCreator {
public:
  std::unique_ptr<Threads> operator()(size_t i) {
   return std::make_unique<ThreadsImpl>(i);
  }
};

// 获取全局唯一的线程池对象（Meyer's Singleton / function-local static 写法）。
// C++11 起标准保证函数内 static 局部变量的初始化是线程安全的
// （即使多个线程第一次并发调用 get_pool()，pool 也只会被构造一次），
// 因此这里不需要额外加锁就能安全地做到“惰性 + 单例”。
// 用 shared_ptr 持有 Pool，是为了配合 Pool::get() 里的
// this->shared_from_this()：只要还有外部借出的 Threads 存在，
// 这个 shared_ptr 引用计数就不会归零，Pool 也就不会被提前销毁。
Pool<Threads, ThreadsCreator> &get_pool() {
  static auto pool = std::make_shared<Pool<Threads, ThreadsCreator>>(ThreadsCreator());
  return *pool;
}

// Threads::setPoolSize 的实现：直接委托给全局池的 set_pool_size。
void Threads::setPoolSize(size_t size){
  get_pool().set_pool_size(size);
}
// Threads::getThreads 的实现：直接委托给全局池的 get()，
// 从池中借出一个（负载最小的）Threads 实例。
// 这是“池化版”的对外入口。
std::shared_ptr<Threads> Threads::getThreads(){
  return get_pool().get();
}

// 下面这个 StaticThreads 命名空间的实现，和上面 Threads::getThreads() 是
// 两套彼此独立的机制：这里维护的是一个“固定下标为 0”的全局单例
// ThreadsImpl，不经过 Pool/引用计数逻辑，只是简单的懒加载单例。
// 当前项目中 AndroidDeviceManager.cpp 实际调用的是
// arlive::StaticThreads::getThreads()->getMediaThread()/getWorkerThread()，
// 也就是走的这一套单例路径。
namespace StaticThreads {


// 返回本命名空间单例的媒体线程指针：先拿到下面的单例 Threads，
// 再转调其虚函数 getMediaThread()。
rtc::Thread *getMediaThread() {
  return getThreads()->getMediaThread();
}

// 本命名空间自己的单例入口：function-local static，线程安全的懒初始化
// （同样受益于 C++11 对局部 static 初始化的线程安全保证）。
// 固定传入下标 0，对应 ThreadsImpl 构造函数里“不加后缀”的命名分支，
// 即线程名固定为 "arlive-media" / "arlive-work"（没有 "#N" 后缀）。
// 返回的是 shared_ptr 的引用，调用者可以长期持有这份引用而不必每次都
// 重新查询，因为它在整个进程生命周期内只会被创建一次。
std::shared_ptr<Threads> &getThreads() {
  static std::shared_ptr<Threads> threads = std::make_shared<ThreadsImpl>(0);
  return threads;
}
};

}
