# C++17 Actor Runtime

面向 Linux/glibc 的轻量 Actor 框架，使用 C++17、`std::any` 和基于
`ucontext` 的协作式有栈协程。源码、测试与示例位于 [`actor_runtime/`](actor_runtime/)。

支持多 worker、Actor 串行执行、固定 owner 的协程、消息广播、父子协程、
超时、显式取消，以及可嵌套的 `Any` / `All` / `AllSettled` 等待。
支持进程名 + Actor 名寻址的 Unix domain datagram 点对点 IPC、动态 Actor
注册/停止、纯 C 请求应答接口，以及有界独立线程池上的阻塞任务与超时等待。
进程内载荷为 `std::any`，跨进程仅传输无指针、同 ABI 的 C struct 字节。
详细语义与限制见 [框架说明](actor_runtime/README.md)。

`dev` 保存运行库、业务示例与测试；`teaching` 保存教学材料和练习进度。
原有 `master` 历史保持不变。

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
最新 WSL GCC 13.3 验收：运行库行覆盖率 **1015/1041 = 97.50%**；
原始 gcov 分支覆盖率 60.05%（包含 C++ 异常边，与行覆盖率不同）。

## 地址消毒与多进程测试

```bash
cmake -S actor_runtime -B actor_runtime/build-asan -DCMAKE_BUILD_TYPE=Debug -DACTOR_RUNTIME_SANITIZERS=ON
cmake --build actor_runtime/build-asan -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir actor_runtime/build-asan --output-on-failure
```

运行库实现 ASan fiber-switch hooks。测试包含真实独立进程、纯 C 编译、同名
Actor 隔离、实例重建、无效报文、取消清理、阻塞任务超时和队列满载等边界。
测试结果和边界说明见 [验收记录](docs/DEV_PLAN.md)。
