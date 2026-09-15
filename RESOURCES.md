# 课程依据与延伸资源

本课程先用自己编写的最小实验说明控制流，再用实现和原始文档核对。以下核对日期为 2026-09-05；平台为 Ubuntu-24.04 WSL / GCC 13.3 / glibc，教学实验强制 C++17、-O0。

## Knowledge

| 资源 | 用在哪 | 如何读 |
|---|---|---|
| [Linux man-pages: makecontext / swapcontext](https://man7.org/linux/man-pages/man3/swapcontext.3.html) | 第 3 章栈设置、切换与 uc_link | 先看 DESCRIPTION 与 RETURN VALUE；NOTES 的参数可移植性后读 |
| [Linux man-pages: getcontext](https://man7.org/linux/man-pages/man3/getcontext.3.html) | 第 3 章 context 所含现场 | 只看 ucontext_t 与 mcontext_t 关系；不展开信号处理 |
| [C++ 工作草案：对象生存期](https://eel.is/c++draft/basic.life) | 第 1 章对象与存储区分 | 滚动草案是概念参考，不宣称是固定 C++17 版本；实验以 C++17 编译 |
| [C++ 工作草案：自动存储期](https://eel.is/c++draft/basic.stc.auto) | 第 1–2 章函数块内局部对象 | 不把抽象语义误当作固定机器栈布局 |
| [教学实验](labs/) | 第 1–8 章可观察证据 | 先写预测/练习，后读参考实现 |
| [框架文档](actor_runtime/README.md)与 [实现](actor_runtime/src/) | 后续各章的真实契约 | 当前源码为本课程框架事实依据；教学简化模型不能覆盖全部语义 |

说明：ucontext 不是标准 C++17 协程，也不是现代 POSIX 的通用可移植方案；这里选择它是为了理解当前 Linux/glibc 框架。C++20 co_await 不属于前三章前置知识。本课程不比较未验证的切换性能，不用“协程一定比线程快”作结论。

### 第 4–8 章新增依据（2026-09-06 核对）

- [C++ 工作草案：mutex requirements](https://eel.is/c++draft/thread.mutex.requirements)
  用于第 7 章锁的独占、作用域与同步边；不从测试耗时推断互斥或公平。滚动草案不是固定 C++17 版本，代码仍以 C++17 编译。
- [C++ 工作草案：any_cast 非成员操作](https://eel.is/c++draft/any.nonmembers)
  用于第 8 章指针形式 any_cast 的失败检查；不将编号约定当成静态类型保证。
- [teaching 分支 actor_system.cpp](actor_runtime/src/actor_system.cpp)
  用于 workerLoop、processJob、resumeFiber、finishFiber 与 ActorSystem::send 的实际路径。特别核对 job 优先于到期 timer、try_to_lock、切回后回收和邮箱无固定容量等边界。
- [最小有栈调度外壳](labs/support/fiber_lab.h)
  第 4–6 章自己构建的可执行模型。模拟时间、完成清单与状态规则由其代码定义，不冒充外部标准。

## Wisdom (Communities)

当前先进行一对一构建与反馈；尚未选择或要求加入外部社区。后期可将自己构造的最小复现与实现边界交给实际维护者核对，不能用本教程的 PASS 代替外部生产经验。

## Gaps

第 9 章以后仍需补齐广播/请求响应、复杂等待、跨线程 owner 与生命周期的逐项依据；现有资源与源码检查支持第 1–8 章，不假称已核查全部 24 章。
