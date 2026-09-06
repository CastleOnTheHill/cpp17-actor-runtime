#pragma once

#include "actor_runtime/actor_ref.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <future>
#include <string>

namespace actor_runtime {

class Actor;

struct RuntimeStats {
    std::size_t liveFibers = 0;
    std::size_t liveStacks = 0;
    std::size_t workerCount = 0;
    bool acceptingMessages = false;
};

class ActorSystem final {
public:
    struct Impl;

    struct Config {
        std::size_t workerCount = 0;
        std::size_t fiberStackSize = 256U * 1024U;
        std::chrono::milliseconds shutdownTimeout{5000};
        std::string processName;
        bool diagnosticLogging = false;
        std::size_t blockingThreads = 2;
        std::size_t blockingQueueCapacity = 64;
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
    void shutdown();

private:
    friend class Actor;
    friend class ActorRef;
    friend class CoroutineContext;
    std::unique_ptr<Impl> impl_;
};

} // namespace actor_runtime
