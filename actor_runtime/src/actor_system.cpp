#include "actor_runtime/actor_system.h"

#include "actor_runtime/actor.h"
#include "internal.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

namespace actor_runtime {

thread_local ActorControl* currentActor = nullptr;
thread_local Worker* currentWorker = nullptr;
thread_local Fiber* currentFiber = nullptr;

namespace {

constexpr std::size_t noLeaf = static_cast<std::size_t>(-1);

void fiberTrampoline(std::uint32_t low, std::uint32_t high) {
    const auto id = static_cast<FiberId>(low) | (static_cast<FiberId>(high) << 32U);
    Fiber* fiber = currentFiber;
    if (fiber == nullptr || fiber->id != id) {
        std::terminate();
    }
    Worker& worker = *fiber->owner;
    // 首次进入该协程栈：完成调度器发起的切换注解（此时本栈还没有 fake stack）。
    if (worker.stackAnnotated) detail::asanFinishSwitch(fiber->asanFakeStack);
    CoroutineContext context(&fiber->contextImpl);
    try {
        fiber->result = fiber->body(context);
        fiber->state = FiberState::Done;
    } catch (const OperationCancelled&) {
        fiber->cancelled = true;
        fiber->state = FiberState::Cancelled;
    } catch (...) {
        fiber->error = std::current_exception();
        fiber->state = FiberState::Done;
    }
    fiber->finishedOnStack = true;
    // 协程栈永久退出：销毁它的 ASan fake stack（传 nullptr），然后显式切回调度器栈。
    if (worker.stackAnnotated) {
        detail::asanStartSwitch(nullptr, worker.stackBottom, worker.stackSize);
    }
    if (::swapcontext(&fiber->context, &worker.schedulerContext) != 0) {
        std::terminate();
    }
}

bool evaluate(RuntimeWaitNode& node) {
    if (node.completed) return true;
    if (node.kind != WaitKind::Any && node.kind != WaitKind::All && node.kind != WaitKind::AllSettled) return false;

    for (auto& child : node.children) evaluate(*child);
    if (node.kind == WaitKind::Any) {
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            if (node.children[index]->completed) {
                node.completed = true;
                node.result = node.children[index]->result;
                node.result.branchIndex = index;
                return true;
            }
        }
        return false;
    }

    if (!std::all_of(node.children.begin(), node.children.end(), [](const auto& child) {
            return child->completed;
        })) {
        return false;
    }
    node.completed = true;
    node.result.children.clear();
    node.result.children.reserve(node.children.size());
    node.result.reason = node.kind == WaitKind::AllSettled ? WakeReason::Settled : WakeReason::ChildCompleted;
    for (const auto& child : node.children) {
        node.result.children.push_back(child->result);
        if (node.kind == WaitKind::AllSettled) continue;
        if (child->result.reason == WakeReason::Error) {
            node.result.reason = WakeReason::Error;
            if (!node.result.error) node.result.error = child->result.error;
        }
        else if (child->result.reason == WakeReason::Cancelled && node.result.reason != WakeReason::Error) {
            node.result.reason = WakeReason::Cancelled;
        }
    }
    return true;
}

WaitResult cancelledResult() {
    WaitResult result;
    result.reason = WakeReason::Cancelled;
    return result;
}

void decrementJobs(ActorSystem::Impl& system) {
    if (system.outstandingJobs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        system.stateCv.notify_all();
    } else {
        system.stateCv.notify_all();
    }
}

} // namespace

StackMapping::StackMapping(std::size_t requestedSize) {
    const long rawPageSize = ::sysconf(_SC_PAGESIZE);
    if (rawPageSize <= 0) throw std::runtime_error("sysconf(_SC_PAGESIZE) failed");
    const auto pageSize = static_cast<std::size_t>(rawPageSize);
    usableSize = std::max(requestedSize, 4U * pageSize);
    usableSize = ((usableSize + pageSize - 1U) / pageSize) * pageSize;
    mappingSize = usableSize + 2U * pageSize;
    mapping = ::mmap(nullptr, mappingSize, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        mapping = nullptr;
        throw std::system_error(errno, std::generic_category(), "mmap fiber stack");
    }
    auto* bytes = static_cast<unsigned char*>(mapping);
    if (::mprotect(bytes, pageSize, PROT_NONE) != 0 ||
        ::mprotect(bytes + pageSize + usableSize, pageSize, PROT_NONE) != 0) {
        const int error = errno;
        ::munmap(mapping, mappingSize);
        mapping = nullptr;
        throw std::system_error(error, std::generic_category(), "mprotect fiber guard page");
    }
    usable = bytes + pageSize;
}

StackMapping::~StackMapping() {
    if (mapping != nullptr) ::munmap(mapping, mappingSize);
}

ActorSystem::Impl::Impl(ActorSystem& facadeValue, Config options)
    : facade(facadeValue), config(options) {
    if (config.workerCount == 0) {
        config.workerCount = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    }
    if (config.fiberStackSize < 16U * 1024U) {
        throw std::invalid_argument("fiber stack size must be at least 16 KiB");
    }
    workers.reserve(config.workerCount);
    for (std::size_t index = 0; index < config.workerCount; ++index) {
        auto worker = std::make_unique<Worker>();
        worker->system = this;
        worker->index = index;
        workers.push_back(std::move(worker));
    }
    for (auto& worker : workers) {
        worker->thread = std::thread([this, workerPtr = worker.get()] { workerLoop(*workerPtr); });
    }
}

ActorSystem::Impl::~Impl() = default;

void ActorSystem::Impl::enqueueGlobal(Job job) {
    outstandingJobs.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        globalQueue.push_back(std::move(job));
    }
    queueCv.notify_one();
}

void ActorSystem::Impl::enqueueLocal(Worker& worker, Job job) {
    outstandingJobs.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(worker.localMutex);
        worker.localQueue.push_back(std::move(job));
    }
    queueCv.notify_all();
}

void ActorSystem::Impl::captureWorkerStack(Worker& worker) {
    ::pthread_attr_t attributes;
    if (::pthread_getattr_np(::pthread_self(), &attributes) != 0) return;
    void* address = nullptr;
    std::size_t size = 0;
    const int status = ::pthread_attr_getstack(&attributes, &address, &size);
    ::pthread_attr_destroy(&attributes);
    if (status != 0 || address == nullptr || size == 0) return;
    worker.stackBottom = address;
    worker.stackSize = size;
    worker.stackAnnotated = true;
}

void ActorSystem::Impl::workerLoop(Worker& worker) {
    currentWorker = &worker;
    captureWorkerStack(worker);
    for (;;) {
        std::optional<Job> selected;
        {
            std::lock_guard<std::mutex> lock(worker.localMutex);
            if (!worker.localQueue.empty()) {
                selected = std::move(worker.localQueue.front());
                worker.localQueue.pop_front();
            }
        }
        if (!selected) {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (!globalQueue.empty()) {
                selected = std::move(globalQueue.front());
                globalQueue.pop_front();
            }
        }
        if (!selected && !stopping.load(std::memory_order_acquire) && !worker.timers.empty() &&
            worker.timers.top().deadline <= std::chrono::steady_clock::now()) {
            auto timer = worker.timers.top();
            worker.timers.pop();
            if (auto actor = timer.actor.lock()) {
                outstandingJobs.fetch_add(1, std::memory_order_relaxed);
                selected = Job{JobKind::TimerFire, std::move(actor), timer.fiberId,
                    timer.token, timer.leafIndex};
            }
        }
        if (selected) {
            processJob(worker, std::move(*selected));
            continue;
        }

        if (stopping.load(std::memory_order_acquire)) {
            bool localEmpty;
            {
                std::lock_guard<std::mutex> lock(worker.localMutex);
                localEmpty = worker.localQueue.empty();
            }
            bool globalEmpty;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                globalEmpty = globalQueue.empty();
            }
            if (localEmpty && globalEmpty) break;
            continue;
        }

        std::unique_lock<std::mutex> lock(queueMutex);
        if (worker.timers.empty()) {
            queueCv.wait_for(lock, std::chrono::milliseconds(10), [&] {
                return stopping.load(std::memory_order_acquire) || !globalQueue.empty();
            });
        } else {
            const auto periodicWake = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
            queueCv.wait_until(lock, std::min(worker.timers.top().deadline, periodicWake), [&] {
                return stopping.load(std::memory_order_acquire) || !globalQueue.empty();
            });
        }
    }
    currentWorker = nullptr;
}

void ActorSystem::Impl::processJob(Worker& worker, Job job) {
    std::unique_lock<std::mutex> actorLock(job.actor->executionMutex, std::try_to_lock);
    if (!actorLock.owns_lock()) {
        if (job.kind == JobKind::ResumeFiber || job.kind == JobKind::TimerFire) {
            enqueueLocal(worker, std::move(job));
        } else {
            enqueueGlobal(std::move(job));
        }
        decrementJobs(*this);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        return;
    }

    currentActor = job.actor.get();
    try {
        switch (job.kind) {
        case JobKind::DispatchMessage: {
            MessagePtr message;
            {
                std::lock_guard<std::mutex> mailboxLock(job.actor->mailboxMutex);
                if (!job.actor->mailbox.empty()) {
                    message = std::move(job.actor->mailbox.front());
                    job.actor->mailbox.pop_front();
                }
            }
            if (message) {
                notifyMessage(job.actor, message);
                try {
                    job.actor->actor->onMessage(std::move(message));
                } catch (...) {
                    // Handler exceptions are isolated at the scheduler seam.
                }
            }
            break;
        }
        case JobKind::ResumeFiber:
            resumeFiber(worker, job.actor, job.fiberId);
            break;
        case JobKind::TimerFire: {
            WaitResult result;
            result.reason = WakeReason::Timeout;
            triggerWait(job.actor, LeafRef{job.fiberId, job.token, job.leafIndex}, std::move(result));
            break;
        }
        case JobKind::CancelActor: {
            job.actor->stopping = true;
            std::vector<FiberId> ids;
            ids.reserve(job.actor->fibers.size());
            for (const auto& item : job.actor->fibers) ids.push_back(item.first);
            for (const auto id : ids) {
                const auto found = job.actor->fibers.find(id);
                if (found != job.actor->fibers.end()) requestCancellation(job.actor, *found->second);
            }
            break;
        }
        }
    } catch (...) {
        // Runtime jobs must not terminate a worker.
    }
    currentFiber = nullptr;
    currentActor = nullptr;
    actorLock.unlock();
    decrementJobs(*this);
}

FiberId ActorSystem::Impl::createFiber(
    const std::shared_ptr<ActorControl>& actor,
    Worker& owner,
    Actor::CoroutineBody body,
    FiberId parentId,
    ChildLifetime lifetime) {
    if (!body) throw std::invalid_argument("coroutine body must not be empty");
    if (actor->stopping) throw std::runtime_error("actor is stopping");

    auto fiber = std::make_unique<Fiber>();
    fiber->id = nextFiberId.fetch_add(1, std::memory_order_relaxed);
    fiber->actor = actor.get();
    fiber->owner = &owner;
    fiber->stack = std::make_unique<StackMapping>(config.fiberStackSize);
    fiber->body = std::move(body);
    fiber->contextImpl.fiber = fiber.get();
    fiber->parentId = parentId;
    fiber->lifetime = lifetime;
    if (::getcontext(&fiber->context) != 0) {
        throw std::system_error(errno, std::generic_category(), "getcontext");
    }
    fiber->context.uc_stack.ss_sp = fiber->stack->usable;
    fiber->context.uc_stack.ss_size = fiber->stack->usableSize;
    fiber->context.uc_stack.ss_flags = 0;
    // uc_link 只是兜底：协程正常结束时由 trampoline 显式 swapcontext 回调度器栈，
    // 这样 ASan 的两次栈切换注解才是成对的。
    fiber->context.uc_link = &owner.schedulerContext;
    const auto low = static_cast<std::uint32_t>(fiber->id & 0xffffffffULL);
    const auto high = static_cast<std::uint32_t>(fiber->id >> 32U);
    ::makecontext(&fiber->context, reinterpret_cast<void (*)()>(&fiberTrampoline), 2, low, high);

    const FiberId id = fiber->id;
    actor->fibers.emplace(id, std::move(fiber));
    if (parentId != 0) {
        const auto parent = actor->fibers.find(parentId);
        if (parent == actor->fibers.end()) throw std::logic_error("parent fiber disappeared");
        parent->second->children.emplace(id, lifetime);
    }
    liveFibers.fetch_add(1, std::memory_order_relaxed);
    liveStacks.fetch_add(1, std::memory_order_relaxed);
    enqueueLocal(owner, Job{JobKind::ResumeFiber, actor, id, 0, 0});
    return id;
}

void ActorSystem::Impl::resumeFiber(
    Worker& worker,
    const std::shared_ptr<ActorControl>& actor,
    FiberId id) {
    const auto found = actor->fibers.find(id);
    if (found == actor->fibers.end()) return;
    Fiber& fiber = *found->second;
    if (fiber.owner != &worker || fiber.state != FiberState::Ready) return;
    if (fiber.cancellationRequested && !fiber.started) {
        fiber.cancelled = true;
        fiber.state = FiberState::Cancelled;
        finishFiber(actor, id);
        return;
    }
    if (fiber.cancellationRequested && fiber.started) fiber.pendingWait = cancelledResult();

    fiber.state = FiberState::Running;
    fiber.started = true;
    currentFiber = &fiber;
    // 调度器栈 -> 协程栈：注解目标栈为协程栈，并保存调度器栈的 fake stack。
    const bool annotate = worker.stackAnnotated;
    if (annotate) {
        detail::asanStartSwitch(
            &worker.schedulerFakeStack, fiber.stack->usable, fiber.stack->usableSize);
    }
    const int status = ::swapcontext(&worker.schedulerContext, &fiber.context);
    const int savedErrno = errno;
    if (annotate) {
        // 回到调度器栈：恢复调度器栈的 fake stack。
        detail::asanFinishSwitch(worker.schedulerFakeStack);
        fiberSwitches.fetch_add(1, std::memory_order_relaxed);
    }
    if (status != 0) {
        currentFiber = nullptr;
        throw std::system_error(savedErrno, std::generic_category(), "swapcontext to fiber");
    }
    currentFiber = nullptr;
    const auto again = actor->fibers.find(id);
    if (again != actor->fibers.end() && again->second->finishedOnStack) finishFiber(actor, id);
}

void ActorSystem::Impl::finishFiber(const std::shared_ptr<ActorControl>& actor, FiberId id) {
    const auto found = actor->fibers.find(id);
    if (found == actor->fibers.end()) return;
    Fiber& fiber = *found->second;
    fiber.wait.reset();

    Completion completion{fiber.result, fiber.error, fiber.cancelled, fiber.parentId};
    actor->completions[id] = completion;

    std::vector<std::pair<FiberId, ChildLifetime>> children;
    children.reserve(fiber.children.size());
    for (const auto& item : fiber.children) children.push_back(item);
    for (const auto& item : children) {
        const auto child = actor->fibers.find(item.first);
        if (child == actor->fibers.end()) continue;
        if (item.second == ChildLifetime::CancelWithParent) {
            requestCancellation(actor, *child->second);
        } else {
            child->second->parentId = 0;
        }
    }

    const auto subscribers = actor->childWaiters.find(id);
    if (subscribers != actor->childWaiters.end()) {
        const auto snapshot = subscribers->second;
        for (const auto& leaf : snapshot) {
            WaitResult result;
            if (completion.cancelled) {
                result.reason = WakeReason::Cancelled;
            } else if (completion.error) {
                result.reason = WakeReason::Error;
                result.error = completion.error;
            } else {
                result.reason = WakeReason::ChildCompleted;
                result.value = completion.result;
            }
            triggerWait(actor, leaf, std::move(result));
        }
        actor->childWaiters.erase(subscribers);
    }

    if (fiber.parentId == 0 && fiber.error) {
        try {
            actor->actor->onCoroutineError(id, fiber.error);
        } catch (...) {
            // Error hooks are isolated like message handlers.
        }
    }

    auto finished = std::move(found->second);
    actor->fibers.erase(found);
    liveFibers.fetch_sub(1, std::memory_order_relaxed);
    finished.reset();
    liveStacks.fetch_sub(1, std::memory_order_relaxed);
    stateCv.notify_all();
}

void ActorSystem::Impl::requestCancellation(
    const std::shared_ptr<ActorControl>& actor,
    Fiber& fiber) {
    if (fiber.cancellationRequested) return;
    fiber.cancellationRequested = true;
    std::vector<FiberId> cascade;
    for (const auto& child : fiber.children) {
        if (child.second == ChildLifetime::CancelWithParent) cascade.push_back(child.first);
    }
    for (const auto childId : cascade) {
        const auto found = actor->fibers.find(childId);
        if (found != actor->fibers.end()) requestCancellation(actor, *found->second);
    }
    if (fiber.state == FiberState::Waiting) {
        if (fiber.wait) fiber.wait->active = false;
        fiber.pendingWait = cancelledResult();
        fiber.state = FiberState::Ready;
        enqueueLocal(*fiber.owner, Job{JobKind::ResumeFiber, actor, fiber.id, 0, 0});
    }
}

std::optional<WaitResult> ActorSystem::Impl::installWait(Fiber& fiber, const WaitSpec& spec) {
    auto registration = std::make_unique<WaitRegistration>();
    registration->token = fiber.nextWaitToken++;

    std::function<std::unique_ptr<RuntimeWaitNode>(const WaitSpec&)> build;
    build = [&](const WaitSpec& current) -> std::unique_ptr<RuntimeWaitNode> {
        const auto& source = *WaitSpecAccess::node(current);
        auto node = std::make_unique<RuntimeWaitNode>();
        node->kind = source.kind;
        node->messageKey = source.messageKey;
        node->childId = source.childId;
        node->deadline = source.deadline;
        if (source.kind == WaitKind::Any || source.kind == WaitKind::All || source.kind == WaitKind::AllSettled) {
            node->children.reserve(source.children.size());
            for (const auto& child : source.children) node->children.push_back(build(child));
        } else {
            node->leafIndex = registration->leaves.size();
            registration->leaves.push_back(node.get());
        }
        return node;
    };
    registration->root = build(spec);

    auto actor = fiber.actor->actor->impl_->control.lock();
    if (!actor) throw std::runtime_error("actor no longer exists");
    for (auto* leaf : registration->leaves) {
        const LeafRef reference{fiber.id, registration->token, leaf->leafIndex};
        if (leaf->kind == WaitKind::Message) {
            actor->messageWaiters[leaf->messageKey].push_back(reference);
        } else if (leaf->kind == WaitKind::Child) {
            bool known = fiber.children.find(leaf->childId) != fiber.children.end();
            const auto active = actor->fibers.find(leaf->childId);
            const auto completed = actor->completions.find(leaf->childId);
            if (!known || (active == actor->fibers.end() && completed == actor->completions.end())) {
                throw std::invalid_argument("WaitSpec::child refers to an unknown child");
            }
            if (completed != actor->completions.end()) {
                leaf->completed = true;
                if (completed->second.cancelled) leaf->result.reason = WakeReason::Cancelled;
                else if (completed->second.error) {
                    leaf->result.reason = WakeReason::Error;
                    leaf->result.error = completed->second.error;
                } else {
                    leaf->result.reason = WakeReason::ChildCompleted;
                    leaf->result.value = completed->second.result;
                }
            } else {
                actor->childWaiters[leaf->childId].push_back(reference);
            }
        } else if (leaf->kind == WaitKind::Timer) {
            if (leaf->deadline <= std::chrono::steady_clock::now()) {
                leaf->completed = true;
                leaf->result.reason = WakeReason::Timeout;
            } else {
                fiber.owner->timers.push(TimerEntry{leaf->deadline, fiber.owner->nextTimerSequence++, actor,
                    fiber.id, registration->token, leaf->leafIndex});
                queueCv.notify_all();
            }
        }
    }

    evaluate(*registration->root);
    if (registration->root->completed) {
        registration->active = false;
        return registration->root->result;
    }
    fiber.wait = std::move(registration);
    return std::nullopt;
}

void ActorSystem::Impl::triggerWait(
    const std::shared_ptr<ActorControl>& actor,
    const LeafRef& leaf,
    WaitResult result) {
    const auto found = actor->fibers.find(leaf.fiberId);
    if (found == actor->fibers.end()) return;
    Fiber& fiber = *found->second;
    if (!fiber.wait || !fiber.wait->active || fiber.wait->token != leaf.token ||
        leaf.leafIndex >= fiber.wait->leaves.size()) return;
    RuntimeWaitNode* node = fiber.wait->leaves[leaf.leafIndex];
    if (node->completed) return;
    node->completed = true;
    node->result = std::move(result);
    if (!evaluate(*fiber.wait->root)) return;

    fiber.wait->active = false;
    fiber.pendingWait = fiber.wait->root->result;
    fiber.state = FiberState::Ready;
    enqueueLocal(*fiber.owner, Job{JobKind::ResumeFiber, actor, fiber.id, leaf.token, 0});
}

void ActorSystem::Impl::notifyMessage(
    const std::shared_ptr<ActorControl>& actor,
    const MessagePtr& message) {
    const MessageKey key{message->typeId, message->correlationId};
    const auto found = actor->messageWaiters.find(key);
    if (found == actor->messageWaiters.end()) return;
    const auto snapshot = found->second;
    for (const auto& leaf : snapshot) {
        WaitResult result;
        result.reason = WakeReason::Message;
        result.value = message;
        triggerWait(actor, leaf, std::move(result));
    }
    auto& refs = found->second;
    refs.erase(std::remove_if(refs.begin(), refs.end(), [&](const LeafRef& leaf) {
        const auto fiber = actor->fibers.find(leaf.fiberId);
        return fiber == actor->fibers.end() || !fiber->second->wait ||
            !fiber->second->wait->active || fiber->second->wait->token != leaf.token;
    }), refs.end());
    if (refs.empty()) actor->messageWaiters.erase(found);
}

namespace {

std::atomic<std::uint64_t> nextSystemGeneration{1};

// 保护单例外壳的 start/stop/instance 状态。
std::mutex& systemStateMutex() {
    static std::mutex mutex;
    return mutex;
}

// 串行化 stop()。它不能是 systemStateMutex：teardown 会 join worker，
// 而 worker 上运行的业务代码随时可能调用 instance()/isRunning()/stats()，
// 持状态锁等待 join 会直接死锁。
std::mutex& systemStopMutex() {
    static std::mutex mutex;
    return mutex;
}

// 进程退出兜底：main 忘记 stop() 时也保证 join 所有 worker。
void stopAtExit() noexcept {
    try {
        ActorSystem::stop();
    } catch (...) {
        // 退出路径不允许抛异常。
    }
}

} // namespace

// 单例外壳：整个进程只分配一次，且永不析构。
// stop() 只释放 impl_，外壳地址保持不变，所以旧的 ActorRef 不会变成悬垂
// 指针——对已经停止的系统调用只会安全地抛异常。
ActorSystem& ActorSystem::shell() {
    static ActorSystem& instance = *new ActorSystem();
    return instance;
}

ActorSystem& ActorSystem::start() { return start(Config{}); }

ActorSystem& ActorSystem::start(Config config) {
    std::lock_guard<std::mutex> lock(systemStateMutex());
    ActorSystem& system = shell();
    if (system.impl_) {
        throw std::logic_error("an ActorSystem is already running in this process");
    }
    static const bool exitHookRegistered = [] {
        std::atexit(&stopAtExit);
        return true;
    }();
    (void)exitHookRegistered;
    // 配置非法时 Impl 构造抛出，impl_ 保持为空，系统仍处于“未运行”状态。
    auto impl = std::make_unique<Impl>(system, config);
    impl->generation = nextSystemGeneration.fetch_add(1, std::memory_order_relaxed);
    system.impl_ = std::move(impl);
    return system;
}

void ActorSystem::stop() {
    if (currentWorker != nullptr) {
        throw std::logic_error("ActorSystem::stop must not be called from a runtime worker thread");
    }
    std::lock_guard<std::mutex> stopLock(systemStopMutex());
    Impl* impl = nullptr;
    {
        std::lock_guard<std::mutex> lock(systemStateMutex());
        ActorSystem& system = shell();
        if (!system.impl_) return;
        impl = system.impl_.get();
    }
    // 状态锁在此处是空闲的：停止期间 worker 上的业务代码仍可安全地取用全局实例，
    // 否则 teardown 等待 join 时会被 worker 卡住，形成死锁。
    impl->teardown();
    std::lock_guard<std::mutex> lock(systemStateMutex());
    shell().impl_.reset();
}

bool ActorSystem::isRunning() {
    std::lock_guard<std::mutex> lock(systemStateMutex());
    return shell().impl_ != nullptr;
}

ActorSystem& ActorSystem::instance() {
    std::lock_guard<std::mutex> lock(systemStateMutex());
    ActorSystem& system = shell();
    if (!system.impl_) {
        throw std::logic_error("ActorSystem is not running; call ActorSystem::start() from main");
    }
    return system;
}

ActorRef ActorSystem::registerActor(std::unique_ptr<Actor> actor) {
    if (!impl_) throw std::runtime_error("actor system is not running");
    if (!actor) throw std::invalid_argument("cannot register a null actor");
    if (!impl_->accepting.load(std::memory_order_acquire)) {
        throw std::runtime_error("actor system is shutting down");
    }
    if (actor->impl_->system != this || actor->impl_->generation != impl_->generation ||
        actor->impl_->id != 0) {
        throw std::invalid_argument(
            "actor belongs to another system or generation, or is already registered");
    }
    const ActorId id = impl_->nextActorId.fetch_add(1, std::memory_order_relaxed);
    auto control = std::make_shared<ActorControl>();
    control->system = impl_.get();
    actor->impl_->id = id;
    actor->impl_->control = control;
    control->actor = std::move(actor);
    {
        std::lock_guard<std::mutex> lock(impl_->registryMutex);
        impl_->registry.emplace(id, control);
    }
    return ActorRef(this, id);
}

ActorRef ActorSystem::ref(ActorId id) noexcept { return ActorRef(this, id); }

MessageId ActorSystem::send(
    ActorId recipient,
    TypeId typeId,
    std::any payload,
    MessageId correlationId) {
    if (!impl_) throw std::runtime_error("actor system is not running");
    if (!impl_->accepting.load(std::memory_order_acquire)) {
        throw std::runtime_error("actor system is shutting down");
    }
    std::shared_ptr<ActorControl> control;
    {
        std::lock_guard<std::mutex> lock(impl_->registryMutex);
        const auto found = impl_->registry.find(recipient);
        if (found == impl_->registry.end()) throw std::invalid_argument("recipient actor does not exist");
        control = found->second;
    }
    const MessageId id = impl_->nextMessageId.fetch_add(1, std::memory_order_relaxed);
    const ActorId sender = currentActor == nullptr ? 0 : currentActor->actor->id();
    auto message = std::make_shared<const Message>(Message{
        typeId, id, correlationId, sender, std::move(payload)});
    {
        std::lock_guard<std::mutex> lock(control->mailboxMutex);
        control->mailbox.push_back(std::move(message));
    }
    impl_->enqueueGlobal(Job{JobKind::DispatchMessage, control, 0, 0, 0});
    return id;
}

RuntimeStats ActorSystem::stats() const noexcept {
    RuntimeStats result;
    result.asanFiberHooks = detail::asanFiberHooksAvailable();
    if (!impl_) return result;
    result.liveFibers = impl_->liveFibers.load(std::memory_order_acquire);
    result.liveStacks = impl_->liveStacks.load(std::memory_order_acquire);
    result.workerCount = impl_->workers.size();
    result.fiberSwitches = impl_->fiberSwitches.load(std::memory_order_relaxed);
    result.acceptingMessages = impl_->accepting.load(std::memory_order_acquire);
    return result;
}

void ActorSystem::Impl::teardown() {
    bool expected = true;
    if (!accepting.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return;

    std::vector<std::shared_ptr<ActorControl>> actors;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        for (const auto& item : registry) actors.push_back(item.second);
    }
    for (const auto& actor : actors) {
        enqueueGlobal(Job{JobKind::CancelActor, actor, 0, 0, 0});
    }

    const auto deadline = std::chrono::steady_clock::now() + config.shutdownTimeout;
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stateCv.wait_until(lock, deadline, [&] {
            return outstandingJobs.load(std::memory_order_acquire) == 0 &&
                liveFibers.load(std::memory_order_acquire) == 0;
        });
    }
    stopping.store(true, std::memory_order_release);
    queueCv.notify_all();
    for (auto& worker : workers) {
        if (worker->thread.joinable()) worker->thread.join();
    }
    std::lock_guard<std::mutex> lock(registryMutex);
    registry.clear();
}

} // namespace actor_runtime
