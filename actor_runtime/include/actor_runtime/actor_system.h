#pragma once

#include "actor_runtime/actor_ref.h"

#include <chrono>
#include <cstddef>
#include <memory>

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
    };

    ActorSystem();
    explicit ActorSystem(Config config);
    ~ActorSystem();

    ActorSystem(const ActorSystem&) = delete;
    ActorSystem& operator=(const ActorSystem&) = delete;

    ActorRef registerActor(std::unique_ptr<Actor> actor);
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
