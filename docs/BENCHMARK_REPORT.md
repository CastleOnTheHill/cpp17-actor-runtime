# Actor runtime benchmark：同机实测与优化建议

日期：2026-09-08。本机 WSL2，开发分支 `dev`，运行库基线 `5acad1251c7dd52ffbceae028889d896717cdd2e`。
本次只增加 benchmark、诊断和文档，未优化或改动运行库；教学 worktree 不变。

## 结论先行

64 B 进程内普通 echo，单 worker 约 **199 万次往返/秒**；4 worker、8 对 Actor 约 **60 万次/秒**。
主要短板不是单次 `std::any` 复制，而是多 worker 的调度/锁开销，以及有栈等待的唤醒稳定性。
真实跨进程 C API echo 约 **1.02 万次往返/秒**，同机裸 Unix DGRAM 约 **2.92 万次/秒**。
以上是指定闭环工作负载的结果，不是框架的绝对峰值、网络服务器容量或生产 SLA。

完成：53 个配置 × 3 轮；12 个重点/敏感性配置 × 5 轮；6 次配对唤醒诊断；8 组真实 perf 火焰图；11/11 CTest。
所有成功的测量均检查回复内容、顺序、总数与延迟样本数。失败的接入冒烟不计入性能结果。

## 1. 关键结果

下面使用**五轮固定请求预算补测的中位数**，不是最大值。延迟列是各轮 p99 的中位数，不是合并所有样本的 p99。
单位 RPS = 完整请求–回复往返/秒；进程内逻辑消息投递数约为 2 × RPS。
每对 Actor 一个客户端和一个 echo；payload 64 B、窗口 1、batch 1。

| 工作负载 | 本框架 RPS（五轮范围） | CAF 0.19.6 | SObjectizer 5.8.3 |
|---|---:|---:|---:|
| 1 worker，1 对 | 1,986,300（1,931,352–2,050,793） | 2,383,115（2,352,803–2,451,187） | 3,570,866（3,450,749–3,764,026） |
| 4 worker，8 对 | 600,773（594,852–605,923） | 5,880,926（5,596,635–7,107,229） | 1,330,299（1,324,918–1,365,804） |
| 1 worker，1 对：p99 µs | 1.021 | 0.873 | 0.586 |
| 4 worker，8 对：p99 µs | 55.620 | 1.115 | 8.349 |

单 worker 对照约为本框架的 1.20× / 1.80×；4 worker、8 对时约为 9.79× / 2.21×。
同一拓扑的扩展性请看完整矩阵：8 对、窗口 1，本框架从 1 worker 的 211.5 万下降至 4 worker 的 57.2 万；
CAF 从 238.2 万升至 614.4 万；SO5 从 326.0 万下降至 137.7 万。并非所有成熟框架在这一调度配置下都随核数提速。

### 窗口、热点和批处理（三轮矩阵）

4 worker、8 对、窗口 16，本框架 batch1 为 122.5 万，CAF batch1 为 666.1 万，SO5 batch1 为 197.6 万 RPS。
仅将对照框架 batch 改为 64，CAF 为 963.0 万，SO5 为 557.9 万，分别约 1.45× / 2.82×。
这说明不能只拿对照框架受限的 batch1 排名；但它也**不证明**给本框架加 batch 就能获得同样收益。
单热点 server 的结果单列在矩阵中：其串行瓶颈和独立 Actor 对不是同一场景。

### 有栈等待

单 worker、单客户端长存 fiber：三轮中位数 79.3 万 RPS，p99 2.498 µs；普通事件对应为 204.1 万。
这是保留调用栈、关联等待和恢复能力的增量成本，CAF/SO5 的普通 handler 不是等价有栈协程。
此例每阶段只创建一个长期存活的 fiber，**没有测试每条请求创建/销毁 fiber 的成本**。

4 worker、1 客户端极不稳定：初始三轮仅 1,332–3,033 RPS、每轮 46 个 RTT 样本，不能据此估计可靠 p99。
随后每轮 6,000 次请求、逐次采样，五轮仍为 2,396–36,391 RPS；其中一轮 p99 达 10.223 ms。
多客户端长测也未消除波动：8 客户端为 5.23–18.98 万，64 客户端为 11.42–33.93 万 RPS。
因此不把某一次较快的 fiber 运行描述成已解决或稳定容量。

### 真正的跨进程 echo

| 64 B、单客户端、窗口1 | 中位 RPS | 五轮范围 | p99 µs |
|---|---:|---:|---:|
| 本框架命名 Actor + C client bridge | 10,171 | 10,017–10,551 | 206.641 |
| 裸 AF_UNIX SOCK_DGRAM | 29,161 | 22,367–29,823 | 92.423 |

服务器是独立子进程，不是本进程 mock。裸 socket 只衡量同类传输的较薄基线，不具备 Actor 命名、调度、关联回复和生命周期能力。
两者逻辑数据都是 64 B + 16 B 序号/时间戳；本框架另有协议头，因此**不是相同 wire size**。
三轮 8 客户端矩阵分别约 3.39 万 / 9.65 万 RPS。0/64/1024 B 的结果变化较小，提示固定路径成本更值得先查，但尚不能量化内核/桥接各自占比。

## 2. 火焰图与证据

证据目录：[`benchmark-results/2026-09-08`](../benchmark-results/2026-09-08/)。
每组保留 `perf.data`、`perf.script`、`stacks.folded`、`flamegraph.svg`、`perf.report.txt`、精确命令/退出码、样本统计和文件哈希。
另保留带调试符号的可执行文件、构建 flags、源代码快照、第三方归档与转换脚本。

| 图 | 样本数 | 已解析叶节点中的重要观察 | 未解析叶节点占全部样本 |
|---|---:|---|---:|
| [事件 1w/1c](../benchmark-results/2026-09-08/profiles/native_echo-w1-c1/flamegraph.svg) | 1,050 | mutex lock/unlock/trylock 合计占全部样本 42.4%；malloc/free 6.7%；any 管理 4.2% | 3.2% |
| [事件 4w/8c](../benchmark-results/2026-09-08/profiles/native_echo-w4-c8/flamegraph.svg) | 992 | 上述 mutex 叶节点合计 33.6%；另有大量 OS 上下文切换 | 16.2% |
| [fiber 1w/1c](../benchmark-results/2026-09-08/profiles/native_fiber_echo-w1-c1/flamegraph.svg) | 897 | `swapcontext` 占 13.6%；等待、分配和锁亦有成本 | 4.0% |
| [fiber 4w/1c](../benchmark-results/2026-09-08/profiles/native_fiber_echo-w4-c1/flamegraph.svg) | 7 | 样本过少，不作热点比例结论 | 14.3% |
| [CAF 4w/8c](../benchmark-results/2026-09-08/profiles/caf_echo-w4-c8/flamegraph.svg) | 3,754 | resume、enqueue、引用计数与消息分配 | 5.8% |
| [SO5 4w/8c](../benchmark-results/2026-09-08/profiles/so5_echo-w4-c8/flamegraph.svg) | 4,186 | `combined_lock::actual_lock_t::lock` 占 42.1% | 4.3% |
| [本框架 IPC](../benchmark-results/2026-09-08/profiles/native_ipc_echo-w1-c1/flamegraph.svg) | 142 | 可见桥接/调度/分配；样本较少，不作精确拆账 | 23.2% |
| [裸 IPC](../benchmark-results/2026-09-08/profiles/raw_ipc_echo-w1-c1/flamegraph.svg) | 21 | 样本过少，不作热点比例结论 | 9.5% |

方法：`cpu-clock:u`、199 Hz、DWARF 8192 B 栈；采样包含预热/结束和继承的 IPC 子进程，吞吐正式测量不运行 profiler。
`perf script` 显式不输出 period，因此折叠权重为**每次采样 1**；八组折叠权重均等于原始 script 的事件数。
默认转换曾把 cpu-clock 周期纳秒当权重，已在最终脚本纠正，未把错误权重叫作样本数。

局限很重要：这是**用户态 on-CPU**，看不到内核执行和睡眠时间；不能把火焰图宽度当成端到端延迟贡献。
IPC 多为系统调用和阻塞，所以用户态样本很少。近乎所有栈包含未解析的 libc/libstdc++ 外层启动帧，
上表另列更有意义的未解析**叶节点**比例。独立 ucontext 栈可能在 trampoline 截断；内联帧、模板/重载折叠也影响 inclusive 归属。
分析优先用可识别叶节点与运行库代码交叉核对，不从不完整栈推出精确收益。

## 3. 差距归因：确认到哪一层

### A. 全局临界区和每消息调度：高优先级、有测量支持

[`ActorSystem::send`](../actor_runtime/src/actor_system.cpp) 的 registry mutex 覆盖了查询、消息分配、mailbox 入队和全局 job 入队；
每条消息都对应一个 DispatchMessage job。worker 还要拿队列锁、Actor execution mutex、mailbox 锁。
`processJob` 抢不到 execution mutex 时重新入队并睡 50 µs。
单 worker 也支付多次锁操作，多 worker 又增加共享锁、唤醒和重新竞争。

证据：五轮 4w/8c 本框架 CPU 秒/墙钟秒仅约 1.80（CAF 3.77，SO5 4.00）；profile 中已见 mutex 热点，
且本框架 4w/8c 的采样运行约 4.60 s 内发生 897,211 次自愿 OS 上下文切换。
这是调度/同步成本显著的证据；**尚未把每把锁的等待时间分别测出来**，也未证明全部差距来自 registry mutex。

### B. 本地 ready queue 唤醒条件遗漏：代码证据 + 支持性对照

`enqueueLocal` 把 ResumeFiber 放到 owner 的 localQueue 后 `notify_all()`；
但 `workerLoop` 的条件变量 predicate 只看 stopping/globalQueue，不看该 worker 的 localQueue。
通知到来时若全局队列为空，predicate 仍为 false，可继续等到 10 ms 周期超时。
这解释了“有工作却睡着”的可能路径，与 10 ms 级 RTT 长尾一致。

配对诊断 `wake_probe` 保持 4 worker、1 客户端、40,000 请求、全量 RTT 样本，只改变是否每毫秒向无关 Actor 发一条全局消息：

| 轮次 | 无 pulse RPS / p99 µs | 有 pulse RPS / p99 µs |
|---|---:|---:|
| 1 | 10,060 / 347.731 | 30,175 / 382.982 |
| 2（顺序反转） | 10,641 / 303.432 | 48,303 / 285.584 |
| 3 | 5,785 / 10,178.569 | 38,580 / 304.599 |

无关消息能提高吞吐，支持唤醒路径假设；但 pulse 同时扰动调度和锁竞争，而且前两轮 p99 并未一致下降。
因此这是诊断证据，**不是修复验收**，不建议在生产中加定时空消息。

### C. 分配、any 和栈切换：真实成本，但不应最先盲改

单 worker 的 allocator/any 叶节点热点低于 mutex 合计；fiber 的 `swapcontext` 是明确的新增热点。
当前 fiber benchmark 已复用长存栈，所以不能凭它建议“栈池一定最有效”。
关闭绝大多数时间戳采样的五轮敏感性实验为 201.3 万 RPS，正常 1/16 采样为 198.6 万（约 1.4% 中位差，范围重叠）；
计时开销不足以解释多 worker 数倍差距。仅采一次的控制组 p99 无统计意义，不用于延迟比较。

## 4. 优化顺序与验收方式（建议，未实施）

| 优先级 | 建议 | 需要的安全边界和验证 |
|---|---|---|
| P0 | 修正 owner-local ready 的通知/等待协议；考虑每 worker epoch/计数或明确就绪谓词 | 先构造 owner 睡眠后定向恢复的回归；检查锁顺序、丢通知、取消、定时器和 shutdown；重复 wake probe，消除可归因的 10 ms 台阶 |
| P1 | 每 Actor 至多一个 ready token；按 Actor 批量 drain 消息，避免同 Actor job 重复竞争与 50 µs 退避 | batch1/8/32/64 对照；保证 Actor 串行、无丢消息/重复消息；同时测热点、长 handler 下公平性和 p99，不只追吞吐 |
| P1 | 缩小 registry 临界区，再评估缓存稳定 actor control/分片或 worker-local ready queues | 与注销/同名重建/跨系统 ActorRef 生命周期保持一致；8/64 对同拓扑扩展性应有改善；用锁等待/off-CPU 证据验证 |
| P2 | 减少 Message/WaitRegistration/any 节点分配；评估小对象存储和安全复用 | 先做独立分配计数；覆盖异常、取消、迟到回复；不能把 owning payload 改成悬空引用来换性能 |
| P2 | IPC 复用已解析地址/编码缓冲，减少 bridge 的额外线程交接 | 另做同进程 C bridge 对照与 syscall/off-CPU profiling；保持协议校验、超时与多客户端隔离；不承诺可达到裸 socket |

每个优化单独提交并复跑同一矩阵；不同时更换 allocator、调度器和消息模型。没有实施的优化不报收益百分比。

## 5. 方法、公平性和复现边界

- Intel Core Ultra 5 125H；WSL 暴露 18 logical CPUs；Ubuntu 24.04；内核 5.15.167.4-microsoft-standard-WSL2。
- GCC 13.3.0 / CMake 3.28.3；C++17，RelWithDebInfo：`-O2 -g -DNDEBUG -fno-omit-frame-pointer`；CAF 另有 PIC/警告参数。所有实际 flags 已归档。
- 单进程 Actor 三方都是长期存活对象、同逻辑 payload/拓扑/窗口；CAF 使用 stealing，显式 max-threads/max-throughput；SO5 thread_pool、individual FIFO、显式 demands batch。
- CAF 0.19.6 / SO5 5.8.3 是固定 C++17 对照版本，**不是最新版排名**。各自类型系统、地址句柄、复制/引用计数策略不同，这些是适配器+框架的总成本，不宣称每个分配动作完全等量。
- 所有测量串行、亲和集合 0–7；未与编译/其他基准并跑。Windows 背景负载、CPU 频率/功耗和虚拟化未锁定，CAF/raw IPC 等仍有跨轮波动。
- worker 指 Actor scheduler 数；不等于全进程线程数。运行库另有 blocking pool，IPC 另有收包线程和同步客户端线程；裸 IPC 没有相同线程结构。
- 53 配置先短测和一次较长校准，按框架分别估算约 1 s 的请求数，随后三轮固定该数。不能称初轮所有框架请求预算相同。补测则同拓扑使用相同固定请求预算、五轮随机顺序。
- 构造 Actor/系统和析构不计入正式时间；预热单独；测量包含 Start 消息发送、样本 reserve/收集。IPC 包含每阶段客户端线程启动，连接和桥接句柄预先创建。全部不启用 ASan/coverage/逐消息日志。
- 闭环每次回复后补发；RTT 每 16 个序号采一个，某些诊断全量采样。没有开放环过载、协调遗漏校正、客户端公平性或生产 SLA 测量。
- `cpu_seconds` 为 user+system 的 RUSAGE_SELF；IPC 不含 server 子进程，不能拿它代表整个 IPC 系统 CPU。RSS 是进程累计峰值，不是阶段增量；OS 上下文切换不包括用户态 swapcontext。
- 运行库等待超时与 IPC 单次 5 s 超时之外，Python runner 对自建进程组有 120 s 截止，包含自己 fork 的子进程；失败记录不计入汇总。

依赖/工具安装完成：Linux perf tools、Ninja、gperftools（后备工具，最终使用 perf）。未降低系统 `perf_event_paranoid=2`。
本次没有重跑覆盖率统计，也不把性能测试冒充覆盖率报告。

复现命令和程序入口见 [`benchmarks/README.md`](../actor_runtime/benchmarks/README.md)。
完整数表见 [`matrix.md`](../benchmark-results/2026-09-08/matrix.md) / [`matrix.csv`](../benchmark-results/2026-09-08/matrix.csv)。
官方 API/调度/采样依据见 [`BENCHMARK_RESEARCH.md`](BENCHMARK_RESEARCH.md)，包括 [CAF scheduler](https://actor-framework.readthedocs.io/en/0.19.6/Scheduler.html)、[SO5 dispatchers](https://github.com/Stiffstream/sobjectizer/wiki/SO-5.8-InDepth-Dispatchers)、[perf-record](https://man7.org/linux/man-pages/man1/perf-record.1.html) 和 [FlameGraph](https://github.com/brendangregg/FlameGraph)。

## 6. 后续业务基准设计（未执行，不计入本轮完成数量）

1. 订单 fan-out/fan-in：库存、风控、支付并行，按 Any/All、取消、迟到回复测吞吐及端到端 p99。
2. blocking pool 饱和：注入不同服务时间/队列容量，观察普通 echo 隔离、拒绝率、超时后的后台占用。
3. 定时器/超时风暴：同时 1k/10k 等待，测到期漂移、取消清理与普通消息公平性。
4. Actor/fiber 生命周期：单独测创建/完成/注销速率、栈映射与内存增长，不能由长存 echo 外推。
5. 开放环固定到达率 + 慢消费者：报告排队长度、丢弃/背压、饱和拐点；做 30–60 分钟 RSS/完成记录稳定性测试。

当前研究和性能诊断采用了官方资料核对、先建立基线再验证假设的方法；这些约束使结果区分了已测事实、支持性归因和未执行的优化建议。
