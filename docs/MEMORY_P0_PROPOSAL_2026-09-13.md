# 四项 P0 内存优化建议（待确认，未实施）

依据：[本机内存实测](MEMORY_ASSESSMENT_2026-09-11.md)、当前 dev 代码及 [成熟框架官方参考](MEMORY_FRAMEWORK_REFERENCES_2026-09-13.md)。建议不代表第三方框架与本库具有相同协程语义，也不是性能排名。

## 1. 协程结果：按所有权回收，不按当前 waiter 数回收

推荐第一版：root 成功结果不缓存，异常仍走错误回调；child 完成结果只保留到父协程放弃读取权或结束。父结束时清理已经完成的 child 结果；尚未结束的 child 按 ChildLifetime 取消或脱离，晚完成时不要重新存下无人可取的结果。

不能只判断“当前是否有人 await”：父协程并行 spawn A、B，先 await 慢 A；B 提前完成时没人等，但随后父协程仍需要 await B。`wait_semantics_tests.cpp:54-56` 还明确要求重复读取 child 的成功、错误、取消结果。

已有等待者时，可以先把结果交给 WaitRegistration/pendingWait 持有，再排入 ready queue，随后释放生产端不需要的引用；不必等消费者真正执行完。单次消费尽量 move，多处读取可共享不可变 outcome，但不能让被唤醒者读取已经删除的 completion。

为长寿命父协程增加显式 `forgetChild(id)`，第一版可仅允许已完成 child；释放 payload 时也回收相关 children/ownership 元数据。forget 后再 await 明确报错，不能挂起。保留既有晚 await、重复 await；后续另行讨论 move-only 结果句柄和单次消费 Interface。

“最近 10 个结果”不宜作为正确性缓存：第 11 个完成可能挤掉第 1 个尚未读取的结果；10 个也不能限制总字节。可做独立诊断环形记录，只保留 ID、状态、耗时、截断错误摘要，不留完整 payload/exception_ptr。若提供可丢失结果缓存，必须显式选择并定义字节限额、过期错误和有效所有者的保护规则。

CAF 的 request 使用一次性响应处理器，适合借鉴“把结果交给请求方”，但不能等同本库可重复 await 的 future/join；官方参考不支持统一固定 10 条缓存的说法。

验收：B 比 A 早完成仍可 join；重复 await 兼容；root 历史完成数不累积结果；父先退出、child 取消/脱离和异常回调正确；主动释放后 payload 与元数据均进入平台期。

## 2. blocking 结果：便捷调用自动释放，显式任务可主动放弃

`blocking()` 内部 TaskId 不暴露给调用方，返回成功/错误/超时或取消退出时应自动释放其结果兴趣，作用域清理覆盖异常路径。这是最适合先做的兼容优化。

`submitBlocking()` 保留显式 TaskId 的晚 join、重复 await，新增 `forgetTask(id)` 放弃结果。现有 `blocking_tests.cpp:41-48` 明确测试超时后再 join 并重复读取，因此 `Any(task, timer)` 超时不能自动忘记显式 task。

超时/forget 不等于终止工作。阻塞函数可能仍运行，必须保留它使用的自有数据，不能强杀线程；晚到结果不再复活 tasks 表项。现有 TaskCompleted 在找不到 task 时丢弃晚结果的路径可作为基础，仍需重测停止/完成竞态。

Akka 官方明确 ask 超时不通知接收方停止执行，借鉴的是“等待超时和执行取消分离”。排队工作、执行中工作、完成结果分别计量；只限制 blocking 队列 64 项不足以控制历史结果或大对象捕获。

验收：常驻 fiber 连续 10 万次 blocking()，内部结果不随历史次数增长；显式 task 在 forget 前可重复读取；forget 后晚完成不重新缓存，工作中数据仍有效。

## 3. waiter/timer：统一注销，保留世代保护

将登记和清理集中到内部 WaitRegistration Module，外部仍使用小的 WaitSpec Interface。完成、取消、fiber 退出、Actor 停止均走幂等 cleanup。

Any 胜出时撤销输家登记，All/AllSettled 完成或中断时清理剩余登记；只是撤销本次等待，不默认取消底层 child/task，允许以后重新 await。先完整校验 WaitSpec，再登记；失败回滚，不能因一个已过期 timer 而跳过非法 child/task 的原有校验。

用稳定登记 ID/反向索引定位消息、task、child 等待项；清理注意广播迭代器安全。timer 首选 owner worker 内支持按 ID 撤销的索引堆；若先采用墓碑压缩，须有明确触发阈值和独立的容量收缩规则，不能仍等到远期 deadline 才清理。

保留 token/generation 校验，防止已经排入 ready queue 的旧事件恢复新一轮等待。CAF delayed send 的 disposable 体现可撤销生命周期，但它不是请求响应取消 API，文档也不保证取消后底层 timer 节点立即物理释放。

验收：唯一 correlationId、高频快速响应击败长 timer、嵌套 Any/All、取消/停止竞态、非法登记回滚；有效/失效条目数和后备容量分别检测，不随历史请求量线性增长。

## 4. mailbox：非阻塞准入，条数与字节双限额

新增 `trySend`，区分 Accepted、Full、Stopped、MessageTooLarge；Accepted 只代表准入，不代表业务成功。保留旧 send 时满队列也应明确报错，不静默改成丢弃。

同一个准入临界区完成容量校验、额度占用、停止检查和入队；出队/拒绝/异常路径正确归还。除每 Actor 限额外还有系统总预算与单消息尺寸限制；std::any 无法通用精确计算深层字节，须定义已知 payload 表示或估算口径，共享 buffer 也要规定计费方式。

不要让 Actor worker 同步等空位。默认业务请求建议 Reject，发送方用在途额度、credit/ack 或有限退避减速；重试队列必须有界。仅允许明确可丢的数据采用 drop/coalesce。取消/恢复/停止等控制路径不能被业务队列挤死，其资源由活跃任务预算等单独约束，也不能无限绕过总预算。

SObjectizer 提供按消息类型的队列限额和溢出策略，但不包括执行中任务；Akka/Pekko 的非阻塞有界 mailbox 将溢出消息送到 dead letters，不等于反馈式背压或可靠投递；CAF flow 的需求驱动可借鉴到持续生产者的 credit 协议，不能认为普通 mailbox 自动获得同样能力。

验收：并发生产者条数/字节不超额；下游停顿拒绝可观测、恢复后继续处理；已准入消息不因溢出静默丢失；关闭竞态、额度回滚、多 Actor 总预算与有限重试均覆盖。

## 实施顺序与边界

先做 root 不缓存、父退出清理、blocking() 自动释放；随后统一等待清理及 timer 撤销；再确认显式 forget、结果准入预算和 trySend 的新增 Interface。最后复跑内存场景、原有晚/重复 await 测试、ASan、取消并发和吞吐回归。

长寿命父协程若主动保留任意多结果，仍不可能固定内存。兼容版需要显式释放和资源准入预算，不能承诺“无限读取历史结果 + 固定内存”。本方案使用 codebase-design 的小 Interface/集中生命周期实现原则，结合 research 官方资料核查；未修改运行库、提交或推送。
