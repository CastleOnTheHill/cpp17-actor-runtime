# 课程依据与延伸资源

本课程先用自己编写的最小实验说明控制流，再用实现和原始文档核对。以下核对日期为 2026-09-05；平台为 Ubuntu-24.04 WSL / GCC 13.3 / glibc，教学实验强制 C++17、-O0。

## Knowledge

| 资源 | 用在哪 | 如何读 |
|---|---|---|
| [Linux man-pages: makecontext / swapcontext](https://man7.org/linux/man-pages/man3/swapcontext.3.html) | 第 3 章栈设置、切换与 uc_link | 先看 DESCRIPTION 与 RETURN VALUE；NOTES 的参数可移植性后读 |
| [Linux man-pages: getcontext](https://man7.org/linux/man-pages/man3/getcontext.3.html) | 第 3 章 context 所含现场 | 只看 ucontext_t 与 mcontext_t 关系；不展开信号处理 |
| [C++ 工作草案：对象生存期](https://eel.is/c++draft/basic.life) | 第 1 章对象与存储区分 | 滚动草案是概念参考，不宣称是固定 C++17 版本；实验以 C++17 编译 |
| [C++ 工作草案：自动存储期](https://eel.is/c++draft/basic.stc.auto) | 第 1–2 章函数块内局部对象 | 不把抽象语义误当作固定机器栈布局 |
| [教学实验](labs/) | 第 1–3 章可观察证据 | 先写预测/练习，后读参考实现 |
| [框架文档](actor_runtime/README.md)与 [实现](actor_runtime/src/) | 后续各章的真实契约 | 当前源码为本课程框架事实依据；教学简化模型不能覆盖全部语义 |

说明：ucontext 不是标准 C++17 协程，也不是现代 POSIX 的通用可移植方案；这里选择它是为了理解当前 Linux/glibc 框架。C++20 co_await 不属于前三章前置知识。本课程不比较未验证的切换性能，不用“协程一定比线程快”作结论。

## Wisdom (Communities)

当前先进行一对一构建与反馈；尚未选择或要求加入外部社区。后期可将自己构造的最小复现与实现边界交给实际维护者核对，不能用本教程的 PASS 代替外部生产经验。

## Gaps

后续章节展开前还需逐项补齐同步语义、平台 ABI、定时器与关闭行为的原始依据；现在的资源足够支撑前三章，不假称已完成全部 24 章的资料核查。
