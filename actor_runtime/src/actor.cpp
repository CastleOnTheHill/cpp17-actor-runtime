#include "actor_runtime/actor.h"

#include "internal.h"

#include <stdexcept>
#include <utility>

namespace actor_runtime {

Actor::Actor(ActorSystem& system) : impl_(std::make_unique<Impl>(system)) {}
Actor::~Actor() = default;

ActorId Actor::id() const noexcept { return impl_->id; }
ActorRef Actor::self() const noexcept { return impl_->system->ref(impl_->id); }

FiberId Actor::start(CoroutineBody body) {
    auto control = impl_->control.lock();
    if (!control || currentActor != control.get() || currentWorker == nullptr || currentFiber != nullptr) {
        throw std::logic_error("Actor::start is only valid while dispatching this actor");
    }
    return impl_->system->impl_->createFiber(
        control, *currentWorker, std::move(body), 0, ChildLifetime::DetachedFromParent);
}

void Actor::onCoroutineError(FiberId, std::exception_ptr) {}

} // namespace actor_runtime
