# C++17 Actor Runtime

面向 Linux/glibc 的单进程 Actor 框架，使用 C++17、`std::any` 和基于
`ucontext` 的协作式有栈协程。源码、测试与示例位于 [`actor_runtime/`](actor_runtime/)。

支持多 worker、Actor 串行执行、固定 owner 的协程、消息广播、父子协程、
超时、显式取消，以及可嵌套的 `Any` / `All` / `AllSettled` 等待。
当前不支持 IPC；详细语义与限制见 [框架说明](actor_runtime/README.md)。

## 从底层学习这个框架

[课程入口](course.html) · [24 章路线](COURSE.md) · [WSL 教学实验](labs/README.md)

按“构建者／还原论”方式，从调用与返回、手写暂停状态，到真实栈切换逐层构建。
前八章已实现；第 9–24 章为规划。每章包含预测、最小实验、构建任务与闭卷过关题。

## 构建与测试

在 Linux 或 Windows WSL 中执行：

```bash
cd actor_runtime
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j4
ctest --test-dir build --output-on-failure
./build/order_fulfillment_example
```

[订单履约示例](actor_runtime/examples/order_fulfillment.cpp) 提供中文注释，
模拟成交、缺货、支付超时和用户撤单，验证嵌套协程、取消及资源清理。

## 覆盖率门禁

```bash
cd actor_runtime # 如果已经在此目录，则不需要重复执行
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DACTOR_RUNTIME_COVERAGE=ON
cmake --build build-coverage --target coverage -j4
```

门禁要求运行库行覆盖率不低于 90%，不排除防御代码；报告由测试现场生成。
已在 Ubuntu WSL / GCC 13.3 中验证，最近一次行覆盖率为 97.36%，原始 gcov
分支覆盖率为 65.96%（包含 C++ 异常边）。
