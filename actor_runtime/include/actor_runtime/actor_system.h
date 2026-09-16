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
    // 带 ASan fiber 注解的协程栈切换累计次数（诊断用）。
    std::size_t fiberSwitches = 0;
    bool acceptingMessages = false;
    // 当前进程是否链接了 AddressSanitizer 的 fiber hook 实现。
    bool asanFiberHooks = false;
};

// 进程级单例。一个进程最多存在一个运行中的 ActorSystem：
// main 调用 ActorSystem::start()，子模块通过 ActorSystem::instance()
// 取得同一个实例并注册自己的 Actor。
class ActorSystem final {
public:
    struct Impl;

    struct Config {
        std::size_t workerCount = 0;
        std::size_t fiberStackSize = 256U * 1024U;
        std::chrono::milliseconds shutdownTimeout{5000};
    };

    // 由 main 调用。已经有运行中的实例时抛 std::logic_error。
    // 配置非法时抛 std::invalid_argument，且不改变“未运行”状态。
    static ActorSystem& start();
    static ActorSystem& start(Config config);
    // 停止并回收全部 worker、Actor 与协程栈；幂等。
    // 不得从运行库的 worker 线程（例如 onMessage/协程体内）调用。
    static void stop();
    static bool isRunning();
    // 未运行时抛 std::logic_error。
    static ActorSystem& instance();

    ActorSystem(const ActorSystem&) = delete;
    ActorSystem& operator=(const ActorSystem&) = delete;
    ActorSystem(ActorSystem&&) = delete;
    ActorSystem& operator=(ActorSystem&&) = delete;

    ActorRef registerActor(std::unique_ptr<Actor> actor);
    ActorRef ref(ActorId id) noexcept;
    MessageId send(
        ActorId recipient,
        TypeId typeId,
        std::any payload,
        MessageId correlationId = NoCorrelation);

    RuntimeStats stats() const noexcept;

private:
    friend class Actor;
    friend class ActorRef;
    friend class CoroutineContext;
    ActorSystem() = default;
    ~ActorSystem() = default;

    // 单例外壳：进程内只分配一次，且永不析构（定义在 actor_system.cpp）。
    static ActorSystem& shell();

    std::unique_ptr<Impl> impl_;
};

} // namespace actor_runtime
