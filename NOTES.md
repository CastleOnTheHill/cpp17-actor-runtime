# 教学偏好与后续辅导约定

## 已确认
- 用户有 C++ 基础，Actor 概念掌握较好。
- 希望教程详细、可以很多章、不限制长期学习时间；已要求规划全程、实现前三章，并于 2026-09-06 追加编写第 4–8 章。
- 用户本轮明确自述“构建者，从比较底层开始往上掌握”。已核对历史「学习 Linux trace 与 eBPF」及其 NOTES.md：还原论、最小机制、因果链、单一原子问题；不是泛泛的“喜欢动手”。

## 固定教学协议（从历史恢复）
- 底层事实 → 源码/ABI → 必要的数学与量纲推导 → 最小实验 → 一道闭卷题。没有定量问题时不硬塞公式。
- 先回答“对象是否真实存在、保存什么状态、何时更新”。实现分支和工具放可选层，不把“底层”误解为一次灌输全部汇编。
- 每次只推进一个原子问题；章节可以详细，但拆成短单元，未过关不推进。时间不限。
- 反馈指出具体正确步骤和第一个因果断裂点，在该点最小纠正，再做一次检索练习。
- 以自己构建可运行部件为主线；先预测再运行，参考实现默认折叠；最终将部件组合到本仓库框架。
- QEMU 学习记录显示用户重视车载 Linux/TBOX 真实需求。后期综合项目可用 CAN、Modem、休眠协调模拟；不要推断已掌握相关内核机制。
- 1/3/7 天复述仅是可调整建议，不是强制计划或记忆效果保证；没有创建提醒。

## 当前材料与学习状态
- 第 1–8 章材料已编写；第 9–24 章是规划。每章一个核心问题，源码细节置于可选证据层。
- 用户第二章练习已有前两段实现，完成态分支仍待学习者补充。本轮不替用户完成该练习，不记作已过关。
- 所有框架映射以 teaching 分支为准，不混用 dev 的新增 IPC/C API/动态 Actor 功能。

## 辅导协议
每次请用户提交：章节/练习编号、运行前预测、实际输出、解释、改动 diff、最不确定的问题。
先检查解释和反例，再给下一步；不要一次给完整答案。需要时提供逐级提示。
练习页勾选只代表自评；没有用户证据不能写成“已掌握”。当前只交付材料，尚无用户完成章节的证据。
历史证据来源：相邻工作区 trace学习/NOTES.md；对应历史任务「学习 Linux trace 与 eBPF」；qemu学习/NOTES.md。此处只复制教学偏好，不复制其他项目的学习成绩。

## 2026-09-14 恢复点：P0 等待登记清理

- 用户要求恢复工程目标：先不切换 timerfd，实施四项 P0，性能不能劣化；禁止改写 git 历史、提交或推送。仍按原子问题教学，不以代理执行测试代替学习者过关。
- 本轮已读 MISSION.md、NOTES.md、COURSE.md 和当时最新的 `learning-records/0001-starting-point.md`。这些文件此前没有近期 P0 停点，因此通过 dev worktree 和 WSL 重新核验。教学代码与第 2 章在途练习未修改。
- 开发目录为 `C:\Users\icema\Documents\ChatGPT\下一代ipc框架-dev`，HEAD 仍以已有提交为基线，存在未提交 P0 实现；不能按旧方案标题“未实施”推断工作区没有修改。2026-09-11 的 MEMORY_ASSESSMENT 四项 P0 和 09-13 的细化方案仍为完整验收范围。
- 实际中断残留：CMake 引用了 `wait_cleanup_tests.cpp`，文件却尚未创建。没有发现仍运行的相关 WSL 构建/测试。本轮补齐此文件并配置其内部诊断头文件路径，恢复构建；没有扩大到 mailbox 改造。
- 本轮原子问题：一次 Any 结束后，失效消息登记与远期 timer 是否被实际删除，而非仅标记 inactive？测试在 owner worker、Actor 持有执行权时读取资源数量；这是资源契约诊断，原有公开等待语义测试仍保留。
- 新测试在 1/4 worker 各做 10000 次即时 Any、1000 次部分登记失败回滚、一次真实消息胜出、一次真实 timer 胜出，每次断言登记清空。使用同一测试、旧版本头文件和静态库，负对照退出 1：`message registration retained`。当前完整 CTest 14/14，专项重复 20/20。计时只用于记录运行，不构成性能对比。
- 证据日志：`../下一代ipc框架-dev/benchmark-results/2026-09-14-wait-cleanup/`；WSL 输出在 `/tmp/actor-p0-20260914-wait-cleanup/`，现候选构建在 `/tmp/actor-p0-20260913/candidate-build`，冻结基线源码/构建在同父目录的 baseline-source/baseline-build。
- P0 状态：结果生命周期、forgetChild/forgetTask、blocking 自动放弃结果、可撤销 timer 堆与等待回滚已有实现；本轮只补核等待清理。显式 forget/晚完成等全面验证、timer 大量乱序撤销/容量/跨 worker 取消验证、mailbox 条数/字节/系统总预算及 trySend、完整内存与 ASan/覆盖率及同机性能 A/B 均未完成验收。不能宣布“P0 全部完成”或“性能不劣化”。
- 下一教学检索题：若只改 `active=false` 而不删除索引/堆项，分别说明错误唤醒和内存增长会怎样。尚未收到独立作答，不记为掌握。原子单元说明见 `lessons/0009-p0-wait-cleanup.html`；该编号是补充材料序号，不表示第 9 章课程已完成或学习者已进入第 9 章。
- 后续先核查以上证据与当前差异，保持完整工程目标；每次只讲一个原子问题，不悄悄放宽性能约束，也不把一次绿色测试当作全部完成。

## 2026-09-14 续接点：mailbox 准入候选已编译，测试落地受审批超时阻断

- 本次再次读取 MISSION.md、NOTES.md、COURSE.md、最新 LR-0002，以及 09-11 评估中的 P0 和 09-13 细化方案。使用 codebase-design 收敛发送 Interface；按 teach/已约定协议区分工程验证和学习过关。LR-0002 检索题仍未得到学习者回答，第二章练习与课程过关状态不变。
- 本轮已先汇报原子问题：一条消息何时占用与归还 mailbox 容量。实际对象是待处理 Message（包括已进入 ready/global 调度队列的 dispatch token）；状态是每 Actor 的条数、估算字节和系统总估算字节；入队前占用，获得 Actor 执行权后、通知等待者/调用 handler 前归还，Actor 停止时清理 deque 中未处理消息。运行中 handler、应用持有的 MessagePtr、控制任务不属于该队列预算。
- dev 已落地候选：ActorRef/ActorSystem 的 trySend，Accepted/Full/Stopped/MessageTooLarge（拒绝 id=0）；旧 send 超限抛错；默认每 Actor 65536 条/64 MiB，系统 256 MiB，单消息计费 16 MiB。0 限额拒绝消息。额度检查与 Actor 停止检查在同一 mailbox 锁中；系统字节用 CAS 预留，拒绝/发布异常回滚，stats 增加 queuedMailboxBytes。
- 计费不是 RSS 测量：固定 envelope 估算 256 字节，加 Bytes/string 容量与元数据长度；未知 std::any 默认仅 sizeof(any)，深层自定义数据需配置纯、线程安全 payloadSizer。共享存储按每消息重复计费。Message 增加只在发布前写入的 estimatedMailboxBytes，内部 IPC 接收创建可变 Message，准入后仍以 shared_ptr<const Message> 交给 Actor。
- 本地 sendBytes 走相同准入。跨进程接收被拒绝只记录 ipc.mailbox_rejected，尚无远程 Actor 准入 ACK/NACK；远端 sendBytes 成功不能解释成远端 Actor 已接受。控制队列不走普通消息限额，其独立资源预算仍需后续审查。
- enqueueGlobal/enqueueLocal 的 outstandingJobs 加计移到队列 push 成功后、解锁前，避免初次发布分配失败留下计数。postMessage 的初次发布失败回滚已写入；整个调度重排路径的分配失败保证还未验收，不能据此宣称全面 OOM 安全。
- WSL Ubuntu-24.04 的 `/tmp/actor-p0-20260913/candidate-build` 已完整编译成功，退出 0；仍有原有 ipc_transport.cpp 忽略 write 返回值警告。**本轮未运行成功 CTest，也没有性能 A/B**：后续 WSL CTest 请求在启动进程前因自动权限审批超时被拒绝。上一节 14/14 和 20/20 的绿色证据仅针对 mailbox 改动之前的版本，不可沿用到此候选。
- `mailbox_capacity_tests.cpp` 两次 apply_patch（初次和允许的一次重试）均因自动审批超时未落地，最终 Test-Path=False。连带的 CMake mailbox 测试注册和 message.h 显式 cstddef include 也未落地。禁止把过程中“加入测试”的预期描述记作已交付。当前 CMake 仍只有此前 14 个测试，不存在新测试缺文件导致的配置错误。
- 待补测试已设计：1 worker 被独立 Actor 闸门占用，验证目标 ready token 也算一条；8 生产者共 800 次发送竞争 64 名额，期望 64 Accepted/736 Full，并比较收到的 ID 集合与接受集合；Bytes 精确限额/超大拒绝/本地 sendBytes；自定义估算抛异常和 size_t 溢出不泄露额度；跨 Actor 总预算、消费后复用、满队列停止、关闭与空句柄。闸门 RAII 必须在 ActorSystem 析构前打开，避免断言失败后测试自身挂死；1/4 worker 覆盖适用场景。
- 后续仍需更完整的并发停止竞态、跨 Actor 同时争抢总预算、动态消费与有界重试、string 容量/零限额/异常回滚测试，再跑全套 CTest、ASan、覆盖率、内存平台期与冻结基线性能 A/B。新增锁和系统原子计费可能影响热路径；不得以“功能增加”为理由放宽性能不能劣化的原目标，也不得禁用默认预算以美化性能。
- 当前阻断是审批服务超时，不是代码被判定不安全；需用户明确允许 dev 写入和 WSL 验证后恢复。没有读取 sudo 密码、安装依赖、切换 timerfd、提交、推送或改写历史。dev HEAD 仍为 ad2389b10794b7b84ae0b103f58182a9fe7e7007，现有和本轮未提交修改均保留。目标保持 active，不冒充已完成或达到三次阻断阈值。

### 自动续接核验：第二次连续遇到相同审批阻断

- 上一目标轮属于实际进展（mailbox 候选代码和编译证据），不是已通过功能/性能验收。本轮核查 dev 状态与缺失测试文件，重试上次未启动的 WSL CTest；再次在创建进程前因自动权限审批超时失败，没有获得可等待的 CTest 进程句柄。没有反复重启某个已确认运行的测试，也没有操作机器上其他 WSL 进程。
- 在已允许的临时目录保存了公开 Interface 测试源：`C:\Users\icema\AppData\Local\Temp\actor-p0-mailbox\mailbox_capacity_tests.cpp`。这是暂存文件，**不在 dev worktree，不在 CMake 测试清单，未经编译/执行**。恢复后应先检查内容再用 apply_patch 纳入 dev，不能以临时测试存在代替验收。
- 暂存测试包括上一节设计的准入/回收场景，并增加 Bytes/string 预留容量大于内容长度、四种零限额、8 个独立 Actor 的并发系统预算竞争。1/4 worker 覆盖同 Actor 生产者并发及停止；独立阻塞 worker 的 ready-token 和跨 Actor 系统预算实验使用 1 worker，以确定性控制消费时机。
- 只读检查未在 PATH 找到 clang++、g++ 或 cl；未安装额外依赖或改用 Windows 运行来替代用户要求的 WSL 验证。dev 代码在本次续接没有继续改动。测试写入/WSL 验证审批仍需恢复；目标保持 active，目前同一阻断只累计两个连续目标轮，未达到三轮 blocked 阈值。

### 第三次连续阻断：目标标记 blocked，等待权限恢复

- 上一轮有测试源码暂存这一实际进展，但 WSL 验证始终没有启动。本轮采用最小无副作用访问检查 `wsl -d Ubuntu-24.04 -- /bin/true`，普通权限仍返回 `Wsl/Service/E_ACCESSDENIED`（退出 1）；此前升级执行请求和 dev 测试写入的自动审批均已超时。没有可等待的测试进程句柄。
- 再次核对 dev HEAD 仍为 ad2389b10794b7b84ae0b103f58182a9fe7e7007，dev 测试文件仍不存在；临时测试 SHA256 为 5769D8DFD8731525F1A13B104ADE04ED14692AAE2508309B3BE06C8A837E2BE2。没有改动、提交、推送或重写 dev 历史。
- 同一实质阻断（dev 写入/WSL 执行权限审批无法完成）已跨三个连续目标轮核实；已调用 update_goal 标记 blocked，非 complete。需要用户授权/审批通道恢复或执行环境权限改变，之后再恢复目标。完整四项 P0、保持现有 timer、性能不能劣化的要求均不变。
- 恢复后的第一步：核查权限与源码现态，将临时测试用 apply_patch 纳入 dev 和 CMake，先跑 mailbox 专项/原有全量回归，再按前述未完成清单逐项完成性能和资源验收。不要把之前 14/14 的结果当作 mailbox 候选的通过证据，也不要在权限未改变时持续重试相同操作。

### 目标重新激活后的第一次复核

- 新的继续请求到来后，get_goal 实际返回 active，因此按重新激活后的新一轮阻断审计计数，不沿用上次三轮计数。
- 本轮申请升级执行最小检查 `wsl -d Ubuntu-24.04 -- /bin/true`，自动审批仍在进程创建前超时，未取得进程句柄；不是 C++ 测试失败。普通权限 E_ACCESSDENIED 与升级执行审批超时须区分，当前缺的是可用执行通道，不是 sudo 密码。
- 只读复核 dev HEAD 和缺失的测试文件、临时测试散列均无变化；没有新的源码改动或运行验证证据。不能仅因 goal 被重新激活就推断权限恢复。本次恢复后仅一次相同阻断，按规则保持 active；继续实测仍需审批/执行通道恢复或用户提供本机测试输出。
- 重新激活后的第二轮：对上述最小升级访问检查做了一次允许的重试，仍在进程创建前审批超时。dev 测试文件仍不存在，没有新运行证据或工程进展，也没有可等待的测试句柄；不继续增加未验证改动。恢复后同一阻断累计两轮，尚不调用 blocked。学习记录的证据边界保持有效，未获得新的学习者作答。
- 重新激活后的第三轮：普通权限最小 WSL 检查仍返回 Wsl/Service/E_ACCESSDENIED（退出 1），前两轮升级审批超时也未获解决。dev 测试文件缺失、临时测试 SHA256 均未变化。已满足本次恢复后的三轮阻断阈值，再次调用 update_goal 标记 blocked。等待执行/审批通道实际恢复，不继续以自动续接和重复记录冒充进展；四项 P0 与性能不劣化仍未完成验收。

## 最新工程恢复点：mailbox 测试已落地，性能仍未验收

以下状态更新此前的审批阻断记录；以本节和实际源码/日志为准，不按旧条目推断测试仍缺失。

### 已恢复并完成的验证

- 新环境下 WSL 升级执行与 dev 文件写入曾恢复。`actor_runtime/tests/mailbox_capacity_tests.cpp` 已通过 apply_patch 纳入 dev，CMake 已注册并加入 coverage 依赖；message.h 已补显式 cstddef。codebase-design 用于集中公开 Interface 测试，diagnosing-bugs 用于建立性能失败回路和单变量实验。
- mailbox 测试覆盖 ready token 计费、条数/字节/单消息/系统预算、8 个生产者争抢 64 名额、接受/收到 ID 集合一致、Bytes/string 预留容量、自定义估算抛异常与溢出、零限额、本地 sendBytes、跨 Actor 总预算争抢、额度复用、满队列停止与关闭。
- 又补 `concurrent_drain`：4 个外部生产者共 8000 次成功发送，容量仅 4 条，生产/消费同时进行，Full 只能在有限截止时间内重试；验证不超系统预算、无丢失/重复、排空后计费为零，分别跑 1/4 worker。修改计数方式之前，该扩展测试重复 10/10 通过；修改后重复 20/20 通过。
- 最新源码（包括下文 H1/H2/H3）普通 CTest **15/15**，ASan/UBSan CTest **15/15**；sanitizer 使用 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1`。这不是 TSan 或完整竞态证明，覆盖率尚未重跑。
- 日志均在 dev 的 `benchmark-results/2026-09-14-mailbox/`：初始 full-ctest.log/mailbox-repeat.log，H1 align-ctest.log，H2 drain-before.log/returns-ctest.log/returns-repeat.log，最新 H3 type-ctest.log/asan-ctest.log。最新普通回归 2.03 秒、sanitizer 3.78 秒仅是测试耗时，不是性能对比结论。

### 固定基线、构建与复现工具

- dev HEAD 仍是 `ad2389b10794b7b84ae0b103f58182a9fe7e7007`。所有修改仍未提交、未推送，没有改写历史。未切换 timerfd；用户第二章练习与课程过关状态未改。
- 旧 `/tmp/actor-p0-*` 构建不可用，新建 `/tmp` 目录也在后续调用中消失；改用已验证跨调用存在的 WSL 根目录 `/var/tmp/actor-p0-20260914-e4S6fw`。内含 `baseline-source`、`baseline-build`、`candidate-build`、`asan-build`；恢复后仍须检查目录是否存在，不能盲信缓存。
- Windows Git worktree 指针包含 C: 路径，WSL Git 不能直接解析该 .git 文件。基线通过 **Windows Git archive 固定提交**，再在 WSL 解包。归档位于 `C:\Users\icema\AppData\Local\Temp\actor-p0-mailbox\baseline-ad2389b.tar`，SHA256 `909DC254F2A2AFE67D45C6352A85CDBA5CF5C92683623EC222F61D158CA94117`。
- 普通基线/候选均为 GCC 13.3.0、CMake RelWithDebInfo、`-fno-omit-frame-pointer`；性能驱动统一 `-O2 -g -DNDEBUG -fno-omit-frame-pointer -std=c++17 -pthread`，各自使用对应源码头文件及静态库。WSL kernel 5.15.167.4-microsoft-standard-WSL2。
- 新工具 `actor_runtime/benchmarks/compare_p0.py`：交替 A/B 顺序，保存命令、二进制 SHA256、内核/CPU 亲和性、原始 stdout/stderr/JSONL 与 summary。支持 `--case` 缩小场景，`--minimum-ratio 1.0` 在任一吞吐中位数低于基线时失败；该筛查不是完整 P0 验收矩阵。
- 使用 `wsl --exec python3 .../compare_p0.py` 和参数数组，避免内联 shell 的变量二次展开。先前 for-loop 构建曾错误生成 `echo-`，比较脚本在缺少正确二进制时拒绝启动；那些失败不构成性能数据。正确工具通过可选的 baseline-source/candidate-source 参数生成 `echo-event` 与 `echo-fiber`，驱动编译全部结束后才计时。
- 基准默认 payload 是 `bench::Packet`（内含 64 字节 string），**不是 Bytes**。默认 mailbox 预算一直开启，没有为跑分放宽配置。taskset 固定 CPU 0-7，event 共约 100 万 round trips、fiber 共约 10 万，每 client 2000 warmup，采样间隔 64，每场景 3 轮；阶段之间不与编译或 sanitizer 测试重叠。

### 性能证据与已实施的单变量优化

比值为 candidate / 固定基线的吞吐中位数，低于 1 即未通过当前严格门槛；不同阶段分别重测基线，短跑与主机噪声仍需更长复核，不能挑最好一轮宣称完成。

| 阶段 / 证据目录 | 单 worker event | 4 worker / 8 client event | 结论 |
|---|---:|---:|---|
| 初始 mailbox / screen-r1 | 0.8509 | 0.5857 | 明确回退 |
| 最小复现 / single-red | 0.8879 | 未测 | 输出 FAIL throughput gate: event-single |
| H1 / align-screen | 0.8884 | 0.7796 | 多 worker 改善，仍失败 |
| H2 / returns-screen | 0.9834 | 0.7762 | 单 worker 改善，仍失败 |
| H3 / type-screen | 0.9788 | 0.7669 | 后续已测完，未达标，已撤回 H3 |
| H4 / config-align-screen | 0.9224 | 0.8121 | 隔离 config 与 ID 后仍未达标 |

- 初始 screen-r1 另外四场景比值：event-window 0.8989、event-hotspot 0.9530、fiber-single 0.9553、fiber-parallel 1.0806。不能用其中一个大于 1 的场景代表全库不劣化。
- 按 diagnosing-bugs 先公布三个可证伪假设，再逐项实施：H1 系统预算计数和调度/ID 计数缓存行干扰；H2 消费路径新增 mailbox 锁；H3 重复 std::any 类型查询。
- **H1 已保留**：internal.h 中 `queuedMailboxBytes` 使用 alignas(64)，隔离调度/ID 计数缓存行。只有系统级布局调整，不改变额度规则。
- **H2 已保留**：ActorControl 使用 `admittedMessages/admittedBytes`（生产者持 mailboxMutex 更新）和 `returnedMessages/returnedBytes`（Actor 执行权持有者发布的 atomic 累计量）。占用为 unsigned 累计准入减累计归还；消费时不再获取 mailboxMutex。归还包括停止时丢弃的队列消息；拒绝/初次发布失败回滚准入量。读到较旧归还量只能提前拒绝，不得放宽预算。累计数回绕逻辑尚需专门反例测试，不能把常规压力跑当作回绕证明。
- **H3 已撤回**：defaultPayloadBytes 合并 payload.type() 查询的候选在后续全六场景重测中没有稳定收益；当前源码不包含此修改。其余四个比值为 event-window 0.8617、event-hotspot 1.0077、fiber-single 0.9825、fiber-parallel 0.8312，六项中五项未达严格门槛。
- 系统总预算仍逐消息使用共享 CAS/fetch_sub，是后续需要测量的候选瓶颈，尚不能当作已证实根因。其他可检验方向包括预算同步是否放大 Actor ownership 重排；不能未经测量直接删除同步、关闭预算、放宽限制或改变计费边界。

### 当时阻断（已被下方续接状态更新）

- H3 全六场景性能重测请求在创建进程前被自动审批拒绝；理由明确是 **审批端额度耗尽**，而非测试断言失败。工具同时禁止绕过拒绝。`type-screen` 目录不存在，没有新的性能进程句柄可以等待；此前构建、回归和 A/B 句柄均已结束。
- 当前 candidate-build 静态库和测试程序包含 H3，但 `echo-event/echo-fiber` 仍对应 H2 的上一轮驱动。恢复后必须用 compare_p0.py 的两个 source 参数重编驱动，不能拿旧二进制给 H3 背书。
- 恢复命令的关键参数：baseline=`/var/tmp/actor-p0-20260914-e4S6fw/baseline-build`；candidate=`.../candidate-build`；baseline-source=`.../baseline-source/actor_runtime`；candidate-source=`/mnt/c/Users/icema/Documents/ChatGPT/下一代ipc框架-dev/actor_runtime`；output=`.../benchmark-results/2026-09-14-mailbox/type-screen`；`--repeats 3 --minimum-ratio 1.0`。先验证路径/源码现态，若输出目录已存在则先检查其运行证据，不能盲目覆盖/重跑活跃任务。
- 完整目标仍 active、未验收：性能回退尚未解决；显式 forget/晚完成/父退出全面测试、timer 大量乱序撤销/容量/跨 worker 取消、mailbox 停止竞态/回绕/分配失败、真实业务流与内存平台期、覆盖率和更完整同机性能矩阵仍需完成。额度恢复后从 H3 性能验证继续，不重复前面的阻断叙述或缩小目标。

## 2026-09-15 当前续接：验证计数器布局，不放宽性能门槛

- 已重新读取 MISSION、NOTES、COURSE、LR-0004，并核对 09-11 四项 P0 及 09-13 细化方案。按 diagnosing-bugs 使用固定基线和单变量对照；教学协议保持一次一个原子问题，不推进尚未有独立作答的课程。
- H3 实际已经测完并撤回，不能继续沿用此前“尚未启动”的描述。H4 将 nextActorId 起始的 ID 组 alignas(64)，与 read-mostly config/accepting 隔离；普通 CTest 15/15，但 event 单/多 worker 比值 0.9224/0.8121，仍失败。证据在 dev 的 type-screen、config-align-screen 及 config-align-ctest.log。
- 上一阶段 H1/H2 的 perf cpu-clock 用户态采样显示候选 postMessage、trySendOwned、releaseMessage 是主要热点；releaseMessage 的样本集中在全局预算原子减附近，postMessage 的样本集中在配置/预算读取及 CAS 附近。采样有 skid，不能把指令附近的占比当作精确耗时，也不能据此宣称唯一根因。原始 perf 数据在 WSL 构建根目录 baseline-event.perf/candidate-event.perf；原计划 profile-h2 文本归档未落地，本轮已确认该目录不存在。
- 当前原子实验 H4b：额外对 outstandingJobs 使用 alignas(64)，将 ID 写入组和调度计数组分离；不改变容量、计费、准入或 timer 后端。当前 candidate-build 构建句柄 89921 已核实成功退出 0；源码暂包含 H1/H2/H4/H4b，不含 H3。H4b 尚不能引用旧 sanitizer 15/15 作验证：asan-build 对应当时 H3 版本。
- 本轮首先请求 H4b 完整 CTest；只有回归完成后才运行 event-single/event-parallel 配对筛查，避免编译/测试干扰计时。尚未完成最终覆盖率、完整内存与业务性能验收，目标保持 active。
- H4b 回归请求首次审批超时，在允许的一次重试后成功：**15/15**，日志 `hot-groups-ctest.log`。短测同样经历一次启动前审批超时，随后重试完成（进程 4762 已退出）：`hot-groups-screen`，3 轮、每场景 100 万 event round trips、默认预算不变、CPU 0–7。单 worker 吞吐比 **0.99448645**，多 worker **1.07315696**；严格门槛仍 FAIL event-single。多 worker 基线三轮约 2.178/3.196/3.389 M rps，波动明显，不能据此宣布稳定提速。
- 保持源码不变继续请求六场景确认：`hot-groups-confirm`，5 轮、event 每轮共 500 万、fiber 共 50 万 round trips。使用刚重编驱动，不重复编译或与其他测试并行；核验二进制散列须和 screen 的 metadata.json 一致。待确认结果出来再决定是否保留 H4/H4b，不用最好的短跑替代验收。
- **本次终点**：六场景确认的首次请求及工具允许的一次重试都在进程创建前因自动审批超时失败，没有产生可等待的 WSL 进程句柄；两个 functions 执行句柄均已终止。不能把审批等待当作基准仍在跑。当前 H4b 保留为待验证候选，不继续叠加新优化；下一步须在执行通道恢复后完成同一六场景确认，仍使用 minimum-ratio=1.0。当前已有功能和短测新证据，不能按本次两次请求失败直接满足三轮 blocked 条件，目标保持 active。
- 本轮已更新本文件及 LR-0004；无学习者新作答，不新建代表学习进步的记录。未修改教学练习、切换 timerfd、提交、推送或改写历史。源码版本的旧 sanitizer 结果不代表当前 H4b 已过 sanitizer，最终覆盖率和完整四项验收仍欠缺。

## 2026-09-15 提交检查点与长测结论

- 六场景长测 `hot-groups-confirm` 已完成（5 轮，event 每轮 500 万、fiber 每轮 50 万 round trips，CPU 0–7，`--minimum-ratio 1.0`，默认 mailbox 预算未放宽）。比值 candidate/baseline：event-single **0.9291**、event-window **0.9447**、event-parallel **0.9923**、event-hotspot **0.9981**、fiber-single **0.9723**、fiber-parallel **0.9425**。六项全部低于 1.0，H1/H2/H4/H4b 的缓存行隔离与计数改造没有消除回退；性能门槛仍未通过，不能宣布“性能不劣化”。
- 用户本轮解除此前“禁止提交”约束，要求提交本轮修改。dev 分支提交 `2c21aa6`，含四项 P0 实现、三个新测试文件、`compare_p0.py`/内存探针、全部性能与 CTest 证据，以及 09-11/09-13 三份评估文档；teaching 分支提交 `10318d9`，含第 4–8 章课文、可运行实验源码、实验工单、调度速查与 LR-0002/0003/0004。两笔均为本地提交，**未推送**；未改写历史。
- 下一轮从性能根因继续。已知候选与基线的差异集中在 `postMessage` 准入路径与 `releaseMessage` 的全局预算原子更新；perf 采样有 skid，只能作为方向。不得为通过门槛而关闭或放宽预算、缩小计费口径或切换 timerfd。仍未验收：显式 forget/晚完成/父退出全面测试、timer 大量乱序撤销与容量、mailbox 停止竞态与分配失败、回绕计数反例、覆盖率复测、内存平台期、完整同机性能矩阵。

## 2026-09-15 修正：测量方法先于结论，计费代价完成归因

**此前基于 3-repeats 中位数的比值不可作为验收依据。** 用同一对二进制自比（噪声底实验，`benchmark-results/2026-09-14-mailbox/noise-floor*`）测得：单轮配对比值在 0.79–1.18 之间，5 轮中位数仍有 ±2–4% 漂移，且偶数轮次存在轻微顺序效应。`compare_p0.py` 已改为同轮内配对求比值并对配对中位数设门槛，胜负计数一并报告。用 16 轮 × 200 万 round trips 复测时噪声底收敛到约 ±1%（配对中位数 1.010 / 1.015）。

冻结基线复测（`split-counter-gate`，16 轮 × 200 万，配对中位数）：event-single **0.9325**（0 胜 16 负）、event-window **0.9264**、event-parallel **0.9562**、event-hotspot **1.0171**、fiber-single **0.9557**、fiber-parallel **0.9403**。即候选确实比基线慢约 4–7%，且单 worker 全败，属于真实回退而非噪声。

**归因实验**（诊断源码在 `%TEMP%\actor-diag`，仅用于测量，不进入仓库）：

| 依次去掉的部分 | 单 worker | 多 worker |
|---|---:|---:|
| 系统级 `admittedMailboxBytes/returnedMailboxBytes` 共享计数器 | 0.998 | **1.316** |
| 再加每 Actor `returned*` 原子读 | 0.995 | 1.341 |
| 再加字节估算、`estimatedMailboxBytes` 写回、release 侧发布写 | **1.080** | 1.390 |
| 只恢复字节估算（release 写仍关闭） | 1.042 | 1.068（window 0.996） |

结论：多 worker 的回退几乎全部来自系统级共享计数器——12 条线程每消息在同一 cache line 上做 CAS，该行成为串行点；单 worker 与它无关，而是字节估算加 release 侧发布写（约 7%）。每 Actor 原子读的贡献接近 0。

**已否决的方案**：用 `fetch_add` 加回滚替代 CAS 循环。瞬时超计会让并发生产者提前拒绝，`concurrent_system_budget` 的 `accepted == 64 && full == 64` 精确断言在 8 次重复中失败 7 次。已撤回，源码回到精确 CAS 版本并复验 15/15。

**待用户决策的设计取舍**：每消息精确的系统级字节预算要求全部生产者写同一个计数器，这在多 worker 下实测约 30% 代价（去掉后 event-parallel 为候选的 1.39 倍）。若要同时守住「不得劣化」，只能把系统预算改为按 Actor 分块预留：保证永不超支（只会提前拒绝），但放弃「并发下恰好填满到限额」的可观察语义，需要改写 `concurrent_system_budget` 的断言；`stats().queuedMailboxBytes` 可继续按每 Actor 精确值求和保持精确。在补齐这一取舍前不宣布性能达标。

### 第三种尝试：两级预留（fetch_add 快路径 + 精确回退）——同样被否决

- 设计意图：稳态只做一次 `fetch_add`，一旦按自己的线性化点判断疑似超限就回滚并落到精确 CAS 判定，从而既消除重试风暴又不误拒。
- 实测失败：`mailbox_capacity_tests` 连续失败 18/20，失败断言是 `concurrent drain failed or exceeded budget`，不是计数少收。原因是 `fetch_add` 在回滚前的那段窗口里，`admitted` 会被并发读走——`stats().queuedMailboxBytes` 是公开可观察量，因此**从未被承认的消息也会短暂体现为占用**，`concurrent_drain` 的「不得超过预算」轮询会捕捉到它。
- 结论（三次实验一致）：在保持「每条消息都不出现瞬态超计」这一可观察精确语义的前提下，无法去掉每消息一次的原子读改写。要么接受精确计费带来的多 worker 串行化代价，要么把预算改成预留式（永不超支、可能提前拒绝）并相应改写断言。二者不可兼得，属于产品语义取舍而非实现技巧。
- 单 worker 的约 7% 独立于系统预算，来自两部分各自约 3.6%：字节估算加 `estimatedMailboxBytes` 写回、以及 release 侧两次发布写。前者是字节预算的必然成本；后者理论上可用打包计数或把发布并入既有 mailbox 锁临界区来削减，但需要另行验证。
- 当前状态：源码已恢复精确 CAS 版本，mailbox 专项重复 10/10、全量 CTest 15/15 通过；未提交、未推送，未切换 timerfd。

### 已实施的选项：`Config::mailboxBudget`（默认 Exact，可选 Reserve）

- 新增 `Config::mailboxBudget`（`Exact` 默认 / `Reserve`）与 `Config::reserveChunkBytes`（默认 64 KiB）。Exact 语义与之前完全一致；Reserve 让每个 Actor 从共享池整块预取，生产者只在换块时触碰共享计数器，并在邮箱连续 64 次准入都为空闲时归还整块，Actor 停止时回收整块。Reserve 只可能**提前拒绝**，永不超支。
- 实测（12 轮 × 200 万，配对中位数，对比冻结基线）：event-parallel **1.2440**（12 胜 0 负，精确模式为 0.9562）、event-single **0.9642**（精确模式 0.9325）、fiber-single **0.9781**（精确模式 0.9557）。证据在 dev 的 `benchmark-results/2026-09-14-mailbox/reserve-mode-gate`；驱动通过 `ACTOR_BENCH_BUDGET=reserve` 切换，基线构建忽略该变量。
- 结论：多 worker 的 30% 串行化代价确实来自精确模式，预留模式把它变成优势；单 worker 剩余约 3.6% 差距来自字节估算本身，属于字节预算的固有成本，与共享计数器无关。
- 边界：默认模式未改，回归 15/15 通过；**Reserve 模式目前只有实现与性能证据，尚无专门的不变量测试**（永不超支、提前拒绝有界、停止后归还），需要在正式启用为默认前补齐。未提交前请勿把 Reserve 当作已验收特性。

### 补齐与收口（Reserve 不变量、ASan、剩余 3% 的性质）

- Reserve 模式的不变量测试已补齐并提交（dev `2c3ed38`）：1/4 worker 下断言永不超预算、确有背压、提前拒绝不超过一个 chunk、已接受消息恰好投递一次、停止后归还整块且池子归零。全量 CTest 15/15，该用例重复 6/6。
- 当前版本（拆分计数器 + 预留模式 + 新不变量）已过 ASan/UBSan：`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1` 下 15/15 通过，用时 3.87 秒。此前的 sanitizer 结论只覆盖旧版本，本条更新它。
- 关于单 worker 剩余约 3%：`ActorRef::send` 的公开签名是 `std::any payload` 值传参，静态类型在调用点已被擦除，因此无法直接在模板层得到编译期体积；即使新增类型化重载加体积提示，也只覆盖“估算”的一部分（三次 `saturatedAdd`、信封常量、发送者地址长度、`estimatedMailboxBytes` 写回仍在）。据此判断：在保留逐消息字节口径的前提下，跨过 1.0 门槛需要另一个与计费无关的独立提速，而不是继续微调估算。
- 仍未验收：显式 forget/晚完成、timer 大量乱序撤销与容量、覆盖率复测、内存平台期、完整同机性能矩阵。timer 后端按用户要求未切换。

### 环境事实与下一步方向（2026-09-15 晚）

- WSL 内核 5.15.167.4-microsoft 下 **perf 不可用**（缺 linux-tools-*-microsoft-standard-WSL2，命令直接提示安装），因此无法用硬件计数分辨剩余 3% 属于算力还是缓存一致性；此前 `baseline-event.perf` 一类的原始文件在此环境不再可复现。**gcovr 也未安装**，覆盖率复测前需要先装（`python3 -m gcovr` 与 `which gcovr` 均失败）。
- 逐项检查可得的微优化后判断：缓存 `payloadSizer` 是否为空、以及发送者为空时跳过地址长度相加，都属于等价改写但省不下任何访存（`std::function` 判空与 `empty()` 同样是加载加比较），期望收益接近零，故未实施。真正剩下的是结构性改动：把准入路径上的每消息原子读缓存到 Actor 内的副本（陈旧方向保守、只会提前拒绝），或改掉 mailbox 互斥锁本身；前者可省约 2 次原子加载，后者在多 worker 下把普通锁换成自旋锁会与既有嵌套锁冲突，需专门设计。
- 也就是说，单 worker 剩余约 3% 的收口需要一次有测量的结构性改动，而不是继续微调；在此之前不宣布性能达标。
