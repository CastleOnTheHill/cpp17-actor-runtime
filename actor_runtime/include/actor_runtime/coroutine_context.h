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
    // 放弃已完成直接子协程的结果；之后不能再 join/cancel 该 ID。
    // 正在运行的子协程不可 forget，须先取消并 join 或等待其完成。
    void forgetChild(FiberId childId);
    // work 必须捕获自有数据，不能捕获 Actor/CoroutineContext 裸指针。
    TaskId submitBlocking(std::function<std::any()> work);
    // 放弃当前协程的 task 结果（可仍在运行）；不取消外部工作。
    // 晚完成结果会丢弃，之后再次 await 此 ID 报 unknown task。
    void forgetTask(TaskId taskId);
    WaitResult blocking(std::function<std::any()> work, std::chrono::steady_clock::time_point deadline);
    bool cancellationRequested() const noexcept;
    void throwIfCancelled() const;
    ActorRef self() const noexcept;

private:
    friend class Actor;
    Impl* impl_;
};

} // namespace actor_runtime
