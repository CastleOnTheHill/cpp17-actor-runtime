# C++17 single-process actor runtime

A small Linux/glibc actor runtime with multiple worker threads and cooperative,
stackful `ucontext` fibers. It is deliberately a deep module: callers use the
non-template `ActorSystem`, `ActorRef`, `Actor`, `CoroutineContext`, and runtime
`WaitSpec` tree while mailbox, scheduling, wait registration, timers, context
switching, and stack lifetime remain internal.

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
```

Requirements: Linux/glibc, a C++17 compiler, CMake 3.16 or newer, and pthreads.
There are no third-party runtime or test dependencies.

## Execution model and invariants

- An Actor has at most one executor at any instant. Each mailbox dispatch and
  fiber resume acquires the same Actor execution token.
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

Shutdown first rejects Actor creation and external sends, requests cancellation
for every fiber, resumes suspended fibers with `Cancelled`, drains scheduler
jobs up to the configured shutdown deadline, stops and joins every worker, then
destroys actors and mappings. Cooperative bodies must return, await, or call
`throwIfCancelled`; a body that computes forever cannot be forcibly unwound
safely. `RuntimeStats` exposes live fiber and stack counts for shutdown checks.

## Sanitizers and known limits

The runtime does not yet call the AddressSanitizer fiber-switch hooks
(`__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber`). ASan may
therefore report false positives or mishandle alternate stacks, so an ASan pass
is not claimed. UBSan coverage around deprecated `makecontext` calling
conventions is likewise platform-sensitive. The automated suite instead
stresses 19 public behaviors including repeated message/timeout races,
broadcast, nested waits, parent/child policies, Actor serialization and
migration, owner-worker affinity, fairness, stack preservation, and complete
shutdown. Production hardening should add sanitizer hooks before treating ASan
as authoritative.
