# Actor benchmark 对照框架调研

日期：2026-09-07。范围：官方资料核查与测试设计，不含吞吐实测；所有速度结论必须来自同一台 WSL 主机的后续运行。

## 1. 固定版本与构建

建议首轮固定 **CAF 0.19.6**、**SObjectizer v.5.8.3**。这是可复现的 C++17 对照版本选择，不宣称它们是最新版。官方发布列表已存在 CAF 1.x、SObjectizer 更高 5.8.x 版本；若报告用于评价“当前最新框架”，应另跑新版，不能将旧版本结果外推。固定版本的发布记录分别对应短提交 `be844d3`、`da09e89`。[CAF releases](https://github.com/actor-framework/actor-framework/releases)，[SObjectizer releases](https://github.com/Stiffstream/sobjectizer/releases)

获取入口：

- CAF 仓库：https://github.com/actor-framework/actor-framework ，tag `0.19.6`；tag archive：https://github.com/actor-framework/actor-framework/archive/refs/tags/0.19.6.tar.gz 。
- SObjectizer 仓库：https://github.com/Stiffstream/sobjectizer ，tag `v.5.8.3`；tag archive：https://github.com/Stiffstream/sobjectizer/archive/refs/tags/v.5.8.3.tar.gz 。

这些是对应已核实 tag 的获取 URL；本次调研未下载归档，不宣称已核实归档哈希。实际获取后需保存完整 Git SHA，或归档 SHA-256，并记录编译器、CMake 配置和生成的二进制哈希。

依赖建议为 CMake、C++17 编译器、pthread、Git（或下载/解压工具）。CAF 官方提供 CMake 构建，根目录 `configure` 只是包装器，不是必须。只测试进程内 Actor 时构建 core 即可，无须将可选网络/TLS模块纳入运行路径。[CAF 0.19.6 Overview](https://actor-framework.readthedocs.io/en/0.19.6/Overview.html)

SObjectizer 5.8 明确要求 C++17；其 CMake source directory 是仓库内的 **`dev`**，不是仓库根目录。安装后目标名为 `sobjectizer::StaticLib` 或 `sobjectizer::SharedLib`。[SObjectizer 官方 README](https://github.com/Stiffstream/sobjectizer)

```sh
cmake -S sobjectizer/dev -B so-build -DCMAKE_BUILD_TYPE=Release \
  -DSOBJECTIZER_BUILD_STATIC=ON -DSOBJECTIZER_BUILD_SHARED=OFF \
  -DBUILD_ALL=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF
cmake --build so-build -j4
```

这些开关来自官方构建说明。该说明的 master 页面含历史内容，因此执行时仍应核对固定 tag 的 `CMakeLists.txt` 和 configure 输出，不能忽略“unused variable”警告。[CmakeQuickHowto](https://github.com/Stiffstream/sobjectizer/blob/master/dev/cmake/CmakeQuickHowto.txt)

CAF 固定版本 CMake 原文通过 web 访问出现 cache miss，另一次直接获取返回 TLS EOF；因此这里不将其可选模块开关写成已验证命令。建议下载后从固定 tag 根 CMake 核实 `CAF_ENABLE_*` 名字，不盲用其他版本命令。

## 2. 两类 echo，避免语义错配

### 2.1 普通消息往返（核心吞吐对照）

三套框架都以两个长期存活的 Actor 做 ping/pong。每收到一条请求，仅回复相同 payload 和 sequence；客户端收到应答后再补发下一条。窗口 `1/16/64/256` 是**在途请求数**，不是一次函数调用中的消息批量。统计完成的 request-response pairs/s，同时注明相当于约两倍的单向 message deliveries/s；不得混用两种单位。

CAF 可用 `event_based_actor`，server `behavior` handler 收到消息后通过 `self->send(reply_to, ...)` 回复。client 用普通 handler 接收 pong 并补发。传递 responder handle 或预先连线，避免每条消息经字符串注册表查询；本框架、SObjectizer也应用同样规则。[CAF Actors](https://actor-framework.readthedocs.io/en/0.19.6/Actors.html)

SObjectizer 用 `so_5::agent_t`、`so_subscribe_self().event(...)` 注册 handler，通过 `so_direct_mbox()` 获得 MPSC 点对点地址，`so_5::send<Response>(reply_to, ...)` 应答。不要为这项测试使用 publish-subscribe 的 MPMC mbox。消息可定义为普通 C++ struct，内部携带 sequence 和 payload；控制层的 mbox handle 是进程内对象，不能误当成跨进程 C POD。[SObjectizer 基础教程](https://github.com/Stiffstream/sobjectizer/wiki/SO-5.8-Tutorials-Basics)

### 2.2 关联请求/等待（业务语义对照）

另列本框架 `send + await(WaitSpec::message(...))`、CAF `request(...).then(...)` 的关联等待成本。CAF handler 返回值可以自动成为 reply；`then` 允许其他正常消息继续处理，`await` 暂停正常行为、以 LIFO 顺序等待回调，但**不是有栈协程挂起**。`scoped_actor` 的 `request(...).receive(...)` 会阻塞原生调用线程，不应冒充同一类轻量等待。[CAF Message Passing](https://actor-framework.readthedocs.io/en/0.19.6/MessagePassing.html)

本框架当前 `CoroutineContext::await` 实际执行 `swapcontext`；其局部变量和调用栈可以跨等待保留。CAF event-based handler 则返回后由后续回调继续；SObjectizer普通 handler也由 dispatcher直接调用。额外保存栈和等待树所承担的能力不同，不能仅凭普通 echo 差距断言实现质量。这里的本框架判断已通过开发树 `actor_runtime/src/coroutine_context.cpp` 读取核对；性能代价大小仍待采样。

## 3. 调度与批处理设置

CAF 默认 work stealing，各 worker拥有自己的队列；空闲worker尝试窃取任务，并经历不同轮询阶段。默认模式并非全局 mutex 队列；官方也提供 work sharing策略，可作为更接近本框架全局调度的敏感性对照。报告需要记录选择的策略和worker数，而不是只写“CAF默认”。[CAF Scheduler](https://actor-framework.readthedocs.io/en/0.19.6/Scheduler.html)

CAF 配置键为 `caf.scheduler.max-threads`、`caf.scheduler.max-throughput`、`caf.scheduler.policy`。0.19.6 文档列出的默认每轮消息上限是 `9223372036854775807`，默认策略为 `stealing`。建议同时报告默认和固定batch1/64版本；无需人为把默认高效路径禁用后宣称胜出。[CAF Configuration](https://actor-framework.readthedocs.io/en/0.19.6/ConfiguringActorApplications.html)

SObjectizer `thread_pool` 默认是 cooperation FIFO与 `max_demands_at_once=4`；同一cooperation的多个agent可能共用顺序队列，不能并行。跨Actor扩展测试必须显式使用 `bind_params_t{}.fifo(fifo_t::individual)`，并记录 `max_demands_at_once`。其队列由dispatcher管理，而非简单地“一Actor一邮箱队列”。建议固定batch1/64做敏感性；不要用允许thread-safe handler并行的 `adv_thread_pool` 冒充Actor状态串行语义。[SObjectizer Dispatchers](https://github.com/Stiffstream/sobjectizer/wiki/SO-5.8-InDepth-Dispatchers)

## 4. 跨进程不可直接混排

SObjectizer 本体不提供开箱即用的分布式通讯，因此其进程内 echo不能拿来和本框架 Unix DGRAM echo算框架快慢比。[SObjectizer 官方 README](https://github.com/Stiffstream/sobjectizer)

推荐两张独立表：

1. **同进程 Actor**：本框架 / CAF / SObjectizer，同workers、同窗口、同payload、同Actor拓扑。
2. **跨进程 IPC**：本框架命名Actor Unix DGRAM / 裸 Unix DGRAM echo。裸socket只作为同传输、同主机下的开销基线，不称其为Actor框架。

如增加CAF网络测试，必须另注明其传输、序列化、地址解析与连接语义，不能将它的网络路径和本框架的本地 C POD memcpy 看作完全等价。

## 5. 建议 benchmark 矩阵（实验设计，不是官方性能结论）

| 场景 | 主要回答的问题 |
|---|---|
| echo窗口1 | 空载/闭环RTT和上下文切换开销 |
| 单热点server，窗口16/64/256 | 邮箱串行瓶颈与队列竞争 |
| 1/4/16个独立client-server对 | 调度扩展性，避免单Actor上限误判线程池 |
| workers1/2/4/8 | 加worker是否增益，或锁竞争反而退化 |
| payload8/64/1024/16384 B | 分配/拷贝/IPC syscall成本 |
| 有栈await versus普通handler | 高级等待语义的增量开销 |
| actor注册/停止/同名重建 | 生命周期吞吐与尾延迟 |
| timer/Any等待超时风暴 | 等待索引清理、timer堆与公平性 |
| blocking任务正常/超时/满队列 | 隔离是否有效，后台负载下的echo尾延迟 |
| C client本进程/跨进程同步请求 | 外部线程桥接成本与并发限制造成的差异 |
| steady-load长时间运行 | RSS、栈、完成记录是否随请求数增长 |

每个case先warmup、再至少5次测量，固定请求数或稳态计时协议；记录median/min/max，RTT p50/p95/p99与请求数，避免把仅闭环测试的p99当成开环服务SLA。持续全速吞吐与逐请求时间戳采样应分别跑，以辨识计时器开销。统一Release编译优化、禁用ASan/覆盖率/逐消息日志；性能分析另用带debug symbols且保留优化的二进制。

记录WSL发行版、内核、CPU型号、有效CPU亲和集合、worker数量、运行总线程数、虚拟化/超线程情况、源码SHA、flags和完整命令。不要在编译其他框架期间测量吞吐；调度默认策略造成的空闲CPU自旋也需要记录CPU时间，不能只看QPS。

## 6. 火焰图证据与结论边界

建议保存：原始采样文件、文本stack dump、folded stacks、SVG、构建信息、精确执行命令、stderr和样本总数。当前任务没有采样结果，以下只是后续验证假设：

- 若 `mmap/mprotect/munmap`、Fiber构造/销毁占比较高：验证按handler分配栈的固定成本，再考虑栈池/复用或无等待handler快路径。
- 若 queue mutex、futex、condition_variable占比较高：增加独立Actor对、减少worker或改batch进行因果对照；考虑按Actor ready queue、局部队列和批处理。
- 若分配器、`std::any`、等待树节点占比较高：分别比较普通echo与关联await；考虑小对象/节点池，但先验证生命周期边界。
- 若CPU火焰图看不到长尾：睡眠/锁阻塞可能属于off-CPU时间，不能把on-CPU百分比等同于端到端延迟占比。
- 使用 `ucontext` 的独立栈可能让采样栈在trampoline处截断；这不是没有调度成本。报告应注明采样方法及栈展开局限，不从SVG宽度直接推导优化收益。

任何优化意见都需要以本机原始数据支持；本调研没有引用网上吞吐成绩，也没有预先决定三套框架谁更快。

## 7. 适配器API示意（需编译验证，不是完整benchmark）

以下是根据官方API编写的示意，刻意使用CAF内建 `actor`、`uint64_t`、`string` 类型以避开自定义type-id注册；正式测试应统一payload的拷贝/构造工作量。

```cpp
caf::behavior echo(caf::event_based_actor* self) {
  return {[self](caf::actor reply_to, uint64_t seq,
                 const std::string& payload) {
    self->send(reply_to, seq, payload);
  }};
}
// client接收(uint64_t, const std::string&)后，校验并补发：
// self->send(server, caf::actor_cast<caf::actor>(self), next_seq, payload);
// cfg.set("caf.scheduler.max-threads", workers);
// cfg.set("caf.scheduler.max-throughput", batch);
```

SObjectizer线程池个体FIFO绑定示意：

```cpp
using namespace so_5::disp::thread_pool;
auto dispatcher = make_dispatcher(env, workers);
env.introduce_coop(
    dispatcher.binder(bind_params_t{}
        .fifo(fifo_t::individual).max_demands_at_once(batch)),
    [&](so_5::coop_t& coop) {
      // make_agent<Echo>() / make_agent<Client>()，连线so_direct_mbox()
    });
// Actor里：so_subscribe_self().event([this](const Reply& r) { ... });
// 发送：so_5::send<Request>(peer_mbox, ...);
```

## 8. 真实CPU采样获取路径

Linux内核文档说明 `perf_event_paranoid >= 2` 的常规限制是仅进程级、用户态监控，因此不应为了本任务先降低系统全局保护。先针对当前benchmark进程短测支持情况；WSL最终是否可用以实际成功采样为准。[内核perf安全文档](https://www.kernel.org/doc/html/latest/admin-guide/perf-security.html)

建议采样命令（`bench`及参数替换为实际目标）：

```sh
perf record -e cpu-clock:u -F 99 --call-graph dwarf,16384 \
  -o case.perf.data -- ./bench [arguments]
perf script -i case.perf.data > case.perf.txt
stackcollapse-perf.pl case.perf.txt > case.folded
flamegraph.pl --title 'CPU samples: case' case.folded > case.svg
```

`--call-graph dwarf,N` 保存用户栈转储并通过DWARF展开，perf构建需有对应unwinder支持；frame-pointer展开则要求适当的二进制编译设置。`cpu-clock:u` 是用户态采样，不能展示内核执行的详细调用栈。[perf-record手册](https://man7.org/linux/man-pages/man1/perf-record.1.html)

folded与SVG转换采用FlameGraph作者维护的脚本，必须保留原始 `perf.data` 和 `perf script` 输出以便重新转换，而不只有图片。[FlameGraph源码及工作流](https://github.com/brendangregg/FlameGraph)

若perf不能用，可在独立profiling build中链接 `-lprofiler` 或 `LD_PRELOAD`加载libprofiler，通过 `CPUPROFILE=case.prof`启用gperftools CPU采样；也可用 `ProfilerStart/ProfilerStop`只包围稳态阶段。默认频率100Hz，默认ITIMER_PROF；不要设置 `CPUPROFILE_REALTIME`后还称其为同一种CPU时间测量。[gperftools CPU profiler](https://gperftools.github.io/gperftools/cpuprofile.html)

Google pprof支持 `-text`、`-traces`及 `-http`中的Flame graph视图；`-svg`是调用图SVG，**不是火焰图**。若用 `-traces`转换folded，必须验证解析器兼容当前pprof格式及权重单位，检查转换前后样本总量一致，不能把图中的flat/cum百分比拼成假stack。[pprof官方文档](https://github.com/google/pprof/blob/main/doc/README.md)

这些工具说明不构成本项目已经完成采样的证据；实际应保存工具版本、原始数据、二进制与共享库符号、stdout/stderr、退出码、样本总数、转换器版本或SHA，以及未知/截断栈比例。
