#pragma once

#include "actor_runtime/message.h"

#include <any>

namespace actor_runtime {

class ActorSystem;

class ActorRef final {
public:
    ActorRef() = default;

    ActorId id() const noexcept;
    explicit operator bool() const noexcept;

    MessageId send(
        TypeId typeId,
        std::any payload,
        MessageId correlationId = NoCorrelation) const;

    // Never waits for capacity. Allocation/payload estimation may still throw.
    SendResult trySend(TypeId typeId, std::any payload,
                       MessageId correlationId = NoCorrelation) const;

private:
    friend class ActorSystem;
    ActorRef(ActorSystem* system, ActorId id) noexcept;

    ActorSystem* system_ = nullptr;
    ActorId id_ = 0;
};

} // namespace actor_runtime
