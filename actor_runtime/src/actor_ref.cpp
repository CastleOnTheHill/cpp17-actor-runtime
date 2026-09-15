#include "actor_runtime/actor_ref.h"

#include "actor_runtime/actor_system.h"

#include <stdexcept>
#include <utility>

namespace actor_runtime {

ActorRef::ActorRef(ActorSystem* system, ActorId id) noexcept : system_(system), id_(id) {}
ActorId ActorRef::id() const noexcept { return id_; }
ActorRef::operator bool() const noexcept { return system_ != nullptr && id_ != 0; }

MessageId ActorRef::send(TypeId typeId, std::any payload, MessageId correlationId) const {
    if (!*this) {
        throw std::invalid_argument("cannot send through an empty ActorRef");
    }
    return system_->sendOwned(id_, typeId, std::move(payload), correlationId);
}

SendResult ActorRef::trySend(TypeId typeId, std::any payload, MessageId correlationId) const {
    if (!*this) return {SendStatus::Stopped, 0};
    return system_->trySendOwned(id_, typeId, std::move(payload), correlationId);
}

} // namespace actor_runtime
