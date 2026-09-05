#pragma once

#include "actor_runtime/actor.h"
#include "actor_runtime/wait.h"

#include <stdexcept>

namespace actor_runtime {

enum class ChildLifetime { CancelWithParent, DetachedFromParent };

class OperationCancelled : public std::runtime_error {
public:
    OperationCancelled();
};

class CoroutineContext final {
public:
    struct Impl;

    // Internal constructor; Impl is opaque to callers.
    explicit CoroutineContext(Impl* impl) noexcept;

    WaitResult await(WaitSpec spec);
    FiberId spawn(Actor::CoroutineBody body, ChildLifetime lifetime);
    // 仅能取消当前协程的直接子协程；重复请求/已经完成返回 false。
    // 请求取消不会内联恢复子协程；用 await(child(id)) 等待清理结束。
    bool cancel(FiberId childId);
    bool cancellationRequested() const noexcept;
    void throwIfCancelled() const;
    ActorRef self() const noexcept;

private:
    friend class Actor;
    Impl* impl_;
};

} // namespace actor_runtime
