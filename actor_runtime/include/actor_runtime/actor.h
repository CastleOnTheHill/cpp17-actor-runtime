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
    // 绑定到进程全局实例（ActorSystem::instance()），供子模块使用。
    // 系统未运行时抛 std::logic_error。
    Actor();
    // 显式绑定；actor 必须在系统运行期间构造，否则注册时会被拒绝。
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
