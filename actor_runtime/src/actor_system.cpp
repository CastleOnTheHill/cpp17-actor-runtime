#include "actor_runtime/actor_system.h"

#include "actor_runtime/actor.h"
#include "internal.h"
#include "sanitizer.h"
#include "ipc_transport.h"
#include "blocking_pool.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <iostream>
#include <limits>
#include <sys/random.h>

namespace actor_runtime {

thread_local ActorControl* currentActor = nullptr;
thread_local Worker* currentWorker = nullptr;
thread_local Fiber* currentFiber = nullptr;

namespace {

constexpr std::size_t noLeaf = static_cast<std::size_t>(-1);
// Bounded scheduler-idle yielding, never an Actor ownership spin loop.
constexpr unsigned idleYieldBudget = 256;

std::size_t saturatedAdd(std::size_t left, std::size_t right) noexcept {
    const auto max = std::numeric_limits<std::size_t>::max();
    return right > max - left ? max : left + right;
}

std::size_t defaultPayloadBytes(const std::any& payload) noexcept {
    if (!payload.has_value()) return 0;
    if (const auto* bytes = std::any_cast<Bytes>(&payload))
        return saturatedAdd(sizeof(Bytes), bytes->capacity());
    if (const auto* string = std::any_cast<std::string>(&payload))
        return saturatedAdd(sizeof(std::string), saturatedAdd(string->capacity(), 1));
    return sizeof(std::any);
}

void throwSendFailure(SendStatus status) {
    switch (status) {
    case SendStatus::Full: throw std::runtime_error("mailbox capacity exceeded");
    case SendStatus::MessageTooLarge: throw std::length_error("message exceeds mailbox size limit");
    case SendStatus::Stopped: throw std::runtime_error("actor system or recipient is stopping");
    case SendStatus::Accepted: return;
    }
}

void fiberTrampoline(std::uint32_t low, std::uint32_t high) {
    const auto id = static_cast<FiberId>(low) | (static_cast<FiberId>(high) << 32U);
    Fiber* fiber = currentFiber;
    if (fiber == nullptr || fiber->id != id) {
        std::terminate();
    }
    sanitizerFinish(fiber->sanitizerFakeStack, &currentWorker->schedulerStackBottom, &currentWorker->schedulerStackSize);
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
    sanitizerStart(nullptr, currentWorker->schedulerStackBottom, currentWorker->schedulerStackSize);
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
    const auto previous = system.outstandingJobs.fetch_sub(1, std::memory_order_acq_rel);
    // Only shutdown waits on this predicate. Serialize the final notification
    // with its wait mutex, without locking/notifying for every ordinary message.
    if (previous == 1 && !system.accepting.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(system.queueMutex);
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
    std::uint64_t seed = 0;
    if (::getrandom(&seed, sizeof(seed), 0) != sizeof(seed)) throw std::runtime_error("cannot allocate process incarnation");
    nextActorId.store(((seed & 0xffffffffULL) << 32U) | 1U);
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

std::array<std::unique_lock<std::mutex>, 64> ActorSystem::Impl::lockRegistryReaders() {
    std::array<std::unique_lock<std::mutex>, 64> locks;
    for (std::size_t i = 0; i < locks.size(); ++i)
        locks[i] = std::unique_lock<std::mutex>(registryReaders[i].mutex);
    return locks;
}

void ActorSystem::Impl::log(const char* event, ActorId actor, FiberId fiber) const {
    if (!config.diagnosticLogging) return;
    static std::mutex outputMutex;
    std::lock_guard<std::mutex> lock(outputMutex);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::clog << "[actor-runtime] t=" << ms << " pid=" << ::getpid() << " process=" << config.processName
              << " worker=" << (currentWorker ? static_cast<long>(currentWorker->index) : -1L)
              << " event=" << event << " actor=" << actor << " fiber=" << fiber << '\n';
}

void ActorSystem::Impl::enqueueGlobal(Job job, bool newJob) {
    Worker* notify;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        globalQueue.push_back(std::move(job));
        if (newJob) outstandingJobs.fetch_add(1, std::memory_order_relaxed);
        globalCount.store(globalQueue.size(), std::memory_order_release);
        notify = claimWakeup();
    }
    if (notify) notify->wakeCv.notify_one();
}

Worker* ActorSystem::Impl::claimWakeup() {
    const auto sleepers = sleepingWorkers.load();
    const auto pending = pendingWakeups.load(std::memory_order_relaxed);
    if (sleepers > pending && outstandingJobs.load(std::memory_order_acquire) > workers.size() - sleepers + pending) {
        for (std::size_t offset = 0; offset < workers.size(); ++offset) {
            auto& candidate = *workers[(nextWakeWorker + offset) % workers.size()];
            if (candidate.sleeping && !candidate.wakeRequested) {
                candidate.wakeRequested = true;
                pendingWakeups.store(pending + 1, std::memory_order_release);
                nextWakeWorker = (candidate.index + 1) % workers.size();
                return &candidate;
            }
        }
    }
    return nullptr;
}

void ActorSystem::Impl::enqueueLocal(Worker& worker, Job job, bool newJob) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        worker.localQueue.push_back(std::move(job));
        if (newJob) outstandingJobs.fetch_add(1, std::memory_order_relaxed);
        worker.localCount.store(worker.localQueue.size(), std::memory_order_release);
    }
    worker.wakeCv.notify_one();
}

SendStatus ActorSystem::Impl::postMessage(std::shared_ptr<ActorControl> actor, std::shared_ptr<Message> message) {
    auto bytes = saturatedAdd(MailboxEnvelopeBytes, config.payloadSizer ?
        config.payloadSizer(message->payload) : defaultPayloadBytes(message->payload));
    bytes = saturatedAdd(bytes, message->senderAddress.process.size());
    bytes = saturatedAdd(bytes, message->senderAddress.actor.size());
    auto* recipient = actor.get();
    std::lock_guard<std::mutex> lock(recipient->mailboxMutex);
    if (!recipient->receiving || !accepting.load(std::memory_order_acquire)) return SendStatus::Stopped;
    if (bytes == std::numeric_limits<std::size_t>::max() || bytes > config.maxMessageBytes)
        return SendStatus::MessageTooLarge;
    const auto queuedMessages = recipient->admittedMessages - recipient->returnedMessages.load(std::memory_order_acquire);
    const auto queuedBytes = recipient->admittedBytes - recipient->returnedBytes.load(std::memory_order_acquire);
    if (queuedMessages >= config.mailboxMaxMessages || bytes > config.mailboxMaxBytes ||
        queuedBytes > config.mailboxMaxBytes - bytes) return SendStatus::Full;
    // Admitted and returned are separate monotonic counters, so the returning
    // worker never steals the line this CAS needs. A stale returned snapshot
    // can only overstate usage: the hard budget is never exceeded, and the
    // retry path refreshes the snapshot before re-checking. This stays a CAS
    // loop on purpose. fetch_add plus rollback was measured and rejected twice:
    // its transient over-count is observable through stats().queuedMailboxBytes
    // and makes a concurrent producer reject early.
    auto returned = returnedMailboxBytes.load(std::memory_order_relaxed);
    auto admitted = admittedMailboxBytes.load(std::memory_order_relaxed);
    for (;;) {
        const auto outstanding = admitted > returned ? admitted - returned : 0;
        if (bytes > config.systemMailboxMaxBytes || outstanding > config.systemMailboxMaxBytes - bytes)
            return SendStatus::Full;
        if (admittedMailboxBytes.compare_exchange_weak(admitted, admitted + bytes,
                std::memory_order_relaxed, std::memory_order_relaxed))
            break;
        returned = returnedMailboxBytes.load(std::memory_order_relaxed);
    }
    ++recipient->admittedMessages;
    recipient->admittedBytes += bytes;
    message->estimatedMailboxBytes = bytes;
    const auto state = recipient->dispatchState.load(std::memory_order_acquire);
    bool queued = false;
    bool inherited = false;
    try {
    if (state == DispatchState::Idle) {
        recipient->dispatchState.store(DispatchState::Scheduled, std::memory_order_release);
        // Admission stays locked: stop/shutdown cannot remove the registered
        // owner before this scope exits. Transfer our reference to the job.
        enqueueDispatch(std::move(actor), std::move(message));
    } else {
        recipient->mailbox.push_back(std::move(message));
        queued = true;
        // Publish after queueing. If the consumer retired meanwhile, this
        // producer inherits the token and schedules the oldest queued message.
        // Pending can only be cleared while holding this same mailbox lock.
        // A producer observing it need not repeat the atomic exchange.
        if (state != DispatchState::Pending &&
            recipient->dispatchState.exchange(DispatchState::Pending, std::memory_order_acq_rel) == DispatchState::Idle) {
            inherited = true;
            // This was Scheduled with an empty deque: the only queued item is
            // ours. Keep it in place until publication succeeds for rollback.
            enqueueDispatch(std::move(actor), recipient->mailbox.front());
            recipient->mailbox.pop_front();
            recipient->dispatchState.store(recipient->mailbox.empty() ? DispatchState::Scheduled : DispatchState::Pending,
                std::memory_order_release);
        }
    }
    } catch (...) {
        if (queued) recipient->mailbox.pop_back();
        if (state == DispatchState::Idle || inherited)
            recipient->dispatchState.store(DispatchState::Idle, std::memory_order_release);
        --recipient->admittedMessages;
        recipient->admittedBytes -= bytes;
        admittedMailboxBytes.fetch_sub(bytes, std::memory_order_relaxed);
        throw;
    }
    return SendStatus::Accepted;
}

void ActorSystem::Impl::releaseMessage(ActorControl& actor, const Message& message) {
    // Actor execution is already owned; no producer writes these counters.
    actor.returnedMessages.store(actor.returnedMessages.load(std::memory_order_relaxed) + 1,
        std::memory_order_release);
    actor.returnedBytes.store(actor.returnedBytes.load(std::memory_order_relaxed) + message.estimatedMailboxBytes,
        std::memory_order_release);
    returnedMailboxBytes.fetch_add(message.estimatedMailboxBytes, std::memory_order_relaxed);
}

void ActorSystem::Impl::discardMailbox(ActorControl& actor) {
    std::deque<MessagePtr> discarded;
    {
        std::lock_guard<std::mutex> lock(actor.mailboxMutex);
        std::size_t bytes = 0;
        for (const auto& message : actor.mailbox) bytes += message->estimatedMailboxBytes;
        actor.returnedMessages.store(actor.returnedMessages.load(std::memory_order_relaxed) + actor.mailbox.size(),
            std::memory_order_release);
        actor.returnedBytes.store(actor.returnedBytes.load(std::memory_order_relaxed) + bytes,
            std::memory_order_release);
        returnedMailboxBytes.fetch_add(bytes, std::memory_order_relaxed);
        discarded.swap(actor.mailbox);
    }
    // User payload destructors run without the admission mutex.
}

void ActorSystem::Impl::enqueueDispatch(std::shared_ptr<ActorControl> actor, MessagePtr message, bool newJob) {
    Worker* worker = currentWorker;
    if (!worker || worker->system != this) {
        enqueueGlobal(Job{JobKind::DispatchMessage, std::move(actor), 0, 0, 0, {}, {}, std::move(message)}, newJob);
        return;
    }
    // Migration cadence is separate from the eight-dispatch fairness check.
    // Each job handles one message; this is not a mailbox-draining batch.
    // Only this job is directed; the Actor has no permanent worker owner.
    if (workers.size() > 1 && ++worker->readyPosts == 64) {
        worker->readyPosts = 0;
        enqueueLocal(*workers[(worker->index + 1) % workers.size()],
            Job{JobKind::DispatchMessage, std::move(actor), 0, 0, 0, {}, {}, std::move(message)}, newJob);
        return;
    }
    if (workers.size() == 1) {
        // Only this thread can access its ready queue; external sends still use
        // globalQueue, and remote completions use the synchronized pinned queue.
        worker->readyQueue.push_back(ReadyDispatch{std::move(actor), std::move(message)});
        if (newJob) outstandingJobs.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(worker->readyMutex);
        worker->readyQueue.push_back(ReadyDispatch{std::move(actor), std::move(message)});
        if (newJob) outstandingJobs.fetch_add(1, std::memory_order_relaxed);
        worker->readyCount.store(worker->readyQueue.size(), std::memory_order_release);
    }
    // Even one job must wake a peer: its owner may still be in a long handler.
    // SC publication precedes the sleeper check; the sleeper's SC registration
    // precedes its epoch check. Either it sees the new epoch or we see a sleeper.
    // Taking the wait mutex in the latter case closes the predicate-to-wait gap.
    worker->readyEpoch.fetch_add(1);
    const auto sleepers = sleepingWorkers.load();
    // Running jobs are included in outstandingJobs. If all awake workers are
    // occupied and another job is queued, wake a peer. Otherwise an already
    // awake scanner can take it; its epoch handshake prevents parking past it.
    // This avoids waking all workers for a chain with only two runnable Actors.
    if (sleepers && outstandingJobs.load(std::memory_order_acquire) >
        workers.size() - sleepers + pendingWakeups.load(std::memory_order_acquire)) {
        Worker* notify;
        { std::lock_guard<std::mutex> lock(queueMutex); notify = claimWakeup(); }
        if (notify) {
            notify->wakeCv.notify_one();
        }
    }
}

bool ActorSystem::Impl::takeReady(Worker& worker, std::optional<Job>& result, bool steal) {
    if (workers.size() == 1) {
        if (worker.readyQueue.empty()) return false;
        result.emplace();
        result->actor = std::move(worker.readyQueue.front().actor);
        result->message = std::move(worker.readyQueue.front().message);
        worker.readyQueue.pop_front();
        return true;
    }
    if (!worker.readyCount.load(std::memory_order_acquire)) return false;
    std::unique_lock<std::mutex> lock(worker.readyMutex, std::defer_lock);
    if (steal) { if (!lock.try_lock()) return false; } else lock.lock();
    if (worker.readyQueue.empty()) return false;
    // Owners and thieves both take oldest-ready work. New continuations do not
    // repeatedly overtake already-ready independent Actors.
    result.emplace();
    result->actor = std::move(worker.readyQueue.front().actor);
    result->message = std::move(worker.readyQueue.front().message);
    worker.readyQueue.pop_front();
    worker.readyCount.store(worker.readyQueue.size(), std::memory_order_release);
    return true;
}

void ActorSystem::Impl::workerLoop(Worker& worker) {
    currentWorker = &worker;
    unsigned localRuns = 0;
    unsigned idlePasses = 0;
    // Snapshot each publisher before scanning its queue. A publication between
    // that snapshot and sleep is detected by the final SC predicate; otherwise
    // the publisher observes our SC sleeper registration and requests a wake.
    // Separate epochs avoid a global atomic RMW on every private enqueue.
    std::vector<std::uint64_t> observedEpochs(workers.size());
    const auto readyChanged = [&] {
        for (std::size_t i = 0; i < workers.size(); ++i)
            if (workers[i]->readyEpoch.load() != observedEpochs[i]) return true;
        return false;
    };
    for (;;) {
        std::optional<Job> selected;
        // At most eight local dispatches before checking pinned work, global
        // traffic and timers. Every dispatch still handles exactly one message.
        if (localRuns < 8 && takeReady(worker, selected)) {
            idlePasses = 0;
            ++localRuns;
            processJob(worker, *selected);
            continue;
        }
        localRuns = 0;
        // Check other categories every budget even when ordinary work remains.
        // Their synchronized counts let an empty check avoid the global mutex.
        // New traffic is checked again at the next local budget; category
        // round-robin remains unchanged. Parking scans locked queues/epochs.
        if (!globalCount.load(std::memory_order_acquire) &&
            !worker.localCount.load(std::memory_order_acquire) &&
            (worker.timers.empty() || worker.timers.top().deadline > std::chrono::steady_clock::now()) &&
            takeReady(worker, selected)) {
            idlePasses = 0;
            localRuns = 1;
            processJob(worker, *selected);
            continue;
        }
        // Empty backoff checks must not themselves contend on queueMutex.
        // These hints only defer a check; the final sleep still uses the locked
        // queues and epoch handshake, so stale hints cannot lose notifications.
        if (idlePasses > 0 && idlePasses < idleYieldBudget && !globalCount.load(std::memory_order_acquire) &&
            !worker.localCount.load(std::memory_order_acquire) &&
            (worker.timers.empty() || worker.timers.top().deadline > std::chrono::steady_clock::now())) {
            bool ready = false;
            for (const auto& candidate : workers)
                if (candidate->readyCount.load(std::memory_order_acquire)) { ready = true; break; }
            if (!ready) {
                ++idlePasses;
                std::this_thread::yield();
                continue;
            }
        }
        std::unique_lock<std::mutex> lock(queueMutex);
        for (std::size_t i = 0; i < workers.size(); ++i)
            observedEpochs[i] = workers[i]->readyEpoch.load();
        for (unsigned attempt = 0; attempt < 3 && !selected; ++attempt) {
            const auto category = (worker.nextQueue + attempt) % 3;
            if (category == 0) {
                if (!worker.localQueue.empty()) {
                    selected = std::move(worker.localQueue.front());
                    worker.localQueue.pop_front();
                    worker.localCount.store(worker.localQueue.size(), std::memory_order_release);
                }
            } else if (category == 1) {
                if (!globalQueue.empty()) {
                    selected = std::move(globalQueue.front());
                    globalQueue.pop_front();
                    globalCount.store(globalQueue.size(), std::memory_order_release);
                }
            } else if (!stopping.load(std::memory_order_acquire) && !worker.timers.empty() &&
                       worker.timers.top().deadline <= std::chrono::steady_clock::now()) {
                auto timer = worker.timers.top();
                worker.timers.pop();
                if (auto actor = timer.actor.lock()) {
                    outstandingJobs.fetch_add(1, std::memory_order_relaxed);
                    selected = Job{JobKind::TimerFire, std::move(actor), timer.fiberId, timer.token, timer.leafIndex};
                }
            }
            if (selected) { worker.nextQueue = (category + 1) % 3; break; }
        }
        if (selected) {
            idlePasses = 0;
            lock.unlock();
            processJob(worker, *selected);
            continue;
        }

        lock.unlock();
        if (!takeReady(worker, selected)) {
            for (std::size_t offset = 1; offset < workers.size() && !selected; ++offset)
                takeReady(*workers[(worker.index + offset) % workers.size()], selected, true);
        }
        if (selected) { idlePasses = 0; processJob(worker, *selected); continue; }
        lock.lock();
        if (!globalQueue.empty() || !worker.localQueue.empty()) continue;
        if (stopping.load(std::memory_order_acquire)) break;
        // Brief, bounded idle backoff avoids parking between tightly coupled
        // messages. This never retries Actor ownership; busy Actor jobs retain
        // their fair requeue/sleep path. A genuinely idle worker still parks.
        if (workers.size() > 1 && idlePasses < idleYieldBudget) {
            ++idlePasses;
            lock.unlock();
            std::this_thread::yield();
            continue;
        }
        worker.sleeping = true;
        sleepingWorkers.fetch_add(1);
        if (worker.timers.empty()) {
            worker.wakeCv.wait(lock, [&] {
                return stopping.load(std::memory_order_acquire) || !globalQueue.empty() || !worker.localQueue.empty() ||
                    readyChanged() || worker.wakeRequested;
            });
        } else {
            worker.wakeCv.wait_until(lock, worker.timers.top().deadline, [&] {
                return stopping.load(std::memory_order_acquire) || !globalQueue.empty() || !worker.localQueue.empty() ||
                    readyChanged() || worker.wakeRequested;
            });
        }
        // Each notification belongs to a specific worker and is acknowledged
        // even if another worker already consumed the originally published job.
        if (worker.wakeRequested) {
            worker.wakeRequested = false;
            pendingWakeups.fetch_sub(1, std::memory_order_release);
        }
        worker.sleeping = false;
        sleepingWorkers.fetch_sub(1);
    }
    currentWorker = nullptr;
}

void ActorSystem::Impl::processJob(Worker& worker, Job& job) {
    const bool needsOwnership = workers.size() > 1;
    bool idle = false;
    if (needsOwnership && !job.actor->executing.compare_exchange_strong(idle, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        if (job.kind == JobKind::ResumeFiber || job.kind == JobKind::TimerFire) {
            enqueueLocal(worker, std::move(job));
        } else {
            enqueueGlobal(std::move(job));
        }
        decrementJobs(*this);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        return;
    }

    // One nonblocking acquire attempt; failed jobs are fairly requeued above,
    // never spun on. Release/acquire transfers all Actor state to its next worker.
    struct ExecutionGuard {
        std::atomic<bool>* executing;
        void unlock() noexcept {
            if (executing) { executing->store(false, std::memory_order_release); executing = nullptr; }
        }
        ~ExecutionGuard() { unlock(); }
    } actorLock{needsOwnership ? &job.actor->executing : nullptr};

    currentActor = job.actor.get();
    // An unsuccessful Actor ownership attempt above must not return capacity.
    // Charge ends before notification/handler; retained payloads are not queued.
    if (job.kind == JobKind::DispatchMessage && job.message)
        releaseMessage(*job.actor, *job.message);
    if (!job.actor->actor) {
        currentActor = nullptr;
        if (job.kind == JobKind::DispatchMessage) {
            std::lock_guard<std::mutex> lock(job.actor->mailboxMutex);
            job.actor->dispatchState.store(DispatchState::Idle, std::memory_order_release);
        }
        actorLock.unlock();
        decrementJobs(*this);
        return;
    }
    try {
        switch (job.kind) {
        case JobKind::TaskCompleted: {
            auto found = job.actor->tasks.find(job.fiberId);
            if (found == job.actor->tasks.end()) { log("blocking.late_result"); break; }
            found->second.result = std::move(job.value);
            found->second.error = job.error;
            found->second.done = true;
            auto waiting = job.actor->taskWaiters.find(job.fiberId);
            if (waiting != job.actor->taskWaiters.end()) {
                auto snapshot = waiting->second;
                job.actor->taskWaiters.erase(waiting);
                for (const auto& ref : snapshot) {
                    WaitResult result;
                    result.reason = job.error ? WakeReason::Error : WakeReason::TaskCompleted;
                    result.value = found->second.result;
                    result.error = job.error;
                    triggerWait(job.actor, ref, std::move(result));
                }
            }
            log("blocking.completed", job.actor->actor->id(), job.fiberId);
            break;
        }
        case JobKind::DispatchMessage: {
            MessagePtr message = std::move(job.message);
            if (message) {
                // The wait index is Actor-owned. With no registrations there is
                // no pre-handler snapshot to notify; avoid hashing/calling the
                // notification path for ordinary event-only Actors.
                if (!job.actor->messageWaiters.empty()) notifyMessage(job.actor, message);
                try {
                    job.actor->actor->onMessage(std::move(message));
                } catch (...) {
                    log("handler.error", job.actor->actor->id());
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
        log("scheduler.error", job.actor->actor->id());
        // Runtime jobs must not terminate a worker.
    }
    currentFiber = nullptr;
    currentActor = nullptr;
    if (job.actor->stopping && job.actor->fibers.empty()) {
        const auto id = job.actor->actor->id();
        job.actor->actor.reset();
        job.actor->completions.clear();
        job.actor->messageWaiters.clear();
        job.actor->childWaiters.clear();
        job.actor->tasks.clear();
        job.actor->taskWaiters.clear();
        discardMailbox(*job.actor);
        job.actor->stoppedPromise.set_value();
        { std::lock_guard<std::mutex> lock(registryMutex); retiring.erase(id); }
        log("actor.stopped", id);
    }
    if (job.kind == JobKind::DispatchMessage) {
        const bool live = static_cast<bool>(job.actor->actor);
        // Release Actor execution before allowing a producer to inherit the
        // dispatch token. A single CAS retires an empty mailbox without locking.
        actorLock.unlock();
        auto expected = DispatchState::Scheduled;
        if (job.actor->dispatchState.load(std::memory_order_acquire) == DispatchState::Pending ||
            !job.actor->dispatchState.compare_exchange_strong(expected, DispatchState::Idle,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            // A producer published Pending before our CAS, so we retain the
            // token. If it published after a successful CAS, it schedules instead.
            MessagePtr next;
            bool reschedule = false;
            {
                std::lock_guard<std::mutex> mailboxLock(job.actor->mailboxMutex);
                if (live && !job.actor->mailbox.empty()) {
                    next = std::move(job.actor->mailbox.front());
                    job.actor->mailbox.pop_front();
                    job.actor->dispatchState.store(job.actor->mailbox.empty() ? DispatchState::Scheduled : DispatchState::Pending,
                        std::memory_order_release);
                    reschedule = true;
                } else {
                    job.actor->dispatchState.store(DispatchState::Idle, std::memory_order_release);
                }
            }
            // State already owns the next token. The current outstanding job
            // protects shutdown until publication; no mailbox lock crosses the
            // scheduling call. A single worker can transfer its outstanding-job
            // credit. Multiworker publication keeps the old/new overlap because
            // wake-demand accounting includes the publishing worker until return.
            if (reschedule) {
                const bool transferCredit = workers.size() == 1;
                enqueueDispatch(std::move(job.actor), std::move(next), !transferCredit);
                if (transferCredit) return;
            }
        }
    } else {
        actorLock.unlock();
    }
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
    sanitizerStart(&worker.sanitizerFakeStack, fiber.stack->usable, fiber.stack->usableSize);
    if (::swapcontext(&worker.schedulerContext, &fiber.context) != 0) {
        currentFiber = nullptr;
        throw std::system_error(errno, std::generic_category(), "swapcontext to fiber");
    }
    sanitizerFinish(worker.sanitizerFakeStack);
    currentFiber = nullptr;
    const auto again = actor->fibers.find(id);
    if (again != actor->fibers.end() && again->second->finishedOnStack) finishFiber(actor, id);
}

void ActorSystem::Impl::finishFiber(const std::shared_ptr<ActorControl>& actor, FiberId id) {
    const auto found = actor->fibers.find(id);
    if (found == actor->fibers.end()) return;
    Fiber& fiber = *found->second;
    if (fiber.wait) cleanupWait(fiber, *fiber.wait);
    fiber.wait.reset();

    for (const auto task : fiber.ownedTasks) {
        actor->taskWaiters.erase(task);
        actor->tasks.erase(task);
    }

    Completion completion{std::move(fiber.result), fiber.error, fiber.cancelled, fiber.parentId};
    // Only a live parent can late-join this result. Root/ownerless results are
    // delivered below and then released, never kept as Actor-wide history.
    if (fiber.parentId && actor->fibers.find(fiber.parentId) != actor->fibers.end())
        actor->completions[id] = completion;

    std::vector<std::pair<FiberId, ChildLifetime>> children;
    children.reserve(fiber.children.size());
    for (const auto& item : fiber.children) children.push_back(item);
    for (const auto& item : children) {
        const auto child = actor->fibers.find(item.first);
        if (child == actor->fibers.end()) {
            actor->completions.erase(item.first);
            actor->childWaiters.erase(item.first);
            continue;
        }
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
    log("fiber.cancel", actor->actor->id(), fiber.id);
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

    // Covers partially installed/invalid specs and immediate Any winners. Normal
    // suspended registrations are cleaned on their owner before await returns.
    struct Rollback {
        Impl& system; Fiber& fiber; std::unique_ptr<WaitRegistration>& registration;
        ~Rollback() { if (registration) system.cleanupWait(fiber, *registration); }
    } rollback{*this, fiber, registration};

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
        } else if (leaf->kind == WaitKind::Task) {
            auto found = actor->tasks.find(leaf->childId);
            if (found == actor->tasks.end() || found->second.parentId != fiber.id)
                throw std::invalid_argument("unknown task or foreign task owner");
            if (found->second.done) {
                leaf->completed = true;
                leaf->result.reason = found->second.error ? WakeReason::Error : WakeReason::TaskCompleted;
                leaf->result.value = found->second.result;
                leaf->result.error = found->second.error;
            } else actor->taskWaiters[leaf->childId].push_back(reference);
        } else if (leaf->kind == WaitKind::Timer) {
            if (leaf->deadline <= std::chrono::steady_clock::now()) {
                leaf->completed = true;
                leaf->result.reason = WakeReason::Timeout;
            } else {
                fiber.owner->timers.push(TimerEntry{leaf->deadline, fiber.owner->nextTimerSequence++, actor,
                    fiber.id, registration->token, leaf->leafIndex, leaf});
                fiber.owner->wakeCv.notify_one();
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

void ActorSystem::Impl::cleanupWait(Fiber& fiber, WaitRegistration& registration) noexcept {
    // triggerWait/cancel may run on another worker holding Actor execution.
    // Only the pinned owner resumes/finishes the fiber and mutates its timer heap.
    const auto remove = [&](auto& index, const auto& key) {
        const auto found = index.find(key);
        if (found == index.end()) return;
        auto& refs = found->second;
        refs.erase(std::remove_if(refs.begin(), refs.end(), [&](const LeafRef& ref) {
            return ref.fiberId == fiber.id && ref.token == registration.token;
        }), refs.end());
        if (refs.empty()) index.erase(found);
    };
    for (auto* leaf : registration.leaves) {
        switch (leaf->kind) {
        case WaitKind::Message: remove(fiber.actor->messageWaiters, leaf->messageKey); break;
        case WaitKind::Child: remove(fiber.actor->childWaiters, leaf->childId); break;
        case WaitKind::Task: remove(fiber.actor->taskWaiters, leaf->childId); break;
        case WaitKind::Timer: fiber.owner->timers.cancel(*leaf); break;
        default: break;
        }
    }
    registration.active = false;
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

ActorSystem::ActorSystem() : ActorSystem(Config{}) {}
ActorSystem::ActorSystem(Config config) : impl_(std::make_unique<Impl>(*this, config)) {
    try {
        impl_->blockingPool = std::make_unique<BlockingPool>(config.blockingThreads, config.blockingQueueCapacity);
        if (!config.processName.empty()) {
            impl_->transport = std::make_unique<IpcTransport>(config.processName,
                [this](const std::string& name, ActorId id, std::shared_ptr<Message> message) {
                    impl_->receiveNamed(name, id, std::move(message));
                }, [this](const char* event) { impl_->log(event); });
        }
    } catch (...) { shutdown(); throw; }
}
ActorSystem::~ActorSystem() { shutdown(); }

ActorRef ActorSystem::registerActor(std::unique_ptr<Actor> actor) {
    return registerActor({}, std::move(actor));
}

ActorRef ActorSystem::registerActor(std::string name, std::unique_ptr<Actor> actor) {
    if (!name.empty()) validateName(name);
    if (!actor) throw std::invalid_argument("cannot register a null actor");
    std::lock_guard<std::mutex> lock(impl_->registryMutex);
    auto readers = impl_->lockRegistryReaders();
    if (!impl_->accepting.load(std::memory_order_acquire)) {
        throw std::runtime_error("actor system is shutting down");
    }
    if (actor->impl_->system != this || actor->impl_->id != 0) {
        throw std::invalid_argument("actor belongs to another system or is already registered");
    }
    const ActorId id = impl_->nextActorId.fetch_add(1, std::memory_order_relaxed);
    if (!name.empty() && impl_->names.count(name)) throw std::invalid_argument("actor name already registered");
    auto control = std::make_shared<ActorControl>();
    control->name = name;
    control->system = impl_.get();
    actor->impl_->id = id;
    actor->impl_->control = control;
    control->actor = std::move(actor);
    impl_->registry.emplace(id, control);
    if (!name.empty()) impl_->names.emplace(name, id);
    impl_->log("actor.register", id);
    return ActorRef(this, id);
}

ActorRef ActorSystem::ref(ActorId id) noexcept { return ActorRef(this, id); }

ActorRef ActorSystem::findActor(const std::string& name) {
    std::lock_guard<std::mutex> lock(impl_->registryMutex);
    auto found = impl_->names.find(name);
    if (found == impl_->names.end()) throw std::invalid_argument("actor name not registered");
    return ref(found->second);
}

std::shared_future<void> ActorSystem::stopActor(ActorId id) {
    std::lock_guard<std::mutex> lock(impl_->registryMutex);
    auto pending = impl_->retiring.find(id);
    if (pending != impl_->retiring.end()) return pending->second->stoppedFuture;
    auto readers = impl_->lockRegistryReaders();
    auto found = impl_->registry.find(id);
    if (found == impl_->registry.end()) throw std::invalid_argument("actor not registered");
    auto actor = found->second;
    std::lock_guard<std::mutex> admission(actor->mailboxMutex);
    actor->receiving = false;
    impl_->names.erase(actor->name);
    impl_->retiring.emplace(id, actor);
    impl_->registry.erase(found);
    impl_->enqueueGlobal(Job{JobKind::CancelActor, actor, 0, 0, 0});
    impl_->log("actor.stop_requested", id);
    return actor->stoppedFuture;
}

MessageId ActorSystem::send(
    ActorId recipient,
    TypeId typeId,
    std::any payload,
    MessageId correlationId) {
    return sendOwned(recipient, typeId, std::move(payload), correlationId);
}

MessageId ActorSystem::sendOwned(
    ActorId recipient,
    TypeId typeId,
    std::any&& payload,
    MessageId correlationId) {
    auto result = trySendOwned(recipient, typeId, std::move(payload), correlationId);
    if (result.status == SendStatus::Stopped && impl_->accepting.load(std::memory_order_acquire))
        throw std::invalid_argument("recipient actor does not exist or is stopping");
    throwSendFailure(result.status);
    return result.id;
}

SendResult ActorSystem::trySend(ActorId recipient, TypeId typeId, std::any payload, MessageId correlationId) {
    return trySendOwned(recipient, typeId, std::move(payload), correlationId);
}

SendResult ActorSystem::trySendOwned(ActorId recipient, TypeId typeId, std::any&& payload, MessageId correlationId) {
    std::shared_ptr<ActorControl> control;
    {
        // The map is still the one authoritative dynamic registry. Independent
        // ID lookups share no single mutex; mutations lock all reader stripes.
        auto& stripe = impl_->registryReaders[recipient % impl_->registryReaders.size()];
        std::lock_guard<std::mutex> lock(stripe.mutex);
        if (!impl_->accepting.load(std::memory_order_acquire))
            return {SendStatus::Stopped, 0};
        const auto found = impl_->registry.find(recipient);
        if (found == impl_->registry.end()) return {SendStatus::Stopped, 0};
        control = found->second;
    }
    const MessageId id = impl_->nextMessageId.fetch_add(1, std::memory_order_relaxed);
    const ActorId sender = currentActor == nullptr || currentActor->system != impl_.get() ? 0 : currentActor->actor->id();
    // Populate the private allocation directly, then surrender the only mutable
    // handle before publication. Avoid moving any/strings via a temporary Message.
    auto message = std::make_shared<Message>();
    message->typeId = typeId;
    message->id = id;
    message->correlationId = correlationId;
    message->sender = sender;
    message->payload = std::move(payload);
    // Fresh Message strings are already empty. Populate only present metadata;
    // avoid a conditional-expression temporary when the sender has a name.
    if (!impl_->config.processName.empty()) message->senderAddress.process = impl_->config.processName;
    if (currentActor && !currentActor->name.empty()) message->senderAddress.actor = currentActor->name;
    const auto status = impl_->postMessage(std::move(control), std::move(message));
    return {status, status == SendStatus::Accepted ? id : 0};
}

RuntimeStats ActorSystem::stats() const noexcept {
    if (!impl_) return {};
    return RuntimeStats{
        impl_->liveFibers.load(std::memory_order_acquire),
        impl_->liveStacks.load(std::memory_order_acquire),
        impl_->workers.size(),
        impl_->accepting.load(std::memory_order_acquire),
        impl_->outstandingMailboxBytes()};
}

void ActorSystem::shutdown() {
    if (!impl_) return;
    if (currentWorker && currentWorker->system == impl_.get()) throw std::logic_error("shutdown cannot join its own worker");
    std::lock_guard<std::mutex> shutdownLock(impl_->shutdownMutex);
    bool expected = true;
    if (!impl_->accepting.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return;
    if (impl_->transport) impl_->transport->stop();

    std::vector<std::shared_ptr<ActorControl>> actors;
    {
        std::lock_guard<std::mutex> lock(impl_->registryMutex);
        for (const auto& item : impl_->registry) actors.push_back(item.second);
        for (const auto& item : impl_->retiring) actors.push_back(item.second);
        impl_->names.clear();
    }
    for (const auto& actor : actors) {
        std::lock_guard<std::mutex> admission(actor->mailboxMutex);
        actor->receiving = false;
        impl_->enqueueGlobal(Job{JobKind::CancelActor, actor, 0, 0, 0});
    }
    if (impl_->blockingPool) impl_->blockingPool->stop();

    const auto deadline = std::chrono::steady_clock::now() + impl_->config.shutdownTimeout;
    {
        std::unique_lock<std::mutex> lock(impl_->queueMutex);
        impl_->stateCv.wait_until(lock, deadline, [&] {
            return impl_->outstandingJobs.load(std::memory_order_acquire) == 0 &&
                impl_->liveFibers.load(std::memory_order_acquire) == 0;
        });
    }
    {
        std::lock_guard<std::mutex> lock(impl_->queueMutex);
        impl_->stopping.store(true, std::memory_order_release);
    }
    for (auto& worker : impl_->workers) worker->wakeCv.notify_one();
    for (auto& worker : impl_->workers) {
        if (worker->thread.joinable()) worker->thread.join();
    }
    std::lock_guard<std::mutex> lock(impl_->registryMutex);
    auto readers = impl_->lockRegistryReaders();
    impl_->registry.clear();
    impl_->retiring.clear();
    impl_->transport.reset();
}

void ActorSystem::Impl::receiveNamed(const std::string& name, ActorId incarnation, std::shared_ptr<Message> message) {
    std::lock_guard<std::mutex> lock(registryMutex);
    if (!accepting) return;
    auto found = names.find(name);
    if (found == names.end() || (incarnation && found->second != incarnation)) {
        log("ipc.dead_letter");
        return;
    }
    auto actor = registry.at(found->second);
    const auto status = postMessage(std::move(actor), std::move(message));
    if (status != SendStatus::Accepted) log("ipc.mailbox_rejected");
}

MessageId ActorSystem::Impl::sendBytes(const ActorAddress& target, ActorId incarnation, TypeId type,
                                    const void* data, std::size_t size, MessageId correlation, ActorId sender) {
    validateName(target.actor);
    if (size > MaxPayloadBytes) throw std::length_error("payload exceeds 60 KiB");
    if (!data && size) throw std::invalid_argument("null payload");
    Bytes payload(size);
    if (size) std::memcpy(payload.data(), data, size);
    std::lock_guard<std::mutex> lock(registryMutex);
    if (!accepting) throw std::runtime_error("actor system is shutting down");
    if (!sender && currentActor && currentActor->system == this) sender = currentActor->actor->id();
    ActorAddress source{config.processName, {}};
    if (sender) {
        auto found = registry.find(sender);
        if (found == registry.end()) throw std::invalid_argument("sender is not registered");
        source.actor = found->second->name;
    }
    const auto id = nextMessageId.fetch_add(1);
    auto message = std::make_shared<Message>(Message{type, id, correlation, sender, std::move(payload), source});
    if (target.process.empty() || target.process == config.processName) {
        auto named = names.find(target.actor);
        if (named == names.end() || (incarnation && named->second != incarnation))
            throw std::invalid_argument("target actor does not exist or was replaced");
        auto actor = registry.at(named->second);
        throwSendFailure(postMessage(std::move(actor), std::move(message)));
    } else {
        if (!transport) throw std::logic_error("IPC requires a configured processName");
        if (sender && source.actor.empty()) throw std::logic_error("IPC sender requires a registered actor name");
        transport->send(target, incarnation, *message);
    }
    return id;
}
MessageId ActorSystem::sendBytes(const ActorAddress& target, TypeId type, const void* data,
                               std::size_t size, MessageId correlation, ActorId sender) {
    return impl_->sendBytes(target, 0, type, data, size, correlation, sender);
}
MessageId ActorSystem::replyBytes(const Message& request, TypeId type, const void* data, std::size_t size) {
    if (!request.sender || request.senderAddress.actor.empty()) throw std::invalid_argument("request has no reply address");
    return impl_->sendBytes(request.senderAddress, request.sender, type, data, size, request.id, 0);
}

TaskId ActorSystem::Impl::submitBlocking(Fiber& fiber, std::function<std::any()> work) {
    if (!work) throw std::invalid_argument("empty blocking function");
    if (!accepting) throw std::runtime_error("system is shutting down");
    const auto id = nextTaskId.fetch_add(1);
    auto actor = fiber.actor->actor->impl_->control.lock();
    actor->tasks.emplace(id, Completion{{}, {}, false, fiber.id, false});
    std::weak_ptr<ActorControl> weakActor = actor;
    auto* owner = fiber.owner;
    try {
        fiber.ownedTasks.insert(id);
        blockingPool->submit([this, weakActor, owner, id, work = std::move(work)]() mutable {
            std::any result;
            std::exception_ptr error;
            try { result = work(); } catch (...) { error = std::current_exception(); }
            // Release captured input before publishing completion. The receiver
            // may run immediately after enqueueLocal, on another worker.
            work = {};
            if (auto target = weakActor.lock()) {
                enqueueLocal(*owner, Job{JobKind::TaskCompleted, target, id, 0, 0, std::move(result), error});
            }
        });
    } catch (...) { actor->tasks.erase(id); fiber.ownedTasks.erase(id); throw; }
    log("blocking.submitted", actor->actor->id(), id);
    return id;
}

} // namespace actor_runtime
