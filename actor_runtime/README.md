# C++17 Linux actor runtime

A small Linux/glibc actor runtime with multiple worker threads and cooperative,
stackful `ucontext` fibers. It is deliberately a deep module: callers use the
non-template `ActorSystem`, `ActorRef`, `Actor`, `CoroutineContext`, and runtime
`WaitSpec` tree while mailbox, scheduling, wait registration, timers, context
switching, and stack lifetime remain internal.

Each system is a single-process scheduler. Named systems communicate directly
using Linux Unix-domain datagram sockets, without a broker. Local messages keep
`std::any`; IPC messages carry copied bytes. There is no persistence, delivery
retry, automatic serialization, or distributed supervision.

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

Requirements: Linux/glibc, C/C++17 compilers, CMake 3.16 or newer, and pthreads.
Python 3 is used by the multi-process test launcher; there are no third-party
runtime dependencies. Development is on `dev`; teaching material is on `teaching`.

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

Leaves are explicit: `WaitSpec::message`, `WaitSpec::child`, `WaitSpec::task`, and
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

Shutdown first rejects Actor creation and external sends, stops the IPC receive
thread, requests cancellation for every fiber, and resumes suspended fibers with
`Cancelled`. It drains and joins the blocking pool while scheduler workers are
still alive, drains scheduler jobs up to the configured shutdown deadline, then
stops and joins every worker and destroys actors and mappings. The shutdown
deadline does not forcibly terminate native functions or CPU-bound actors.
Cooperative bodies must return, await, or call
`throwIfCancelled`; a body that computes forever cannot be forcibly unwound
safely. `RuntimeStats` exposes live fiber and stack counts for shutdown checks.

## Named IPC / 跨进程寻址

```cpp
ActorSystem::Config config;
config.processName = "inventory";
config.diagnosticLogging = true;
ActorSystem system(config);
auto stock = system.registerActor("stock", std::make_unique<StockActor>());
// In an Actor coroutine, the named sender is filled automatically.
struct Request { uint64_t order; uint32_t quantity; uint32_t reserved; };
Request request{42, 2, 0};
auto id = system.sendBytes({"billing", "payment"}, RequestType,
                           &request, sizeof(request));
// Await message({ReplyType, id}) against a timer, as for local requests.
// Receiver uses any_cast<Bytes>, checks size, then memcpy into a local struct.
// system.replyBytes(originalRequest, ReplyType, &reply, sizeof(reply));
```

- `ActorAddress{process, actor}` allows identical Actor names in different
  processes. Names contain 1–63 ASCII letters, digits, `_`, `-`, or `.`.
  Empty `Config::processName` disables remote transport; empty target process
  selects the local system. Process names must be unique within the socket
  namespace. Named Actors must be unique within their system.
- One nonblocking `AF_UNIX/SOCK_DGRAM` socket per process, using abstract address
  `actor-runtime.<uid>.<process>` in the same Linux network namespace. The
  transport is peer-to-peer; the kernel releases the socket on process exit.
  A receive thread validates complete datagrams and enqueues Actor messages.
- Maximum payload is 60 KiB. The wire contract is local, trusted, same ABI and
  endianness: use pointer-free, trivially copyable C structs, fixed-width fields,
  identical packing, and initialized padding. Never transmit pointers, STL
  objects, or the object representation of `std::any`. Copy `Bytes` with
  `memcpy` into an aligned local object; do not reinterpret an unaligned buffer.
  This is not a portable serialization or a security/authentication boundary.
- Send copies data before returning. Missing peer, full socket queue, oversized
  payload, and bad names fail immediately; no retry is hidden. A successful
  datagram send does not acknowledge handler execution. Unknown remote Actors
  are dropped; use a reply deadline. Malformed datagrams are rejected.
- Requests carry correlation and sender-instance IDs. `replyBytes` targets the
  original instance, not a new Actor that reused its name. Actor IDs include a
  random process incarnation (probabilistic protection across restarts).
  Reply-capable senders must be named. External C++ sends can supply the named
  sender's ActorId; the C client bridge does this automatically.

`tests/c_api_process_test.c` and `tests/run_ipc_tests.py` form an executable
multi-process example: two services with the same `echo` Actor name perform
route-specific request/reply work for a separate C client process. CTest runs
100 exchanges plus timeout, malformed packet, and shutdown cases.

## Dynamic Actor lifecycle / 动态注册与消亡

`registerActor(name, unique_ptr<Actor>)` publishes an instance;
`findActor(name)` returns its ref. `stopActor(id)` immediately unpublishes the
name and returns a `shared_future<void>`: cancellation is queued to owner
workers, suspended fibers unwind cooperatively, and the future becomes ready
after the Actor destructor. A replacement may register the same name while
the old instance retires. Old refs and addressed replies cannot reach it.

Wait on the stop future only from an external thread, never an Actor worker.
Repeated stops during retirement return the same future; unknown/already
destroyed IDs throw. This is dynamic instance registration, **not** `dlopen`
shared-library loading. Handles/refs must not outlive their ActorSystem.

## Blocking functions / 阻塞函数隔离

```cpp
auto result = context.blocking(
    [ownedInput]() -> std::any { return legacyBlockingCall(ownedInput); },
    std::chrono::steady_clock::now() + 100ms);
// Any result: branch 0 = task completion/error, branch 1 = timeout.
result.rethrowIfError();
```

For later joining, use `submitBlocking(work)` and `WaitSpec::task(taskId)`.
Tasks belong to the submitting fiber and compose with message/child/timer
waits. Exceptions become `Error`; repeated joins return the saved result.
`Config::blockingThreads` (default 2) and `blockingQueueCapacity` (default 64)
bound the executor; capacity counts pending work, not running work. Full queues
reject synchronously. Actor workers never execute the submitted native work.

A timeout deactivates the wait, **not the running function**. You may join its
task again while the submitting fiber remains alive. Capture owned values or
safe shared state, never raw Actor/Context pointers: Actor destruction need not
wait for native work. Late results for dead fibers/Actors are discarded.
System shutdown drains accepted tasks and joins their threads, so native
functions must eventually return or implement their own cooperative deadline.
There is no safe forced cancellation of an arbitrary C/C++ function.

## Pure C integration / 纯 C 接口

Include `actor_runtime/c_api.h` from C11 code and link `actor_runtime` with the
C++ linker. `ar_system_create` owns a runtime; a C++ host can instead pass its
`ActorSystem*` to `ar_system_borrow`. The borrowed wrapper does not own it.

Register a C callback with `ar_actor_register`; use `ar_reply` inside that
callback. Payload, `ar_message`, internal request token, and callback system view
are valid only during the callback. Copy data needed later. Callbacks execute
under Actor serialization and must not block. The cleanup callback runs when
the adapter is destroyed (including a failed registration after its creation).

An external native thread uses `ar_client_create` and `ar_client_request` to
send bytes to `(process, actor)` and wait for the matching reply type/ID. Calls
on one client serialize; late replies after timeout are discarded. Status codes
distinguish invalid input, timeout, closed client, small output buffer, and
other errors; `ar_last_error` is thread-local. `AR_BUFFER_SMALL` reports the
required length but consumes that response. `ar_actor_stop` waits for cleanup
with a deadline; a timeout does not undo the stop request.

Destroy client handles before the owned system; a borrowed host must outlive
its handles. Do not destroy a handle concurrently with API calls using it.
`ar_client_request`, synchronous Actor stop, and system destruction are not
allowed on Actor workers. See the actual C integration example/test above.

## Diagnostics and sanitizer validation

Set `Config::diagnosticLogging = true` to log monotonic milliseconds, PID,
process, worker, event, Actor and fiber IDs to stderr. Events cover registration,
stop/cancellation, handler failures, IPC receive/send/reject/dead-letter, and
blocking task submission/completion/late-result discard. Payloads are not logged.
CTest preserves test output in `build*/Testing/Temporary/LastTest.log`.

The runtime calls `__sanitizer_start_switch_fiber` and
`__sanitizer_finish_switch_fiber` around scheduler/fiber transitions, including
first entry and final exit. Linux/glibc with GCC in WSL is the verified platform.
ASan may still print its generic `makecontext/swapcontext` support warning;
no error suppression is configured. A passing suite is not proof of all possible
concurrency interleavings or support for other libc/compiler combinations.

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DACTOR_RUNTIME_SANITIZERS=ON
cmake --build build-asan -j4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```
