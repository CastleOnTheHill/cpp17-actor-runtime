# dev 分支扩展验收

教学提交及用户练习保存在 `teaching`（5540b50）；`dev` 从 0e21450 建立。
不改写已有 master 历史。此文件用于跨轮次保留完整任务与验收证据。

待完成：

- [x] 教学/业务分支分离，保留未提交练习。
- [x] 进程名 + Actor 名寻址；Unix domain SOCK_DGRAM 点对点通信。
- [x] C/POD 字节载荷、报文边界校验、迟到响应实例隔离。
- [x] 动态注册/停止 Actor，取消挂起任务，验证同名重建等边界。
- [x] 纯 C 模块请求/应答接口及真实 C 编译测试。
- [x] 独立有界线程池执行阻塞函数；Actor 可超时，不占 worker。
- [x] 运行时诊断日志与多进程测试日志。
- [x] sanitizer fiber-switch hooks；WSL ASan/UBSan/LSan 验证。
- [x] 普通回归、跨进程集成测试和文档。
- [x] 分支提交/推送及远端校验。

接口约束：跨进程只发送复制后的字节，不把 std::any 对象表示直接上网；
业务 C struct 必须同 ABI、无指针，调用方明确尺寸。动态加载按运行时注册
Actor 实例设计。阻塞函数不能安全强杀；超时只解除等待，运行中的函数仍
需返回后才能回收线程。必须在文档和测试中明确这个生命周期。

## 现场验证

WSL Ubuntu-24.04 / Linux glibc / GCC，2026-09-06：

- `build-asan` 中 10/10 CTest 通过，总耗时 6.00 秒。
- 编译参数实际包含 `-fsanitize=address,undefined -fno-omit-frame-pointer`。
- 执行设置 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` 和
  `UBSAN_OPTIONS=halt_on_error=1`，未配置 suppression；无 sanitizer 错误。
  ASan 仍会打印其通用的 makecontext/swapcontext 支持警告。
- 日志：`actor_runtime/build-asan/Testing/Temporary/LastTest.log`（本机构建产物，不入库）。

| 测试目标 | 验证内容 |
| --- | --- |
| actor_runtime_tests | 广播、关联、串行性、worker 迁移、owner 固定、取消和清理 |
| wait_semantics_tests | 嵌套 Any/All/AllSettled、错误聚合与超时竞态 |
| api_contract_tests | 参数校验、异常隔离、关闭契约 |
| order_fulfillment | 1/4 worker 下成交、缺货、支付超时、撤单及 RAII |
| lifecycle_tests | 20 轮动态注册/取消/销毁、同名重建、旧引用拒绝 |
| ipc_lifecycle_tests | 进程名冲突、跨系统同名、迟到响应实例隔离、socket 重绑定 |
| ipc_c_processes | 两个服务进程 + 一个纯 C 客户端进程，100 次路由请求及错误边界 |
| c_api_lifecycle_tests | C++ 宿主借用、C 回调、cleanup、关闭客户端和非法参数 |
| blocking_tests | 独立线程、返回值/异常、超时不阻塞 Actor、迟到 join、Actor 先退出 |
| blocking_capacity_tests | 确定性握手验证 running=1/pending=1 时第三次提交拒绝 |

## 覆盖率门禁

权限恢复后，已重新编译全部新增运行库模块并清空旧计数，10/10 CTest
通过（6.03 秒），coverage 目标返回 0。实测运行库行覆盖率：
**1015/1041 = 97.50%**，满足 >=90% 门禁，无代码排除。
原始 gcov 分支覆盖率为 **995/1657 = 60.05%**（包含 C++ 异常边）。
其中 IPC 为 108/109 行，C API 为 95/97 行，阻塞池为 26/27 行。
普通 coverage 构建的 sanitizer 包装是空操作；真实切栈 hooks 由独立
`build-asan` 的 ASan 测试验证，不以普通覆盖率替代地址消毒。

完整逐文件报告在本机 `actor_runtime/build-coverage/coverage/report.md`，
`report.json` 包含未覆盖行。执行命令：

```bash
cd actor_runtime
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DACTOR_RUNTIME_COVERAGE=ON
cmake --build build-coverage --target coverage -j4
```

曾因执行权限审核超时受阻，当前权限已经恢复，以上为恢复后新生成的结果。
构建出现挂载盘时间戳偏差提示，因此另行执行普通 Debug 构建复核。
独立 `build-dev` 普通 Debug 构建完成，10/10 CTest 通过，耗时 10.17 秒，
未再出现时间戳警告。

## GitHub 交付

已推送并用 `git ls-remote --heads origin dev teaching master` 实际核对：

- 私有仓库：`CastleOnTheHill/cpp17-actor-runtime`。
- `dev` 功能提交：`0f2a65a42bc6b4837455f5268492860c036604e1`。
- `teaching`：`5540b50fba426a7ccd4ac0ca41974ab4855f4dc6`，含用户未完成练习。
- 远端 `master` 保持 `0e214506f1c5fdc962877a19e55d403c76eafc93`，未改写历史。

本验收记录的后续文档提交不改变已验证的运行库代码。
