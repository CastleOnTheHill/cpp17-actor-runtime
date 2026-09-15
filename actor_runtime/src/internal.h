#pragma once

#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include "wait_internal.h"

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <ucontext.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace actor_runtime {

struct ActorControl;
struct Fiber;
struct Worker;
class IpcTransport;
class BlockingPool;

struct Actor::Impl {
    explicit Impl(ActorSystem& owner) : system(&owner) {}
    ActorSystem* system;
    ActorId id = 0;
    std::weak_ptr<ActorControl> control;
};

enum class FiberState { Ready, Running, Waiting, Done, Cancelled };

struct StackMapping final {
    explicit StackMapping(std::size_t requestedSize);
    ~StackMapping();
    StackMapping(const StackMapping&) = delete;
    StackMapping& operator=(const StackMapping&) = delete;

    void* mapping = nullptr;
    std::size_t mappingSize = 0;
    void* usable = nullptr;
    std::size_t usableSize = 0;
};

struct RuntimeWaitNode final {
    std::size_t timerIndex = static_cast<std::size_t>(-1);
    WaitKind kind = WaitKind::Timer;
    MessageKey messageKey{};
    FiberId childId = 0;
    std::chrono::steady_clock::time_point deadline{};
    std::size_t leafIndex = static_cast<std::size_t>(-1);
    bool completed = false;
    WaitResult result;
    std::vector<std::unique_ptr<RuntimeWaitNode>> children;
};

struct WaitRegistration final {
    std::uint64_t token = 0;
    bool active = true;
    std::unique_ptr<RuntimeWaitNode> root;
    std::vector<RuntimeWaitNode*> leaves;
};

struct CoroutineContext::Impl {
    Fiber* fiber = nullptr;
};

struct Fiber final {
    void* sanitizerFakeStack = nullptr;
    FiberId id = 0;
    ActorControl* actor = nullptr;
    Worker* owner = nullptr;
    ucontext_t context{};
    std::unique_ptr<StackMapping> stack;
    Actor::CoroutineBody body;
    CoroutineContext::Impl contextImpl;
    FiberState state = FiberState::Ready;
    bool started = false;
    bool finishedOnStack = false;
    bool cancellationRequested = false;
    bool cancelled = false;
    std::uint64_t nextWaitToken = 1;
    std::unique_ptr<WaitRegistration> wait;
    std::optional<WaitResult> pendingWait;
    std::any result;
    std::exception_ptr error;
    FiberId parentId = 0;
    ChildLifetime lifetime = ChildLifetime::DetachedFromParent;
    std::unordered_map<FiberId, ChildLifetime> children;
    std::unordered_set<TaskId> ownedTasks;
};

struct MessageKeyHash final {
    std::size_t operator()(const MessageKey& key) const noexcept {
        const auto first = std::hash<TypeId>{}(key.typeId);
        const auto second = std::hash<MessageId>{}(key.correlationId);
        return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }
};

struct MessageKeyEqual final {
    bool operator()(const MessageKey& left, const MessageKey& right) const noexcept {
        return left.typeId == right.typeId && left.correlationId == right.correlationId;
    }
};

struct LeafRef final {
    FiberId fiberId = 0;
    std::uint64_t token = 0;
    std::size_t leafIndex = 0;
};

struct Completion final {
    std::any result;
    std::exception_ptr error;
    bool cancelled = false;
    FiberId parentId = 0;
    bool done = true;
};

enum class DispatchState { Idle, Scheduled, Pending };

struct ActorControl final {
    std::string name;
    std::promise<void> stoppedPromise;
    std::shared_future<void> stoppedFuture = stoppedPromise.get_future().share();
    ActorSystem::Impl* system = nullptr;
    std::unique_ptr<Actor> actor;
    std::atomic<bool> executing{false};
    std::mutex mailboxMutex;
    std::deque<MessagePtr> mailbox;
    // Producers serialize cumulative admissions with mailboxMutex. The Actor
    // execution owner alone publishes cumulative returns (including discarded
    // queued messages). Unsigned differences give occupancy across wraparound;
    // outstanding bytes/count remain bounded. A stale return only rejects early.
    std::size_t admittedMessages = 0;
    std::size_t admittedBytes = 0;
    std::atomic<std::size_t> returnedMessages{0};
    std::atomic<std::size_t> returnedBytes{0};
    // Pending owns the same dispatch token, with additional messages in mailbox.
    // Producers serialize via mailboxMutex; an empty consumer retires by CAS.
    std::atomic<DispatchState> dispatchState{DispatchState::Idle};
    bool receiving = true; // mailboxMutex: admission is ordered against stop/shutdown.
    bool stopping = false;
    std::unordered_map<FiberId, std::unique_ptr<Fiber>> fibers;
    std::unordered_map<FiberId, Completion> completions;
    std::unordered_map<MessageKey, std::vector<LeafRef>, MessageKeyHash, MessageKeyEqual> messageWaiters;
    std::unordered_map<FiberId, std::vector<LeafRef>> childWaiters;
    std::unordered_map<TaskId, Completion> tasks;
    std::unordered_map<TaskId, std::vector<LeafRef>> taskWaiters;
};

enum class JobKind { DispatchMessage, ResumeFiber, CancelActor, TimerFire, TaskCompleted };

struct Job final {
    JobKind kind = JobKind::DispatchMessage;
    std::shared_ptr<ActorControl> actor;
    FiberId fiberId = 0;
    std::uint64_t token = 0;
    std::size_t leafIndex = 0;
    std::any value{};
    std::exception_ptr error{};
    MessagePtr message{}; // Dispatch token carries its next immutable message.
};

// Ordinary-ready queues never contain fiber/timer/completion jobs. Keep their
// ownership token compact; materialize the general Job only when selected.
struct ReadyDispatch final {
    std::shared_ptr<ActorControl> actor;
    MessagePtr message;
};

struct TimerEntry final {
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t sequence = 0;
    std::weak_ptr<ActorControl> actor;
    FiberId fiberId = 0;
    std::uint64_t token = 0;
    std::size_t leafIndex = 0;
    RuntimeWaitNode* leaf = nullptr; // Stable until its owner removes the registration.
};

struct TimerLater final {
    bool operator()(const TimerEntry& left, const TimerEntry& right) const noexcept {
        if (left.deadline != right.deadline) return left.deadline > right.deadline;
        return left.sequence > right.sequence;
    }
};

// Owner-worker-only indexed heap. Cancellation uses the registration's stable
// node, avoiding a second allocation/hash lookup for every logical timer.
class TimerQueue final {
public:
    bool empty() const noexcept { return entries_.empty(); }
    std::size_t size() const noexcept { return entries_.size(); }
    std::size_t capacity() const noexcept { return entries_.capacity(); }
    const TimerEntry& top() const noexcept { return entries_.front(); }
    void push(TimerEntry entry);
    void pop() noexcept;
    void cancel(RuntimeWaitNode& leaf) noexcept;
private:
    void erase(std::size_t index) noexcept;
    void exchange(std::size_t left, std::size_t right) noexcept;
    void up(std::size_t index) noexcept;
    void down(std::size_t index) noexcept;
    std::vector<TimerEntry> entries_;
};

struct Worker final {
    void* sanitizerFakeStack = nullptr;
    const void* schedulerStackBottom = nullptr;
    std::size_t schedulerStackSize = 0;
    unsigned nextQueue = 0;
    ActorSystem::Impl* system = nullptr;
    std::size_t index = 0;
    std::thread thread;
    ucontext_t schedulerContext{};
    // Protected by system->queueMutex, also used by the sleeping predicate.
    std::deque<Job> localQueue;
    std::condition_variable wakeCv;
    bool sleeping = false, wakeRequested = false; // system->queueMutex
    std::atomic<std::size_t> localCount{0}; // Hint for bounded idle checks, not the sleep predicate.
    // Ordinary dispatch can be stolen. Pinned fiber jobs stay in localQueue.
    std::mutex readyMutex;
    std::deque<ReadyDispatch> readyQueue;
    std::atomic<std::size_t> readyCount{0};
    std::atomic<std::uint64_t> readyEpoch{0}; // Single publisher; SC sleep handshake.
    unsigned readyPosts = 0;
    TimerQueue timers;
    std::uint64_t nextTimerSequence = 1;
};

struct alignas(64) RegistryReader final {
    std::mutex mutex; // Avoid false sharing between independently locked stripes.
};

struct ActorSystem::Impl final {
    explicit Impl(ActorSystem& facade, Config options);
    ~Impl();
    // Caller holds registryMutex. Mutations exclude every striped lookup, in
    // ascending lock order; lookups never acquire registryMutex while striped.
    std::array<std::unique_lock<std::mutex>, 64> lockRegistryReaders();

    void enqueueGlobal(Job job, bool newJob = true);
    void enqueueLocal(Worker& worker, Job job, bool newJob = true);
    Worker* claimWakeup(); // queueMutex must be held; reserves one specific sleeper.
    // newJob=false transfers an already-counted job; caller must not decrement it.
    void enqueueDispatch(std::shared_ptr<ActorControl> actor, MessagePtr message, bool newJob = true);
    bool takeReady(Worker& worker, std::optional<Job>& result, bool steal = false);
    SendStatus postMessage(std::shared_ptr<ActorControl> actor, std::shared_ptr<Message> message);
    void releaseMessage(ActorControl& actor, const Message& message);
    // Outstanding system bytes are admitted minus returned. Clamping keeps an
    // out-of-order snapshot of the two monotonic counters from wrapping.
    std::size_t outstandingMailboxBytes() const noexcept {
        const auto admitted = admittedMailboxBytes.load(std::memory_order_relaxed);
        const auto returned = returnedMailboxBytes.load(std::memory_order_relaxed);
        return admitted > returned ? admitted - returned : 0;
    }
    void discardMailbox(ActorControl& actor);
    void workerLoop(Worker& worker);
    void processJob(Worker& worker, Job& job);
    FiberId createFiber(
        const std::shared_ptr<ActorControl>& actor,
        Worker& owner,
        Actor::CoroutineBody body,
        FiberId parentId,
        ChildLifetime lifetime);
    void resumeFiber(Worker& worker, const std::shared_ptr<ActorControl>& actor, FiberId id);
    void finishFiber(const std::shared_ptr<ActorControl>& actor, FiberId id);
    void requestCancellation(const std::shared_ptr<ActorControl>& actor, Fiber& fiber);
    std::optional<WaitResult> installWait(Fiber& fiber, const WaitSpec& spec);
    void cleanupWait(Fiber& fiber, WaitRegistration& registration) noexcept;
    void triggerWait(const std::shared_ptr<ActorControl>& actor, const LeafRef& leaf, WaitResult result);
    void notifyMessage(const std::shared_ptr<ActorControl>& actor, const MessagePtr& message);
    void log(const char* event, ActorId actor = 0, FiberId fiber = 0) const;
    void receiveNamed(const std::string& name, ActorId incarnation, std::shared_ptr<Message> message);
    MessageId sendBytes(const ActorAddress& target, ActorId incarnation, TypeId type,
                        const void* data, std::size_t size, MessageId correlation, ActorId sender);
    TaskId submitBlocking(Fiber& fiber, std::function<std::any()> work);

    ActorSystem& facade;
    Config config;
    std::atomic<bool> accepting{true};
    std::atomic<bool> stopping{false};
    // ID/job writes must not invalidate read-mostly config/admission flags.
    alignas(64) std::atomic<ActorId> nextActorId{1};
    std::atomic<MessageId> nextMessageId{1};
    std::atomic<FiberId> nextFiberId{1};
    std::atomic<TaskId> nextTaskId{1};
    alignas(64) std::atomic<std::size_t> outstandingJobs{0};
    std::atomic<std::size_t> liveFibers{0};
    std::atomic<std::size_t> liveStacks{0};
    // Only producers write admittedMailboxBytes and only the consuming worker
    // writes returnedMailboxBytes, so the system budget does not ping-pong one
    // cache line between a sender and its Actor worker. Each counter keeps its
    // own line away from the scheduler/ID counters.
    alignas(64) std::atomic<std::size_t> admittedMailboxBytes{0};
    alignas(64) std::atomic<std::size_t> returnedMailboxBytes{0};

    mutable std::mutex registryMutex;
    std::array<RegistryReader, 64> registryReaders;
    std::unordered_map<ActorId, std::shared_ptr<ActorControl>> registry;
    std::unordered_map<std::string, ActorId> names;
    std::unordered_map<ActorId, std::shared_ptr<ActorControl>> retiring;
    std::mutex shutdownMutex;
    std::unique_ptr<IpcTransport> transport;
    std::unique_ptr<BlockingPool> blockingPool;

    std::mutex queueMutex;
    std::condition_variable stateCv;
    std::size_t nextWakeWorker = 0; // queueMutex
    std::atomic<std::size_t> sleepingWorkers{0};
    std::atomic<std::size_t> pendingWakeups{0}; // queueMutex owns changes; atomic reads are hints.
    std::deque<Job> globalQueue;
    std::atomic<std::size_t> globalCount{0}; // queueMutex publishes this non-owning readiness hint.
    std::vector<std::unique_ptr<Worker>> workers;
};

extern thread_local ActorControl* currentActor;
extern thread_local Worker* currentWorker;
extern thread_local Fiber* currentFiber;

} // namespace actor_runtime
