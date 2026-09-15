# P0 内存优化验收（2026-09-16）

本文件记录四项 P0 的实现范围、验证证据、性能实测，以及 2026-09-16 对性能取舍的最终决定。所有数字都来自本仓库 `benchmark-results/` 下已提交的原始输出，不是复述计划。

## 1. 实现范围

| 项 | 内容 | 关键证据 |
|---|---|---|
| P0-1 结果生命周期 | root 成功结果不再长期缓存；父退出时已完成的 child 结果被清理，未完成的 child 按 `ChildLifetime` 取消或脱离；新增 `forgetChild`/`forgetTask`；晚到结果不再复活 | `tests/result_lifetime_tests.cpp`（含父退出矩阵、显式 forget、晚到结果丢弃、64 个同时挂起定时器） |
| P0-2 blocking 结果 | 便捷 `blocking()` 返回即释放；显式 `submitBlocking` 可用 `forgetTask` 主动放弃，晚到结果走丢弃路径 | 同上 + `tests/blocking_tests.cpp` |
| P0-3 等待与定时器清理 | 等待登记统一注销，`Any` 胜出时撤销输家；owner-worker 索引堆支持按登记撤销并收缩容量；非活动登记不再滞留 | `tests/wait_cleanup_tests.cpp`（含堆容量断言）、`benchmark-results/2026-09-15-memory/candidate-r6` |
| P0-4 邮箱预算 | 每 Actor 条数与估算字节双限额、单消息上限、系统级总预算；`trySend` 返回 `Accepted/Full/Stopped/MessageTooLarge`；计数与字节精确到每条消息 | `tests/mailbox_capacity_tests.cpp`、`benchmark-results/2026-09-14-mailbox/` |

## 2. 验证结果

- 功能回归：CTest **15/15**（精确预算模式为默认）。
- Sanitizer：`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1` 下 **15/15**。
- 内存诊断（`benchmarks/run_memory.py`，每场景 3 重复）：**全套通过**，含 10 万条深队列、fibers 四种栈配置、`completed_*` 多轮完成、`tasks_*`、`timers_10000/100000`、`waiters_10000/100000`。证据：`benchmark-results/2026-09-15-memory/candidate-r6`。
- 覆盖率：`gcovr` 统计 `src/` 与 `include/` 为 **1391/1442 行 = 96%**（排除测试与基准）。证据：`benchmark-results/2026-09-15-coverage/coverage.txt`。复现命令见该目录说明与 `NOTES.md`。
- 三项直接证据：`completed_*` 证明 root 结果不再逐轮累积；`waiters_*` 证明 `Any` 竞争的输家登记被撤销（持有期 1 键 1 引用、释放后 0/0）；`timers_*` 证明等待结束后堆无残留且 RSS 平坦。

## 3. 性能实测与最终决定

测量方法：同机交替 A/B，同轮内配对求比值（消除机器漂移），取配对中位数作为门槛；噪声底用同一对二进制自比测得约 ±1%（16 轮 × 200 万 round trips）。早期基于 3 轮中位数的结论不可用，已作废。

对比冻结基线 `ad2389b`：

| 场景 | 精确模式（默认） | 预留模式（可选） |
|---|---:|---:|
| event-parallel | 0.9562 | **1.2440** |
| event-single | 0.9325 | 0.9642 |
| fiber-single | 0.9557 | 0.9781 |

结论：多 worker 场景在预留模式下比基线快 20% 以上；单 worker 场景两种模式都低 2–4%，差距来自 P0-4 要求的**逐消息字节口径**本身（信封常量、`payloadSizer`/`defaultPayloadBytes` 查询、三次饱和加法、`estimatedMailboxBytes` 写回），与共享计数器无关——后者已通过拆分 admitted/returned 计数器消除。

已否证并撤回的方案（均留有证据）：裸 `fetch_add` 与两级预留（瞬时超计可被 `stats().queuedMailboxBytes` 观察到，破坏并发精确填满断言）、Actor 内 returned 快照缓存（实测单 worker 从 0.9642 降到 0.9188）。

**2026-09-16 决定：接受单 worker 2–4% 的代价，保留逐消息精确字节预算。** 理由是字节预算是 P0-4 的核心要求，其观测语义（并发下恰好填满到限额、`stats().queuedMailboxBytes` 精确）已被回归测试固定；用条数口径替换会改变对外契约。需要吞吐时可显式选择预留模式。

## 4. 预留模式（可选）的语义

`Config::mailboxBudget = MailboxBudget::Reserve`（默认 `Exact`）＋ `Config::reserveChunkBytes`（默认 64 KiB）：每个 Actor 从共享池整块预取，仅在换块时触碰共享计数器；连续 64 次准入门到空闲或 Actor 停止时归还整块。

- 保证：**永不超支**；每 Actor 条数与字节限额仍然精确。
- 代价：并发接近系统限额时可能**提前拒绝**（提前量以块为单位有界）；`stats().queuedMailboxBytes` 在该模式下是上界而非精确值。
- 不变量测试：`tests/mailbox_capacity_tests.cpp` 的 `reserve_mode`（1/4 worker，断言不超支、确有背压、提前拒绝有界、恰好投递一次、停止后归还预算）。

## 5. 已知边界

- 定时器场景在持有期实测堆条目为 0，因此它证明的是清理与无增长，不证明「曾同时存在一万条堆条目」；64 条同时挂起由 `result_lifetime_tests.cpp` 覆盖，堆为空是间接保证。
- fiber 固定绑定创建时的 owner，因此「跨 worker 定时器取消」在设计上不可达；若将来引入 fiber 迁移，必须重新审视 `TimerQueue` 的归属模型（见 `src/actor_system.cpp` 中该赋值处的注释）。
- 计时器后端仍为现有实现，未切换到 timerfd。
