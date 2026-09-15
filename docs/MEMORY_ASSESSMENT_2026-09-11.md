# Actor runtime 运行时内存评估

日期：2026-09-11。评估版本：`ad2389b10794b7b84ae0b103f58182a9fe7e7007`（dev）。

## 结论

当前库的基础占用不大，但尚不能视为“长期运行、内存有界”的服务运行时。主要风险不是默认 256 KiB fiber 栈，而是无界消息积压、已完成工作的结果保留，以及失效等待登记的延迟清理。

本机 WSL 最终执行 **25 个场景 × 3 个独立进程，共 75 次，全部通过探针断言**。这里的“通过”表示场景执行、数量核对和数据采集正确，**不表示内存风险已修复**。

| 核心指标 | 本机实测结论 |
|---|---|
| 空载进程 RSS | 1 worker 中位数 3.91 MiB；默认 18 workers 中位数 8.07 MiB，存在进程间波动 |
| 空 Actor | 约 1.415 KiB/个 RSS 增量，含探针保存的 16 B ActorRef |
| 浅栈等待 fiber | 约 5.79 KiB/个 RSS 增量；256 KiB 可用栈对应 264 KiB 虚拟映射 |
| 10 万条消息，独占 1 KiB payload/条 | RSS 增加 117.57 MiB |
| 9000 个已完成 root fibers，各返回 4 KiB | live stacks 已为 0，结果表仍有 9000 项，RSS 增加 39.94 MiB |
| 长寿命 fiber 完成 3000 次 blocking，各返回 4 KiB | 结果表仍有 3000 项，RSS 增加 12.17 MiB；fiber 退出后才清理 |
| 10 万个失效消息等待项 | fiber 退出后仍有 10 万项，RSS 增量约 10.48 MiB |
| 10 万个失效远期 timer | 原 Actor 停止后仍有 10 万项；队列相关 malloc mmap 分配约 7 MiB |

工程判断：适合继续开发、教学和有明确并发/积压限制的业务验证；上线长期驻留的高请求量服务前，应先做结果生命周期、失效等待清理和背压。**这不是与 CAF/SObjectizer 等框架的同口径内存排名**，本轮没有运行其他框架。

## 1. 方法、环境与边界

- Windows 上的开发 worktree，经 WSL Ubuntu-24.04 执行；未改动教学 worktree。
- Intel Core Ultra 5 125H，WSL 可见 18 逻辑 CPU；Linux `5.15.167.4-microsoft-standard-WSL2`；约 15.38 GiB WSL 内存。
- GCC 13.3.0、glibc 2.39；运行库重新构建为 RelWithDebInfo，`-O2 -g -DNDEBUG`，无 ASan/UBSan、无 coverage 插桩。探针 `-O2 -g -fno-omit-frame-pointer`。
- 页大小 4 KiB，实测 pthread 默认栈 8 MiB，`vm.max_map_count=1048576`。每个系统默认另有 2 个 blocking threads。
- 每个配置运行 3 次新进程，串行测量。用消息/原子计数/完成 future 保证阶段到达，采样前等待 20 ms；不把计时 sleep 当作业务完成判据。
- 除空载 worker 扩展外，负载采用 1 worker，以便在 Actor handler 内安全读取内部表；内部检查只读，不修改运行库。IPC 未开启。
- 记录 RSS、PSS、Private_Dirty、VmSize、VmHWM、线程数、VMA 数、malloc 指标、live fibers/stacks 和可安全观测的持有表项。`counts_observed=0` 时对象表计数的 0 是占位值，**不能解释为已清空**；`live_fibers/live_stacks` 不受此标记影响。
- 表中增量先在同一次运行内减基线，再取三次中位数，不是随意相减不同进程 RSS。Actor 基线是 system_idle，其余负载是 actor_ready。三次 min/max 保存在 summary.json。

RSS 是实际驻留页，包含共享代码页；PSS 将共享页按比例分摊；VmSize 是虚拟地址空间，不能等同物理内存。报告以 RSS 增量为容量依据，不把三者相加。[Linux smaps 定义](https://man7.org/linux/man-pages/man5/proc_pid_smaps.5.html)

`mallinfo2` 仅作为分配/释放方向的辅助证据：它不是整个进程、整个库或所有 arena 的完整分配账本，也不包含库自己直接 mmap 的 fiber 栈。持有问题由**源码路径 + 实际表项数量 + RSS 趋势**共同确认。[mallinfo2 限制](https://man7.org/linux/man-pages/man3/mallinfo.3.html)

限制：这是分阶段驻留量和有限轮次回收实验，不是连续瞬时峰值测量、24 小时泄漏认证或生产内存上限保证。没有覆盖 socket 内核缓冲、跨进程 IPC/C API 大 payload、真实数据库/序列化库、海量并发 blocking 捕获数据、深递归及异常展开的栈峰值。探针、libstdc++、线程库均包含在进程绝对值中。未关闭宿主机其他应用；空载绝对 RSS 波动明显，负载配对增量更稳定。不能将本机数字直接当成其他编译器/分配器下的常数。

## 2. 空载与 Actor 成本

| workers | 总线程数（含主线程和 blocking） | RSS 中位数 [min, max]，KiB | PSS 中位数，KiB | VmSize 中位数，KiB |
|---:|---:|---:|---:|---:|
| 1 | 4 | 4004 [3992, 6036] | 1391 | 96664 |
| 4 | 7 | 6016 [4064, 6024] | 3459 | 317860 |
| 18（配置为 0） | 21 | 8264 [6224, 8268] | 5703 | 1350240 |

相对“尚未创建 ActorSystem”的配对 RSS 增量中位数分别为 308、2296、4576 KiB。绝对值存在约 MiB 级波动，不据此拟合精确的每线程 RSS 成本。

默认配置的 VmSize 约 1.29 GiB，**不是吃掉 1.29 GiB RAM**。线程栈及分配器地址预留都可能贡献虚拟空间；本轮没有将每个 VMA 进一步归因，不能把全部增量算给 pthread 栈。Linux 线程默认栈与进程启动时 RLIMIT_STACK 有关，本机测得为 8 MiB。[pthread 栈规则](https://man7.org/linux/man-pages/man3/pthread_create.3.html)

| 空 Actor 数 | RSS 增量，KiB | 折算 KiB/Actor |
|---:|---:|---:|
| 1000 | 1420 | 1.420 |
| 10000 | 14148 | 1.415 |

两个配置三次 RSS 增量均相同。包含 registry、控制块、空 mailbox 的底层分配、promise/future、Actor 对象等；探针另保存每 Actor 一个 16 B ActorRef，扣除此项后约 1.40 KiB/Actor，仍不是逐项精确归因。业务字段、命名字符串及业务缓存另计。

编译期对象尺寸（不是总堆成本）：ActorControl 568 B，EmptyActor 16 B，Message 112 B，Fiber 1240 B，Worker 1352 B，RuntimeWaitNode 144 B，Completion 48 B，TimerEntry 56 B，ReadyDispatch 32 B，Job 88 B，ActorSystem::Impl 4736 B。容器后备存储和指针指向的对象不包含在 sizeof 中。

## 3. 消息积压：必须按条数和字节双重控制

场景：先让消费者 handler 等待闸门，发送端填满 mailbox，再开闸消费；payload 为逐条独占的 `vector<unsigned char>`，检查消费数量与首尾内容。闸门阻塞 worker 是测量手段，不是建议的生产编程方式。

| 积压数 | 每条 payload | RSS 增量，KiB | MiB | 折算 B/条（含 payload） |
|---:|---:|---:|---:|---:|
| 10000 | 无 payload | 1572 | 1.54 | 160.97 |
| 100000 | 无 payload | 15704 | 15.34 | 160.81 |
| 10000 | 64 B | 2668 | 2.61 | 273.20 |
| 100000 | 64 B | 26644 | 26.02 | 272.83 |
| 10000 | 1024 B | 12044 | 11.76 | 1233.31 |
| 100000 | 1024 B | 120396 | 117.57 | 1232.86 |

各行三次 RSS 增量一致。非空 vector payload 场景约为 `payload + 209 B/条`，空 payload 约 161 B/条。差异包含 any/vector 的额外分配、分配器粒度、shared ownership 和队列存储；换成小对象 any 或共享大 buffer 后不能直接沿用该系数。

当前 mailbox 没有容量上限。只要生产速度持续超过消费速度，即使不使用 fiber，也没有固定内存上界。10 万条 1 KiB payload 的业务突发，单是队列就约 118 MiB，而非仅算 payload 得出的约 98 MiB。

## 4. Fiber：区分可用栈、映射和实际触碰

场景：同一 Actor 同时挂起 N 个 root fibers，等待消息；touch=64 KiB 时用 volatile 逐页写入并在恢复后核对。touch=0 仅表示没有额外压栈，运行时和 await 本身仍使用栈。

| 同时挂起数 | 可用栈/个 | 额外触碰/个 | RSS 增量，KiB | VmSize 增量，KiB | VMA 增量 |
|---:|---:|---:|---:|---:|---:|
| 1000 | 256 KiB | 0 | 5792 | 264000 | 2002 |
| 2000 | 256 KiB | 0 | 11580 | 528000 | 4002 |
| 1000 | 256 KiB | 64 KiB | 69792 | 264000 | 2002 |
| 1000 | 64 KiB | 0 | 5792 | 72000 | 2002 |

各行三次增量一致。默认栈是 256 KiB 可用区 + 两个 4 KiB guard pages，总映射 264 KiB；源码 `actor_system.cpp:110` 附近实现 mmap/mprotect，析构 munmap。

浅等待约 5.79 KiB RSS/fiber；额外触碰 64 KiB 后约 69.79 KiB/fiber。将可用栈从 256 KiB 降到 64 KiB，为 1000 fibers 节省 **187.5 MiB 虚拟空间，但本场景 RSS 没有下降**。所以“统一调小栈就能省掉几百 MiB RAM”不成立。

完成后所有 live fibers/stacks 均归零，VmSize 中的 fiber 映射消失；但 RSS 不必回到 actor_ready，因为结果表和普通堆缓存仍在。1000 个浅等待 fiber 的 released 阶段仍有 1000 个 completion，RSS 增量为 1968 KiB。

VMA 合并使实测数量接近 2N，不应固定按 3N 估算；实际数量仍随存活 fiber 线性增加。更小栈并未减少本场景 VMA 数。最大并发还需受实际栈深度、地址空间和目标机器 VMA 限额约束。

## 5. 长期保留风险：工作结束不等于内存生命周期结束

### 5.1 所有 fiber completion 保存到 Actor 停止

`actor_system.cpp:679` 的 finishFiber 将结果写入 `actor->completions`，包括 parentId=0 的 root fiber。查到的整体清理点为 Actor 停止路径 `:557`；fiber 栈销毁不会同步删除 completion。

在同一个长寿命 Actor 上分三轮完成 root fibers，业务端不持有返回值：

| 每轮 3000 个、每个返回 4 KiB | 实测 completion 数 | live stacks | RSS 配对增量，KiB |
|---|---:|---:|---:|
| 第 1 轮后 | 3000 | 0 | 16620 |
| 第 2 轮后 | 6000 | 0 | 28720 |
| 第 3 轮后 | 9000 | 0 | 40896 [40860, 40896] |

前两行三次增量一致。后续每新增 3000 项约再增加 12 MiB RSS，表项数严格累加。空结果对照也从 1000 → 2000 → 3000 项，RSS 增量 1508 → 1620 → 1728 KiB，说明不仅大 payload 才有问题。

这属于**可达数据的生命周期保留**，不是已经证实的“丢失指针、永不释放”泄漏；但在长期 Actor 不退出的业务中，随累计完成数增长，仍可造成内存耗尽。Actor 停止后分配指标明显下降，剩余 RSS 中又混入分配器缓存。需要修正的是生命周期边界，而不是只看 liveStacks=0 就宣布回收正常。

### 5.2 长寿命 fiber 的 blocking task 结果

单个 fiber 顺序执行并 await 已成功完成的 blocking，每次结果 4 KiB，之后继续等待消息而不退出：

| 已完成次数 | tasks 实测数量 | RSS 增量，KiB [min, max] |
|---:|---:|---:|
| 1000 | 1000 | 4176 [4176, 4180] |
| 3000 | 3000 | 12460 [12456, 12460] |

释放该 fiber 后 tasks 数实测归零。源码 `finishFiber:685` 按 parentId 扫描并删除 task，await 本身不消费结果。因此后台常驻轮询协程即使一次只做一个 blocking，也会保留历史结果直到自己结束。blockingQueueCapacity=64 只限制待执行工作条数，**不限制这些已完成结果，也不等于字节上限**。

### 5.3 已经输掉 Any 的等待登记

确定性构造：重复 `Any(未来 10 分钟 timer, 已过期 timer)` 或 `Any(新 correlationId 的消息, 已过期 timer)`。后一个分支立即获胜。这里测的是失效登记的生命周期，不是实际网络吞吐，也没有等满 10 分钟。

| 失效登记 | 次数 | fiber 退出后实测保留项 | held 阶段 RSS 增量，KiB [min, max] |
|---|---:|---:|---:|
| 不同消息 key | 10000 | 10000 | 1028 [1028, 1028] |
| 不同消息 key | 100000 | 100000 | 10736 [10736, 10736] |
| 远期 timer | 10000 | 10000 | 684 [684, 684] |
| 远期 timer | 100000 | 100000 | 5920 [5868, 6432] |

消息场景 held 另有 1 项真正的释放消息等待；退出后这 1 项清掉，10 万项旧登记仍在。源码 `installWait:755` 先登记叶子，再判断组合是否已完成；`notifyMessage:868` 只有相应 key 收到消息时才顺便剔除旧引用。不断变化且不再收到消息的 correlationId 会使旧 key 留到 Actor 停止。

timer 在 worker priority_queue 中。新增一个只读观察 Actor 后确认：**原 Actor 已 stop，10 万个 timer 仍存在**。这不是 ActorRef 把旧 Actor 强持有造成的；TimerEntry 使用 weak_ptr，但队列条目及弱引用相关分配仍占空间。源码 `:391` 只在 deadline 到达时 pop；本轮没有测到期后的缩容，不能声称届时 RSS 一定下降。priority_queue 的底层 vector 容量还可能保留高水位。

10 万 timer 的 malloc mmap 分配为 7344128 B（约 7 MiB），shutdown 后仍在，ActorSystem 析构后消失。不要只看 uordblks 约 0.1 MiB 就漏掉这部分。对固定最大超时的稳定流量，过期前的登记数量与“请求速率 × 超时窗口”相关；对不断增长的超时窗口或流量，没有现成总量限制。

## 6. 回收：RSS 不下降不是单一含义

| 实验 | 观察 | 判断 |
|---|---|---|
| 注册/停止 10000 空 Actor，循环 5 轮 | 第 2 至第 5 轮停止后的 RSS 均为 18096 KiB；uordblks 约 0.19 MiB 而非 14 MiB | 未看到按整轮 Actor 内存累加；有限轮次结果支持主要为堆高水位复用，不能替代长稳测试 |
| 10 万条 1 KiB 消息排空 | RSS 仍为 124392 KiB；uordblks 从 123387264 B 降至 150480 B | 消息已消费，剩余 RSS 主要是分配器持有空闲页的证据，不应报成 118 MiB 消息泄漏 |
| 同一消息实验，系统析构后做一次诊断 trim | RSS 中位数降到 4204 KiB | 大部分驻留页可归还；不是还被业务消息引用 |
| 10 万失效消息登记，Actor 停止/系统析构后 trim | trim 后 RSS 仍约 13512 KiB，uordblks 约 89 KB | trim 并不保证归还所有空闲页；不能单凭 trim 无效推断对象泄漏 |

`malloc_trim(0)` 仅在系统销毁后的探针尾部调用，业务阶段数据均为调用前采样。它尝试归还可释放空闲内存，不是保证全部归还；不建议作为每条消息后的“修复”。[malloc_trim 行为](https://man7.org/linux/man-pages/man3/malloc_trim.3.html)

## 7. 优化优先级与验收建议

以下为后续方案，**本轮均未实施，收益不是已兑现数字**。

| 优先级 | 建议 | 依据与代价 | 应补的业务验收 |
|---|---|---|---|
| P0 | 明确 root/child completion 的回收边界；不再无条件长期缓存 root 返回值 | 可消除当前按总完成数增长的大结果保留。必须保留错误回调、child 晚 await/重复 await 等契约，不能直接把所有结果 erase | 同一个订单 Actor 连续完成 10 万次、每次 4 KiB 结果；不重启 Actor，保留表和稳态内存不随历史总数线性增长 |
| P0 | 给 blocking 结果定义 consume/forget 或明确的 owner 释放策略 | 常驻 fiber 不应默认积累所有历史查询结果；改动需验证是否允许重复 await | 常驻数据库轮询 fiber 完成 10 万次查询仍不退出；已消费结果有界；同时覆盖超时、取消、晚完成 |
| P0 | 等待完成/取消时撤销失效登记，或采用有明确阈值的安全压缩 | 当前不同消息 key 和远期 timer 可大量保留；需要反向登记或压缩策略，保留 token 校验、防止重复恢复和广播迭代失效 | RPC 使用唯一 correlationId，99% 快速回复、1% 超时；百万次 Any/All/取消混合后失效条目有界，旧 timer 不唤醒新一轮 await |
| P0 | mailbox 按条数及估算字节双重背压，规定拒绝/重试策略 | 当前 1 KiB 消息约 1233 B/条；拒绝路径必须显式反馈，不能静默丢消息。std::any 不能自动准确计算深层大小，需 envelope 大小信息或业务提供估算 | 下游停顿 30 秒、生产速率翻倍；队列不突破预算、拒绝数可观测，恢复后成功接收消息不丢不重 |
| P1 | 将 event-only Actor 不使用的 fiber/wait/task 状态延迟分配；检查空 deque 的最低分配成本 | 1 万空 Actor 约 13.8 MiB，有优化空间；可能增加首次协程启动成本和指针访问，需测量后决定 | 10 万主要空闲会话；同时回归事件吞吐、首次 await 延迟和生命周期 |
| P1 | 栈配置按业务分级、限制 live fibers，并采样实际栈高水位 | 64 KiB 配置在本次浅栈测试只省虚拟空间，不省 RSS；需要覆盖序列化、递归和异常路径后才能降栈 | 正常/异常最深调用链、取消展开、guard page 溢出隔离；不以本探针浅调用通过代替安全裕量 |
| P1 | 加入 mailbox bytes/depth、completion/task 数与字节、有效/失效 waiter/timer、stack reserved/touched 指标 | 仅 liveFibers/liveStacks 无法发现本报告的保留问题；字节估算需声明精度 | 在上述业务场景中能由监控定位增长来源，不能只报总 RSS |
| P2 | 按需创建 blocking pool；配置 worker 数时结合实际负载 | 纯事件系统也创建默认 2 个 blocking threads；优化影响首任务延迟，不能仅按 CPU 数推导最优值 | 纯事件与首次 blocking 分别测 RSS、线程数、p99 和吞吐 |
| P2 | 清理无界生命周期后，再评估消息/等待节点池、分配器和释放阈值 | 池化可能减少碎片，也可能提高常驻高水位；不修改全局 malloc/pthread 策略来掩盖持有问题 | 流量突发后回落、多轮稳态、高水位与 p99 联合 A/B，保留回退开关 |

验收顺序建议：先给上述失败模式建立有界性测试，再改实现，然后同时跑等待语义、取消/停止并发、ASan 和吞吐回归。本轮探针中的“旧结果必须仍有 N 项”断言用于确认现版本事实；修复后应改为新生命周期契约，不能把旧行为断言当成永久要求。

## 8. 初步容量估算与业务含义

仅适用于本报告的 payload 表示和浅等待模型，可用于确定下一轮压测规模：

```text
RSS ≈ 实测服务基线
    + Actor 数 × 约 1.4 KiB
    + 积压消息数 ×（独占 vector payload 字节数 + 约 209 B）
    + 活跃浅等待 fibers × 约 5.8 KiB
    + 实际额外触碰的 fiber 栈页
    + 保留结果/等待索引/定时器
    + 业务对象、I/O 缓冲、碎片及分配器缓存
```

举例：1 万会话 Actor、10 万条 1 KiB 积压、1000 个浅等待 fiber，仅这三项约 **137 MiB 增量**，尚不含基线及历史结果。相同 1000 个 fiber 各额外触碰 64 KiB，再加 62.5 MiB。这个是分项外推，不是混合业务实测；组合负载可能改变 allocator 行为。

完成结果、长寿命 task 和旧 waiter 的保留未解决前，**不能只根据同时在线数给出一个长期安全内存额度**。若按工程假设再留 30% 余量，也只是下一轮压测起点，不是生产 SLA/OOM 保证。生产容器还要计入内核缓冲、页缓存等进程 RSS 之外的收费项，本轮未测。

## 9. 交付与复现

- 探针：[memory_probe.cpp](../actor_runtime/benchmarks/memory_probe.cpp)。
- 串行运行器：[run_memory.py](../actor_runtime/benchmarks/run_memory.py)。
- 最终证据目录：[2026-09-11-memory](../benchmark-results/2026-09-11-memory/README.md)：75 次原始 stdout、返回码、命令，汇总、环境/源码与二进制 SHA-256、验证结果。
- 测量期间运行库 src/include 哈希不变；仅新增评估探针、脚本、报告和证据，未修改运行库实现、未提交或推送 GitHub。

完整构建和运行命令见证据目录 README。不要把 WSL 临时 build 目录或探针二进制混入发布库。
