#pragma once

#include "actor_runtime/actor_ref.h"

#include <any>
#include <exception>
#include <functional>
#include <memory>

namespace actor_runtime {

class ActorSystem;
class CoroutineContext;

class Actor {
public:
    using CoroutineBody = std::function<std::any(CoroutineContext&)>;

    Actor(const Actor&) = delete;
    Actor& operator=(const Actor&) = delete;
    virtual ~Actor();

    ActorId id() const noexcept;
    ActorRef self() const noexcept;

protected:
    explicit Actor(ActorSystem& system);

    FiberId start(CoroutineBody body);
    virtual void onMessage(MessagePtr message) = 0;
    virtual void onCoroutineError(FiberId fiberId, std::exception_ptr error);

private:
    friend class ActorSystem;
    friend class CoroutineContext;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace actor_runtime
