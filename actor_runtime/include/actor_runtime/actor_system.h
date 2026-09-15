#pragma once

#include "actor_runtime/actor_ref.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <future>
#include <functional>
#include <string>

namespace actor_runtime {

class Actor;

struct RuntimeStats {
    std::size_t liveFibers = 0;
    std::size_t liveStacks = 0;
    std::size_t workerCount = 0;
    bool acceptingMessages = false;
    std::size_t queuedMailboxBytes = 0;
};

class ActorSystem final {
public:
    struct Impl;

    struct Config {
        // How the system-wide byte budget is enforced. Exact accounts every
        // message separately: the pool is never over-reserved and rejection is
        // exact, at the cost of one shared atomic per send. Reserve draws a
        // per-Actor chunk from the pool so senders touch the shared counter once
        // per chunk; it never exceeds the budget either, but can reject slightly
        // early while chunks are held and makes stats().queuedMailboxBytes an
        // upper bound instead of an exact figure.
        enum class MailboxBudget { Exact, Reserve };
        std::size_t workerCount = 0;
        std::size_t fiberStackSize = 256U * 1024U;
        std::chrono::milliseconds shutdownTimeout{5000};
        std::string processName;
        bool diagnosticLogging = false;
        std::size_t blockingThreads = 2;
        std::size_t blockingQueueCapacity = 64;
        // Queue only: includes the ready-dispatch message, excludes running
        // handlers, retained MessagePtr values and control jobs. Zero rejects all.
        std::size_t mailboxMaxMessages = 65536;
        std::size_t mailboxMaxBytes = 64U * 1024U * 1024U;
        std::size_t systemMailboxMaxBytes = 256U * 1024U * 1024U;
        MailboxBudget mailboxBudget = MailboxBudget::Exact;
        std::size_t reserveChunkBytes = 64U * 1024U;
        std::size_t maxMessageBytes = 16U * 1024U * 1024U;
        // Optional pure, thread-safe estimator, called outside admission locks.
        // Default: Bytes/string capacity; opaque any: sizeof(any). Deep custom
        // payloads need an estimator. Shared storage is charged per message.
        std::function<std::size_t(const std::any&)> payloadSizer;
    };

    ActorSystem();
    explicit ActorSystem(Config config);
    ~ActorSystem();

    ActorSystem(const ActorSystem&) = delete;
    ActorSystem& operator=(const ActorSystem&) = delete;

    ActorRef registerActor(std::unique_ptr<Actor> actor);
    ActorRef registerActor(std::string name, std::unique_ptr<Actor> actor);
    ActorRef findActor(const std::string& name);
    // 注销地址后异步取消；future 完成表示 Actor 的析构已经执行。
    std::shared_future<void> stopActor(ActorId id);
    // 复制无指针 C struct 的 bytes。sender=0 时使用当前 Actor（外部调用可不要求应答）。
    MessageId sendBytes(const ActorAddress& target, TypeId typeId, const void* data,
                        std::size_t size, MessageId correlation = 0, ActorId sender = 0);
    MessageId replyBytes(const Message& request, TypeId typeId, const void* data, std::size_t size);
    ActorRef ref(ActorId id) noexcept;
    MessageId send(
        ActorId recipient,
        TypeId typeId,
        std::any payload,
        MessageId correlationId = NoCorrelation);

    RuntimeStats stats() const noexcept;
    SendResult trySend(ActorId recipient, TypeId typeId, std::any payload,
                       MessageId correlationId = NoCorrelation);
    void shutdown();

private:
    friend class Actor;
    friend class ActorRef;
    friend class CoroutineContext;
    // Both public send entry points already own their by-value payload. Transfer
    // that ownership without constructing another type-erased intermediate.
    MessageId sendOwned(ActorId recipient, TypeId typeId, std::any&& payload,
                        MessageId correlationId);
    SendResult trySendOwned(ActorId recipient, TypeId typeId, std::any&& payload,
                            MessageId correlationId);
    std::unique_ptr<Impl> impl_;
};

} // namespace actor_runtime
