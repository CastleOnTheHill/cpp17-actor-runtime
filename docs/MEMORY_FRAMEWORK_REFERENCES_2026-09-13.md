# Actor 框架内存有界性参考

核验日期：2026-09-13。仅使用下列 6 个官方文档；CAF 页面标注 1.1.0，Akka 页面标注 2.10.22，Pekko 页面标注 1.7.0，SObjectizer 使用 5.8 系列专题。`stable/current` 链接会随发布变化。本文不声称这些框架默认实现了全进程硬内存上限。

## 已核验事实

| 机制 | 官方行为 | 适用边界 |
|---|---|---|
| CAF request / response handler | `request` 返回中间句柄，用于安装一次性响应处理器。`then` 与常规 actor behavior 复用；`await` 暂停常规 behavior，并按 LIFO 处理等待响应；blocking actor 的 `receive` 才阻塞调用线程。请求可配置超时，超时产生 `sec::request_timeout`，`infinite` 关闭超时。[CAF Message Passing](https://actor-framework.readthedocs.io/en/stable/core/MessagePassing.html) | 这是消息关联与一次性回调语义，不能等同于支持晚到、重复 `co_await` 的 future/task 结果对象。超时参数本身未规定同时存在的请求总数上限。 |
| CAF delayed send / disposable | 延迟或定时 `send` / `delegate` 返回 `disposable`，允许取消尚未发送的操作。待发送消息默认持有发送方、接收方强引用，可选弱引用；弱引用无法升级时丢弃消息。[CAF Message Passing](https://actor-framework.readthedocs.io/en/stable/core/MessagePassing.html) | “可取消”是生命周期控制能力，不构成固定数量的 timer 配额；该文档也不保证调用取消后底层定时器节点立即物理回收。不要把这里的 disposable 直接当作请求响应取消 API。 |
| CAF flow | `subscription.request` 表示消费需求，`dispose` 停止接收。默认背压会减慢乃至停止生产者；`on_backpressure_buffer` 设置固定条目容量，溢出策略可选 `drop_oldest`、`drop_newest`、`fail`，默认 `fail`。[CAF Data Flows](https://actor-framework.readthedocs.io/en/stable/core/DataFlows.html) | 这是 flow 通道机制，不能推导任意 actor mailbox 自动具有背压。条目数也不是字节数；`distinct` 保存历史唯一值、`to_vector` 收集全部输入，因此仍须审查算子状态和输入范围。 |
| Akka bounded mailbox | 默认 mailbox 无界；有界 mailbox 满时新消息进入 dead letters。`NonBlockingBoundedMailbox` 明确不阻塞、丢弃溢出消息到 dead letters。[Akka Mailboxes](https://doc.akka.io/libraries/akka-core/current/typed/mailboxes.html) | 队列溢出不等于请求调用方立即收到业务拒绝响应；也不等于生产者自动获得需求额度。容量针对消息排队，不能据此证明任务结果缓存或业务状态有界。 |
| Akka ask | 一个 ask 只有一次响应；超时后接收 actor 不知道超时，仍可能完成甚至才开始处理。actor 外 ask 返回 Future / CompletionStage；actor 内示例把响应或失败适配为 actor 自己的消息。[Akka Interaction Patterns](https://doc.akka.io/libraries/akka-core/current/typed/interaction-patterns.html) | 超时是调用方等待失败，不能等同于撤销对端工作。actor reply 与外部 future 是不同层次；此文档没有支持“当前挂起等待者为零即可删除 future 结果”的结论。 |
| Pekko bounded mailbox | 默认无界；非阻塞有界 mailbox 将溢出消息送 dead letters。其他 bounded mailbox 在非零 `mailbox-push-timeout-time` 下可阻塞发送者，Typed 文档要求列出的这些实现只使用零 push timeout。[Pekko Mailboxes](https://pekko.apache.org/docs/pekko/current/typed/mailboxes.html) | 必须区分具体 mailbox 类型和配置，不宜笼统称“有界 mailbox 一律不阻塞”。本次 ask 超时结论直接由 Akka 文档核验，未仅凭两者谱系推广到 Pekko 所有版本。 |
| SObjectizer message limits | 限制按 agent 的消息类型分别设置；默认限制仍应用到各类型，不是所有类型共享一个总容量。溢出可丢弃、abort、重定向、变换后发送。入队前增加计数，出队并调度处理时、调用 handler 前减少计数。[SObjectizer Message Limits](https://github.com/Stiffstream/sobjectizer/wiki/SO-5.8-InDepth-Message-Limits) | 官方明确这不是完整过载控制。计数不覆盖已经出队的执行状态；重定向只转移压力。重定向/变换回调在发送者上下文执行，必须可并发调用。 |

## 对本框架的设计启示（推论，不是外部框架承诺）

1. 应分别界定排队消息、执行中任务、未完成请求、定时器、完成结果以及用户保留句柄的所有权和预算。单独限制 mailbox 不足以界定整个 runtime 的内存。
2. 在入口建立可解释的容量与溢出行为，或用需求/额度协议限制在途工作，比事后删除任意历史结果更容易证明语义。容量不足应对应明确的拒绝、背压或文档化丢弃策略。
3. 如果本地 task 契约允许晚到和重复 await，`waiter_count == 0` 只表示当前没有挂起等待者，不代表没有 future/handle 持有者或未来消费兴趣。结果回收需要所有权/兴趣生命周期证据，不能从 CAF 的一次性 handler 或 actor reply 推导。
4. deadline 限制等待时长，admission limit 限制并发数量，两者不能互代；取消后的逻辑失效与物理内存回收也应分别验证。
5. 本次官方资料不支持“成熟框架统一用固定 10 条结果缓存”这一说法，也未提供可移植到本框架的统一结果缓存大小。

## 验证范围

只核验公开文档语义，未克隆或逐行审计三方框架内部实现，未运行三方内存压测。因此没有对 timer 物理回收延迟、分配器保留量或全局内存硬上限作保证；本框架的实现选择仍须由其公开契约和本地测试决定。
