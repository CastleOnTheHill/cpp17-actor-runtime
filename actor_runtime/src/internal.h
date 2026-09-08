#pragma once

#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include "wait_internal.h"

#include <atomic>
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

struct ActorControl final {
    std::string name;
    std::promise<void> stoppedPromise;
    std::shared_future<void> stoppedFuture = stoppedPromise.get_future().share();
    ActorSystem::Impl* system = nullptr;
    std::unique_ptr<Actor> actor;
    std::mutex executionMutex;
    std::mutex mailboxMutex;
    std::deque<MessagePtr> mailbox;
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
};

struct TimerEntry final {
    std::chrono::steady_clock::time_point deadline;
    std::uint64_t sequence = 0;
    std::weak_ptr<ActorControl> actor;
    FiberId fiberId = 0;
    std::uint64_t token = 0;
    std::size_t leafIndex = 0;
};

struct TimerLater final {
    bool operator()(const TimerEntry& left, const TimerEntry& right) const noexcept {
        if (left.deadline != right.deadline) return left.deadline > right.deadline;
        return left.sequence > right.sequence;
    }
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
    std::priority_queue<TimerEntry, std::vector<TimerEntry>, TimerLater> timers;
    std::uint64_t nextTimerSequence = 1;
};

struct ActorSystem::Impl final {
    explicit Impl(ActorSystem& facade, Config options);
    ~Impl();

    void enqueueGlobal(Job job);
    void enqueueLocal(Worker& worker, Job job);
    void workerLoop(Worker& worker);
    void processJob(Worker& worker, Job job);
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
    void triggerWait(const std::shared_ptr<ActorControl>& actor, const LeafRef& leaf, WaitResult result);
    void notifyMessage(const std::shared_ptr<ActorControl>& actor, const MessagePtr& message);
    void log(const char* event, ActorId actor = 0, FiberId fiber = 0) const;
    void receiveNamed(const std::string& name, ActorId incarnation, MessagePtr message);
    MessageId sendBytes(const ActorAddress& target, ActorId incarnation, TypeId type,
                        const void* data, std::size_t size, MessageId correlation, ActorId sender);
    TaskId submitBlocking(Fiber& fiber, std::function<std::any()> work);

    ActorSystem& facade;
    Config config;
    std::atomic<bool> accepting{true};
    std::atomic<bool> stopping{false};
    std::atomic<ActorId> nextActorId{1};
    std::atomic<MessageId> nextMessageId{1};
    std::atomic<FiberId> nextFiberId{1};
    std::atomic<TaskId> nextTaskId{1};
    std::atomic<std::size_t> outstandingJobs{0};
    std::atomic<std::size_t> liveFibers{0};
    std::atomic<std::size_t> liveStacks{0};

    mutable std::mutex registryMutex;
    std::unordered_map<ActorId, std::shared_ptr<ActorControl>> registry;
    std::unordered_map<std::string, ActorId> names;
    std::unordered_map<ActorId, std::shared_ptr<ActorControl>> retiring;
    std::mutex shutdownMutex;
    std::unique_ptr<IpcTransport> transport;
    std::unique_ptr<BlockingPool> blockingPool;

    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::condition_variable stateCv;
    std::deque<Job> globalQueue;
    std::vector<std::unique_ptr<Worker>> workers;
};

extern thread_local ActorControl* currentActor;
extern thread_local Worker* currentWorker;
extern thread_local Fiber* currentFiber;

} // namespace actor_runtime
