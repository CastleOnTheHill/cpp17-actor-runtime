# C++17 single-process actor runtime

A small Linux/glibc actor runtime with multiple worker threads and cooperative,
stackful `ucontext` fibers. It is deliberately a deep module: callers use the
non-template `ActorSystem`, `ActorRef`, `Actor`, `CoroutineContext`, and runtime
`WaitSpec` tree while mailbox, scheduling, wait registration, timers, context
switching, and stack lifetime remain internal.

`ActorSystem` is a **process-wide singleton**: `main` starts it, every
submodule obtains the same instance and registers its own actors. Fiber
switching is annotated for AddressSanitizer, so ASan understands alternate
stacks instead of guessing.

This release is **single-process only and does not support IPC**. It has no wire
protocol, serialization, remote addresses, persistence, delivery retry, or
distributed supervision.

## Build and run on WSL/Linux

```bash
cd actor_runtime
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/request_response_example
./build/parallel_workflow_example
./build/order_fulfillment_example
./build/module_registration_example

# 带 AddressSanitizer 的协程切换验证
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DACTOR_RUNTIME_ASAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Requirements: Linux/glibc, a C++17 compiler, CMake 3.16 or newer, and pthreads.
There are no third-party runtime or test dependencies.

## 进程级 ActorSystem 单例

```cpp
int main() {
    actor_runtime::ActorSystem& system = actor_runtime::ActorSystem::start({4});
    const auto inventory = inventory::install();  // 子模块自己注册
    const auto order = orders::install(inventory);
    order.send(PlaceOrder, std::string("SKU-42"));
    actor_runtime::ActorSystem::stop();
}
```

- `ActorSystem::start()` / `start(Config)` 由 `main` 调用，返回唯一的实例。
  进程内已有运行中的实例时抛 `std::logic_error`；配置非法时抛
  `std::invalid_argument` 并保持“未运行”状态，不会留下半启动的系统。
- `ActorSystem::instance()` 是子模块取用全局实例的入口，未运行时抛
  `std::logic_error`。`isRunning()` 提供无异常的探测。
- `Actor` 的默认构造 `Actor()` 直接绑定全局实例，子模块不需要传递
  `ActorSystem&`；`explicit Actor(ActorSystem&)` 仍然保留给需要显式绑定的场合。
- `ActorSystem::stop()` 幂等，负责取消全部协程、排空任务、join 所有 worker
  并释放协程栈。它不得从运行库的 worker 线程（`onMessage` 或协程体内）调用，
  否则抛 `std::logic_error`；进程退出时还有一次 `atexit` 兜底。
- 单例外壳对象只分配一次且永不析构，`stop()` 只释放内部实现。因此停止之后
  旧的 `ActorRef` 不会变成悬垂指针：它只会安全地抛 `std::runtime_error`。
- 每次 `start` 递增一个世代号。跨世代的对象（例如重启前构造、重启后才注册的
  `Actor`）会被 `registerActor` 以 `std::invalid_argument` 拒绝。
- `examples/module_registration.cpp` 与 `examples/modules/*` 是跨编译单元的最小
  示例：子模块只写 `ActorSystem::instance()`，不接收 `ActorSystem&` 参数。

## Execution model and invariants

- An Actor has at most one executor at any instant. Each mailbox dispatch and
  fiber resume acquires the same Actor execution token.
- A process runs at most one ActorSystem. Worker threads, the actor registry,
  and the id sequences all belong to that single instance.
- Mailbox turns are global jobs, so different turns of one Actor may execute on
  different workers. A created fiber is permanently bound to its owner worker;
  every later resume is placed on that worker's local queue.
- `Actor::start` and `CoroutineContext::spawn` only enqueue work. They never
  switch contexts inside the notification or `onMessage` call stack.
- A fiber holds an `mmap` allocation with one inaccessible guard page at each
  end. The default usable stack is 256 KiB, so each live fiber reserves roughly
  264 KiB on a 4 KiB-page machine. The stack is unmapped only after control has
  returned to the scheduler stack.
- Each job processes one message, event, or resume. Contended Actor jobs are
  fairly requeued with a short backoff, preventing a hot Actor from permanently
  monopolizing a worker.
- Exceptions never unwind across a context boundary. Root-fiber failures call
  `onCoroutineError`; child failures are returned by a child wait as `Error`.

The implementation really calls `getcontext`, `makecontext`, and `swapcontext`.
`ucontext` is not portable C++ and was removed from POSIX.1-2008; this project is
therefore intentionally restricted to Linux/glibc.

## Messages and request correlation

`ActorRef::send` creates a non-zero process-local message instance ID. A reply
uses the request's ID as its `correlationId`. Message waits match the exact
`(typeId, correlationId)` pair. Dispatch first broadcasts one immutable
`MessagePtr` to every waiter that existed at dispatch start, then invokes
`onMessage` exactly once. Waiters created by that `onMessage` cannot observe the
same message.

The `Message` object is immutable through `shared_ptr<const Message>`, but
`std::any` cannot enforce deep immutability of its contained object. Put only
immutable/copyable values in it. Move-only or large payloads should be wrapped
in `std::shared_ptr<const T>`; mutable pointees would remain mutable by user code.

An `ActorRef` must not outlive its `ActorSystem`. An empty ref, an unknown actor,
or a send after shutdown throws a documented standard exception.

## Waiting and children

Leaves are explicit: `WaitSpec::message`, `WaitSpec::child`, and
`WaitSpec::timer`. Composites are ordinary runtime trees:

```cpp
auto result = context.await(WaitSpec::all({
    WaitSpec::message({Reply, requestId}),
    WaitSpec::any({
        WaitSpec::child(childId),
        WaitSpec::timer(std::chrono::steady_clock::now() + 100ms)
    })
}));
```

`Any` records the winning direct `branchIndex` and deactivates every losing
leaf. `All` preserves specification order in `children`, regardless of event
completion order. Timer leaves return `Timeout`; cooperative cancellation
returns `Cancelled`. `WaitResult::rethrowIfError()` rethrows a child exception.
Empty `Any`/`All`/`AllSettled` and unknown child IDs throw `std::invalid_argument`.

`All` retains the original wait-for-every-branch behavior (it is not a fail-fast
Promise.all). Once all branches finish, it aggregates `Error` before `Cancelled`
and preserves the first error in specification order for `rethrowIfError()`.
`AllSettled` also waits for every branch but returns `WakeReason::Settled`, with
no aggregate exception; each ordered child retains its own success, exception,
cancellation, or timeout. Both forms can contain arbitrary nested composites.
Cancellation of the waiting coroutine itself always returns a top-level
`Cancelled`, even for `AllSettled`; it does not wait forever for remaining sources.

`context.cancel(childId)` requests cancellation of a direct child. It returns
`true` only for the first request on a live child, and `false` for an already
cancelled/completed child. Unknown or foreign children throw `invalid_argument`.
It never resumes the child inline. Call `await(WaitSpec::child(childId))` to wait
for that child's cleanup. A cancelled child should call `throwIfCancelled()`
after releasing resources to finish with a `Cancelled` completion; a body that
explicitly handles cancellation and returns a value finishes normally.
Cancelled coroutines cannot spawn additional work. Cancellation cascades through
`CancelWithParent` descendants; each descendant still cleans up on its own owner
worker. Joining a parent alone does not imply all descendants have already exited.
An `Any` winner deactivates losing *waits*; it does not cancel their child fibers.
The caller must cancel unwanted children explicitly.

## 中文业务模拟与覆盖率验收

`examples/order_fulfillment.cpp` 开头的中文注释说明完整业务流程。它既是
可执行示例，也是 CTest 集成测试：报价竞速 → 库存/风控汇聚 → 支付/撤单/
超时竞速 → 审计通知收尾。以 1 和 4 个 worker 分别验证成交、缺货、支付
超时、用户撤单，断言实际消息关联、多层 child 取消、AllSettled 中的失败
隔离，以及每个模拟资源的 RAII 析构计数。所有外部等待都有测试超时保护。

覆盖率使用 WSL GCC/gcov 的真实计数，门禁要求运行库行覆盖率至少 90%。
分母包含 `src/*.cpp`、内部头文件及 `include/actor_runtime/*.h` 的可执行行，
不包含测试、示例、标准库。当前不删除防御代码，也不使用排除宏缩小分母；
可公开触发的参数校验、重复取消、异常处理都通过真实测试覆盖。分支覆盖率
另行展示，包含 GCC 生成的 C++ 异常边，不能与行覆盖率混用。

| CTest 目标 | 覆盖内容 |
| --- | --- |
| `actor_runtime_tests` | 调度、消息、Any/All/AllSettled、迁移、公平性、栈回收 |
| `wait_semantics_tests` | 等待语义边界：立即完成、陈旧注册、取消粘性、顺序保持 |
| `api_contract_tests` | 单例契约、参数校验、异常隔离、幂等停止与重启 |
| `actor_system_lifecycle_tests` | `start`/`instance`/`stop` 生命周期、子模块注册、worker 线程拒绝停止 |
| `asan_fiber_tests` | ASan 注解切换压力、挂起前后局部变量有效、ASan 状态断言 |
| `order_fulfillment` | 中文业务链路的集成验证（1/4 worker × 4 条路径） |
| `module_registration` | 跨编译单元的子模块注册示例 |

```bash
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DACTOR_RUNTIME_COVERAGE=ON
cmake --build build-coverage --target coverage -j4
```

`coverage` 目标先清空该构建目录中的旧 `.gcda` 计数，再运行全部 CTest，
任一测试失败或覆盖率不足都会返回非零状态。结果输出到
`build-coverage/coverage/report.md` 和 `report.json`，JSON 同时列出未覆盖行。
多 worker 计数使用 `-fprofile-update=atomic` 防止并发丢计数；不需要第三方
覆盖率包，只需 GCC/gcov、CMake 和 Python 3.9+。不要同时启动多个 coverage
任务共用同一构建目录。

Every spawn states a policy. `CancelWithParent` is cooperatively cancelled when
its parent ends. `DetachedFromParent` may outlive normal or exceptional parent
completion, but is still cancelled when its Actor or ActorSystem shuts down.
Cancellation is observed at await/resume boundaries; the runtime never destroys
an executing stack asynchronously.

## Shutdown

`ActorSystem::stop()` first rejects Actor creation and external sends, requests
cancellation for every fiber, resumes suspended fibers with `Cancelled`, drains
scheduler jobs up to the configured shutdown deadline, stops and joins every
worker, then destroys actors and mappings. Cooperative bodies must return, await,
or call `throwIfCancelled`; a body that computes forever cannot be forcibly
unwound safely. `RuntimeStats` exposes live fiber and stack counts for shutdown
checks. Note that `liveFibers` and `liveStacks` are decremented one after the
other, so a shutdown check must wait for both to reach zero.

## AddressSanitizer 协程切换 hook

The runtime calls the AddressSanitizer fiber annotation interface
(`__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber`) at every
stack switch, so ASan tracks the alternate stacks, the fake stacks used by
`detect_stack_use_after_return`, and the moment a fiber stack is destroyed.
Call sites, all in the switching code paths:

| 切换 | `start_switch` 的目标栈 | `finish_switch` 的位置 |
| --- | --- | --- |
| 调度器 -> 协程 (`resumeFiber`) | 协程栈 | 回到调度器栈后 |
| 协程 -> 调度器 (`await`) | worker 线程栈 | 被恢复后的协程栈上 |
| 协程首次进入 (`fiberTrampoline`) | — | 进入协程栈后 |
| 协程永久退出 (`fiberTrampoline` 尾部) | worker 线程栈，`fake_stack_save` 传 `nullptr` 销毁 fake stack | 回到调度器栈后 |

- 两个 hook 声明为**弱符号**，运行库不需要在编译期依赖 ASan：链接了 ASan 运行时
  就调用真实实现，否则退化为空操作。因此普通构建与 ASan 构建共用同一份代码。
- 每个 worker 线程启动时通过 `pthread_getattr_np` 取得自身栈范围，作为切回
  调度器栈时的目标描述；取不到范围时该 worker 关闭注解（在 glibc 上不会发生）。
- 协程栈本身是可用的 `mmap` 区间（`usable` / `usableSize`），两端各有一个
  `PROT_NONE` 保护页。
- `RuntimeStats::asanFiberHooks` 报告进程内是否解析到了 hook 实现，
  `RuntimeStats::fiberSwitches` 统计带注解的协程栈切换次数，两者都可用于
  CI 断言。
- ASan 自身会对 `makecontext`/`swapcontext` 打印一次
  `WARNING: ASan doesn't fully support makecontext/swapcontext ...`。这是
  ASan 的拦截器提示，注解接口正是为此提供的补救手段；用例在该警告下仍然全绿。
- `ACTOR_RUNTIME_ASAN=ON` 会把运行库与用例一起插桩，`asan_fiber_tests` 在
  `detect_stack_use_after_return=1`、`detect_leaks=1`、`halt_on_error=1` 下运行
  30 轮嵌套 spawn/cancel/挂起/恢复（约 780 次切换），并在挂起前后校验逃逸到
  fake stack 上的局部变量仍然有效。覆盖率构建不开 ASan，因此
  `src/asan_fiber.cpp` 中真正调用 hook 的两行在覆盖率报告里不计入已覆盖，
  整体门禁仍然是 90%（当前约 96.6%）。

## Known limits

`ucontext` is deprecated and this project is deliberately restricted to
Linux/glibc. `makecontext` is called through a convention that UBSan cannot
fully validate, so UBSan is not claimed even though the automated suite passes.
The ASan fiber hooks remove the largest source of false positives on alternate
stacks, but a signal delivered while a fiber switch is in progress still runs
outside the fake-stack machinery — that limitation is documented by ASan itself.
`pthread_getattr_np` is a glibc extension, which is why the worker stack bounds
cannot be captured on other libc implementations.

Beyond that, the suite stresses 22 public behaviors, including repeated
message/timeout races, broadcast, nested waits, parent/child policies, Actor
serialization and migration, owner-worker affinity, fairness, stack
preservation, complete shutdown, the singleton lifecycle, submodule
registration, and the annotated switch stress run.
