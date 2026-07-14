//
// Created by liu on 2021/9/18.
//

#ifndef LIVEPLAYER_PLATFORMCONTEXT_H
#define LIVEPLAYER_PLATFORMCONTEXT_H

namespace arlive {
  // PlatformContext：跨平台“上下文句柄”的抽象基类（marker/interface 基类）。
  // 本类本身没有任何成员变量或非虚方法，只声明了一个虚析构函数，
  // 目的是让不同平台可以各自派生出具体实现（例如 Android 平台的
  // AndroidContext，定义在其他文件中，通常会持有 JavaVM/JNIEnv 或
  // Android Context 相关的引用），并通过基类指针/引用统一传递、
  // 屏蔽具体平台差异。
  class PlatformContext {
    public:
        // 虚析构：保证通过 PlatformContext* 基类指针释放派生类对象
        // （如 AndroidContext）时能正确调用到派生类的析构函数。
        // = default 表示使用编译器生成的默认实现（空实现）。
        virtual ~PlatformContext() = default;
    };
}

#endif //LIVEPLAYER_PLATFORMCONTEXT_H
