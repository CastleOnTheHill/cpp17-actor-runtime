# Actor 模型与框架调研

> 调研日期：2026-09-04。资料仅采用论文、项目官方文档及官方仓库；本文中的“保证”均以各框架文档声明为准。

## 一句话结论

Actor 不是一种线协议，而是一套**并发状态所有权与故障隔离模型**：每个 Actor 独占自己的状态，经邮箱接收消息并逐条执行行为；Actor 引用隐藏其实际位置。它很适合成为下一代 IPC 的上层编程模型，但不应代替传输层、流控、序列化、持久化与端到端可靠性协议。

## 1. 核心语义

Actor 模型由 Hewitt、Bishop、Steiger 在 1973 年提出。Actor 收到消息后可以做三类动作：向已知 Actor 发送有限条消息、创建有限个 Actor、指定处理下一条消息的行为（[原始论文](https://www.ijcai.org/Proceedings/73/Papers/027B.pdf)，[Apache Pekko 的概念说明](https://pekko.apache.org/docs/pekko/current/general/actors.html)）。工程实现通常包含：

- **状态隔离**：外界只持有 `ActorRef`/地址，不能直接读取或修改 Actor 内部状态。Actor 之间不共享可变状态，只通过消息协作。
- **串行处理、整体并发**：单个 Actor 在任一时刻只执行一个消息处理过程，因此其局部状态通常不需要锁；大量 Actor 由调度器并发运行，常见实现是多个 Actor 复用线程池，并非“一 Actor 一线程”（[Pekko](https://pekko.apache.org/docs/pekko/current/general/actors.html)）。
- **消息驱动行为**：邮箱解耦发送者与接收者；发送通常是异步的。Actor 可在处理后切换行为，因而天然适合状态机。
- **封装位置**：同一个引用可指向本进程或远程节点上的 Actor。但“调用形式透明”不代表“分布式故障透明”：跨进程仍有序列化、延迟、丢包、重复、超时和网络分区（[Akka 对位置透明边界的说明](https://doc.akka.io/libraries/akka-core/current/general/remoting.html)）。

### 生命周期与故障模型

经典 Actor 往往显式创建/停止并形成父子树；父节点停止时递归停止子节点。监督者根据故障执行恢复、重启、停止或向上升级，失败处理与业务协议分离（[Akka 生命周期](https://doc.akka.io/libraries/akka-core/current/typed/actor-lifecycle.html)，[Akka 监督](https://doc.akka.io/libraries/akka-core/current/general/supervision.html)）。Erlang/OTP 将这一思路工程化为 worker/supervisor 监督树；进程退出通过 link/monitor 传播，监督者按策略重启子进程（[Erlang 进程、link 与 monitor](https://www.erlang.org/doc/system/ref_man_processes.html)，[OTP Supervisor](https://www.erlang.org/doc/system/sup_princ.html)）。

Virtual Actor 则把“逻辑身份”和“内存实例”分开：Actor/Grain 逻辑上由类型与 ID 标识，收到请求时按需激活，空闲后回收，故障后可在别处重新激活。Orleans 和 Dapr 属于这一类（[Orleans activation collection](https://learn.microsoft.com/en-us/dotnet/orleans/host/configuration-guide/activation-collection)，[Dapr Actor runtime](https://docs.dapr.io/developing-applications/building-blocks/actors/actors-features-concepts/)）。

### 顺序与可靠性：最容易误解的部分

- Actor 模型本身**不承诺 exactly-once**。Akka/Pekko 的普通消息是 `at-most-once`；只保证同一 sender→receiver 的直接发送按入邮箱顺序到达，多个发送者之间没有全序，转发也不保留传递因果顺序。优先级邮箱还可能改变实际处理顺序（[Akka](https://doc.akka.io/libraries/akka-core/current/general/message-delivery-reliability.html)，[Pekko](https://pekko.apache.org/docs/pekko/snapshot/general/message-delivery-reliability.html)）。
- Erlang 同一实体发往同一目标的 signal 保序，但 signal 仍可能因分布通道断开而丢失；其选择性 `receive` 还可能跳过队首不匹配消息（[Erlang signal 顺序](https://www.erlang.org/doc/system/ref_man_processes.html)，[邮箱与选择性接收](https://www.erlang.org/doc/system/conc_prog.html)）。
- Orleans 默认不重试时是 `at-most-once`；开启重试后可能重复，成为 `at-least-once`，运行时不替业务做持久去重（[Orleans 投递保证](https://learn.microsoft.com/en-us/dotnet/orleans/implementation/messaging-delivery-guarantees)）。
- 处理消息时崩溃，当前消息通常不会因 Actor 重启而自动重放。所谓“有效一次处理”需要应用参与：稳定消息 ID、处理后确认、持久化重试队列、幂等处理/去重，以及必要时 outbox/inbox（[Pekko Reliable Delivery](https://pekko.apache.org/docs/pekko/current/typed/reliable-delivery.html)）。

## 2. 代表框架

| 框架 | 生态与模型 | 关键特性 | 选型提示 |
|---|---|---|---|
| **Akka** | JVM，Java/Scala；Typed Actor、Cluster、Persistence | 显式父子生命周期；`at-most-once` + sender/receiver 对内有序；监督可 resume/restart/stop | 成熟但当前 Akka 以 BSL 1.1 发布，生产使用需评估商业许可（[官方许可](https://doc.akka.io/libraries/akka-core/current/project/licenses.html)） |
| **Apache Pekko** | JVM，源自 Akka 2.6 的 Apache 项目；Java/Scala | 与经典 Akka 生态和语义接近；Typed Actor、Cluster、Persistence、Streams | JVM 上需要 Apache 2.0 路线时优先评估；Typed Actor 未配置监督时异常默认停止（[故障容错](https://pekko.apache.org/docs/pekko/current/typed/fault-tolerance.html)） |
| **Erlang/OTP** | BEAM；Erlang，Elixir 可复用其运行时能力 | 极轻量进程、选择性接收、link/monitor、监督树、`gen_server`/`gen_statem` 等 behaviours | 长驻、高并发、强故障隔离系统的标杆；模型与 VM 深度一体化（[OTP 设计原则](https://www.erlang.org/doc/system/design_principles.html)） |
| **Microsoft Orleans** | .NET/C#；Virtual Actor（Grain） | 类型+键构成稳定逻辑身份；自动放置、激活、回收与故障后重激活；每个 activation 逻辑串行 | 很适合云端业务实体；跨 Grain 一致性、重试幂等和外部副作用仍由应用设计（[概览](https://learn.microsoft.com/en-us/dotnet/orleans/overview)，[故障处理](https://learn.microsoft.com/en-us/dotnet/orleans/deployment/handling-failures)） |
| **Dapr Actors** | 多语言 sidecar；Virtual Actor；HTTP/gRPC API | placement 服务负责分布；每个 Actor 使用 turn lock；状态存储、timer、持久 reminder | 适合 Kubernetes/多语言统一能力，但多一跳 sidecar 与序列化成本；热点 Actor 会串行瓶颈，循环调用可能死锁（[运行时特性](https://docs.dapr.io/developing-applications/building-blocks/actors/actors-features-concepts/)，[timer/reminder](https://docs.dapr.io/developing-applications/building-blocks/actors/actors-timers-reminders/)） |
| **CAF** | C++ Actor Framework；本机与分布式 Actor | 动态/静态类型消息、请求响应、监视/link、网络透明；消息用 copy-on-write 降低只读广播复制 | 适合高性能原生 C++ 组件；仍需自行定义线协议兼容、资源上限与跨版本策略（[CAF 消息传递](https://actor-framework.readthedocs.io/en/latest/core/MessagePassing.html)） |
| **Actix Actor** | Rust；typed message、`Addr`、Context 生命周期 | `Started → Running → Stopping → Stopped`；邮箱容量和不同 send API 的丢弃/背压语义不同（[Actor](https://actix.rs/docs/actix/actor/)，[Address](https://actix.rs/docs/actix/address/)） | 不建议作为新 Rust 项目的默认基座：Actix 维护者称 Actor 系统已 soft-deprecated，并建议 Tokio actor pattern 或异步原生框架（[维护者答复](https://github.com/actix/actix-extras/discussions/535)） |

## 3. 对“下一代 IPC 框架”的适配判断

### 值得吸收的设计

1. **Actor ID/引用作为逻辑端点**：把业务身份与 PID、线程、socket、节点地址分离；迁移和重启不迫使调用方更新引用。
2. **每端点单写者**：以 mailbox + run-to-completion 保护局部状态，避免把共享锁暴露给业务代码；同时允许不同端点并行。
3. **类型化协议**：消息 schema、请求/响应关联 ID、协议版本和能力协商应是一等公民；Actor 引用最好携带可发送消息的类型/接口约束。
4. **可靠性显式分层**：默认快速 `at-most-once`；另提供带 ACK、重试、去重与持久化的可靠通道。ACK 必须区分“进入传输层/邮箱”和“业务处理完成”。
5. **背压先于重试**：有界邮箱、credit/demand 流控、按端点配额、过载拒绝与 dead-letter/诊断必须内建。无限邮箱只是把过载变成 OOM。
6. **故障是控制面事件**：监督、watch/monitor、取消、超时和进程退出需要明确语义；控制消息与普通消息若走不同队列，必须说明二者没有相对顺序保证。
7. **两类生命周期并存**：底层资源 Actor 适合显式父子生命周期；海量业务实体适合按 ID 激活/回收的 Virtual Actor。不要强迫所有端点采用同一种模型。
8. **可观测性进入协议**：队列深度、排队/处理时延、丢弃与重试次数、Actor 重启原因、trace context 应跨进程传递。

### 不应由 Actor 抽象掩盖的事实

- 本机与跨机调用成本不同；“位置透明”应是 API 透明，而非性能和失败语义透明。
- 单 Actor 是串行瓶颈。热点键需要分片、代理/路由 Actor、批处理或把读路径拆出；公平调度要防止某个邮箱霸占执行器。
- 大块二进制、连续媒体和高吞吐数据流更适合共享内存/ring buffer/stream 通道；Actor 消息宜承载控制信息、描述符或所有权令牌，而非反复复制 payload。
- 跨 Actor 事务、全局查询和广播不是 Actor 的强项，应交给事务存储、日志/流系统、索引或专门协调器。
- 阻塞 I/O 不能占住 Actor 调度线程；应异步化或隔离到专用执行器。

## 4. 建议的总体架构

建议把 Actor 作为 IPC 之上的**可选执行与状态模型**，而不是唯一抽象：

```text
业务协议 / Typed Actor API
        ↓
邮箱、调度、监督、生命周期、请求关联、背压
        ↓
投递策略：at-most-once | reliable(at-least-once + dedupe)
        ↓
统一帧格式、版本协商、认证、观测
        ↓
传输：进程内队列 | 共享内存 | Unix socket/Named Pipe | TCP/QUIC
```

最合适的场景是：设备连接、会话、窗口/插件实例、订单、游戏实体等“大量按 ID 分片、每份状态较小、单实体内需串行一致”的对象。若项目首要目标是大吞吐数据搬运，则应先设计零拷贝/批量/流式传输，再用 Actor 管理通道生命周期和控制逻辑。

## 5. 下一步验证建议

先做一个最小原型，不急于绑定某个完整框架：实现类型化 `ActorRef`、有界 mailbox、单端点串行调度、watch/stop、超时/取消，以及本机队列与跨进程传输两种 backend。用同一组测试验证 sender→receiver 顺序、进程崩溃中的消息状态、重试去重、热点 Actor 公平性和背压；这能更快判断 Actor 是应成为 IPC 的核心 API，还是仅作为其上层库。
