#include "actor_runtime/coroutine_context.h"

#include "internal.h"
#include "sanitizer.h"

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace actor_runtime {

OperationCancelled::OperationCancelled() : std::runtime_error("coroutine operation cancelled") {}

CoroutineContext::CoroutineContext(Impl* impl) noexcept : impl_(impl) {}

WaitResult CoroutineContext::await(WaitSpec spec) {
    if (impl_ == nullptr || impl_->fiber == nullptr || currentFiber != impl_->fiber || currentWorker == nullptr) {
        throw std::logic_error("await must be called by the running coroutine");
    }
    Fiber& fiber = *impl_->fiber;
    if (fiber.cancellationRequested) {
        WaitResult result;
        result.reason = WakeReason::Cancelled;
        return result;
    }
    auto immediate = currentWorker->system->installWait(fiber, spec);
    if (immediate) return std::move(*immediate);

    fiber.state = FiberState::Waiting;
    sanitizerStart(&fiber.sanitizerFakeStack, currentWorker->schedulerStackBottom, currentWorker->schedulerStackSize);
    if (::swapcontext(&fiber.context, &currentWorker->schedulerContext) != 0) {
        throw std::system_error(errno, std::generic_category(), "swapcontext to scheduler");
    }
    sanitizerFinish(fiber.sanitizerFakeStack);
    if (!fiber.pendingWait) throw std::logic_error("resumed coroutine has no wait result");
    WaitResult result = std::move(*fiber.pendingWait);
    fiber.pendingWait.reset();
    fiber.wait.reset();
    return result;
}

FiberId CoroutineContext::spawn(Actor::CoroutineBody body, ChildLifetime lifetime) {
    if (impl_ == nullptr || impl_->fiber == nullptr || currentFiber != impl_->fiber || currentWorker == nullptr) {
        throw std::logic_error("spawn must be called by the running coroutine");
    }
    Fiber& parent = *impl_->fiber;
    throwIfCancelled();
    auto control = parent.actor->actor->impl_->control.lock();
    if (!control) throw std::runtime_error("actor no longer exists");
    return currentWorker->system->createFiber(
        control, *currentWorker, std::move(body), parent.id, lifetime);
}

bool CoroutineContext::cancellationRequested() const noexcept {
    return impl_ != nullptr && impl_->fiber != nullptr && impl_->fiber->cancellationRequested;
}

bool CoroutineContext::cancel(FiberId childId) {
    if (!impl_ || !impl_->fiber || currentFiber != impl_->fiber || !currentWorker) {
        throw std::logic_error("cancel must be called by the running coroutine");
    }
    Fiber& parent = *impl_->fiber;
    if (parent.children.find(childId) == parent.children.end()) {
        throw std::invalid_argument("cancel refers to an unknown child");
    }
    auto found = parent.actor->fibers.find(childId);
    if (found == parent.actor->fibers.end() || found->second->cancellationRequested) return false;
    auto control = parent.actor->actor->impl_->control.lock();
    currentWorker->system->requestCancellation(control, *found->second);
    return true;
}

void CoroutineContext::throwIfCancelled() const {
    if (cancellationRequested()) throw OperationCancelled();
}

TaskId CoroutineContext::submitBlocking(std::function<std::any()> work) {
    if (!impl_ || !impl_->fiber || currentFiber != impl_->fiber || !currentWorker)
        throw std::logic_error("submitBlocking requires a running coroutine");
    throwIfCancelled();
    return currentWorker->system->submitBlocking(*impl_->fiber, std::move(work));
}
WaitResult CoroutineContext::blocking(std::function<std::any()> work, std::chrono::steady_clock::time_point deadline) {
    auto id = submitBlocking(std::move(work));
    return await(WaitSpec::any({WaitSpec::task(id), WaitSpec::timer(deadline)}));
}

ActorRef CoroutineContext::self() const noexcept {
    if (impl_ == nullptr || impl_->fiber == nullptr) return {};
    return impl_->fiber->actor->actor->self();
}

} // namespace actor_runtime
