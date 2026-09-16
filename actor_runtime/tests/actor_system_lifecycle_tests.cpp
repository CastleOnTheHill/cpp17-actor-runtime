// ActorSystem 的进程级单例契约：main 启动，子模块通过 instance() 注册。
#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace actor_runtime;
using namespace std::chrono_literals;

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Exception, class Operation>
void expectFailure(const Operation& operation, const char* message) {
    bool caught = false;
    try {
        operation();
    } catch (const Exception&) {
        caught = true;
    }
    require(caught, message);
}

template<class Predicate>
void eventually(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(predicate(), message);
}

// 子模块风格的 Actor：默认构造绑定进程全局实例，不需要外部注入 ActorSystem。
class ModuleActor final : public Actor {
public:
    struct Counters {
        std::atomic<int> handled{0};
        std::atomic<int> stopRejected{0};
        std::atomic<int> coroutineDone{0};
        std::atomic<int> looping{0};
    };

    explicit ModuleActor(Counters& counters) : counters_(counters) {}

private:
    void onMessage(MessagePtr message) override {
        if (message->typeId == 1) {
            counters_.handled.fetch_add(1, std::memory_order_release);
            return;
        }
        if (message->typeId == 3) {
            start([this](CoroutineContext& context) -> std::any {
                context.await(WaitSpec::timer(std::chrono::steady_clock::now() + 2ms));
                counters_.coroutineDone.fetch_add(1, std::memory_order_release);
                return {};
            });
            return;
        }
        if (message->typeId == 4) {
            start([this](CoroutineContext& context) -> std::any {
                counters_.looping.store(1, std::memory_order_release);
                // 密集取用全局实例，逼近“stop() 持状态锁等待 join”的死锁窗口。
                // 上限 3 秒只是兜底；正常情况下取消会立即让循环退出。
                const auto until = std::chrono::steady_clock::now() + 3s;
                while (std::chrono::steady_clock::now() < until) {
                    (void)ActorSystem::instance().stats();
                    if (context.cancellationRequested()) break;
                }
                context.throwIfCancelled();
                return {};
            });
            return;
        }
        // 运行库 worker 线程上禁止停止整个系统。
        try {
            ActorSystem::stop();
        } catch (const std::logic_error&) {
            counters_.stopRejected.fetch_add(1, std::memory_order_release);
        }
    }

    Counters& counters_;
};

} // namespace

int main() {
    try {
        ModuleActor::Counters counters;

        // 1. 未启动：instance() 与绑定全局实例的 Actor 都不可用。
        require(!ActorSystem::isRunning(), "system reported running before start");
        expectFailure<std::logic_error>([] { ActorSystem::instance(); },
            "instance() before start must fail");
        expectFailure<std::logic_error>([&] { auto actor = std::make_unique<ModuleActor>(counters); },
            "Actor() before start must fail");

        // 2. 非法配置：start 抛出且不留下半启动状态。
        expectFailure<std::invalid_argument>([] {
            ActorSystem::Config config;
            config.fiberStackSize = 1024;
            ActorSystem::start(config);
        }, "invalid config must be rejected");
        require(!ActorSystem::isRunning(), "failed start changed the running state");

        // 3. main 启动，并且一个进程只允许一个实例。
        ActorSystem& system = ActorSystem::start({2});
        require(ActorSystem::isRunning(), "start did not make the system running");
        require(&ActorSystem::instance() == &system, "instance() differs from start() result");
        require(system.stats().workerCount == 2, "worker count mismatch");
        expectFailure<std::logic_error>([] { ActorSystem::start({1}); }, "second start must be rejected");

        // 4. 子模块注册并工作；协程切换统计与 ASan hook 状态由 stats 暴露。
        auto module = system.registerActor(std::make_unique<ModuleActor>(counters));
        module.send(1, {});
        module.send(2, {});
        module.send(3, {});
        eventually([&] { return counters.handled.load(std::memory_order_acquire) == 1; },
            "module actor did not handle its message");
        eventually([&] { return counters.stopRejected.load(std::memory_order_acquire) == 1; },
            "stop() from a worker thread was not rejected");
        eventually([&] { return counters.coroutineDone.load(std::memory_order_acquire) == 1; },
            "coroutine did not finish");
        eventually([&] {
            const auto snapshot = system.stats();
            return snapshot.liveFibers == 0 && snapshot.liveStacks == 0;
        }, "fiber stacks were not reclaimed");
        require(ActorSystem::isRunning(), "worker-thread stop() stopped the system");
        const auto running = system.stats();
        require(running.acceptingMessages, "running system stopped accepting messages");
        require(running.fiberSwitches > 0, "no annotated fiber switch was recorded");

        // 5. 本世代构造、尚未注册的 Actor，用于验证停止/重启后的注册行为。
        auto orphan = std::make_unique<ModuleActor>(counters);
        auto stale = std::make_unique<ModuleActor>(counters);

        // 6. 长跑协程持续取用全局实例时停止：stop 不能持锁等待 join。
        module.send(4, {});
        eventually([&] { return counters.looping.load(std::memory_order_acquire) == 1; },
            "global-instance loop did not start");
        const auto stopStarted = std::chrono::steady_clock::now();
        ActorSystem::stop();
        const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
        require(stopElapsed < 4s, "stop() blocked behind worker access to the global instance");

        // 7. 停止：幂等，且释放全部 worker 与协程栈。
        require(!ActorSystem::isRunning(), "stop did not clear the running state");
        ActorSystem::stop();
        const auto stopped = system.stats();
        require(stopped.workerCount == 0 && stopped.liveFibers == 0 && stopped.liveStacks == 0 &&
            !stopped.acceptingMessages, "stopped stats are not empty");
        expectFailure<std::logic_error>([] { ActorSystem::instance(); },
            "instance() after stop must fail");
        expectFailure<std::runtime_error>([&] { module.send(1, {}); }, "send after stop must fail");
        expectFailure<std::runtime_error>([&] { system.registerActor(std::move(orphan)); },
            "registerActor after stop must fail");
        expectFailure<std::logic_error>([&] { system.registerActor(std::make_unique<ModuleActor>(counters)); },
            "constructing an Actor while stopped must fail");

        // 8. 重启后旧世代 Actor 被拒绝，新 Actor 正常工作。
        ActorSystem& restarted = ActorSystem::start();
        require(restarted.stats().workerCount > 0, "restart with the default config failed");
        expectFailure<std::invalid_argument>([&] { restarted.registerActor(std::move(stale)); },
            "actor from a previous generation must be rejected");
        auto fresh = restarted.registerActor(std::make_unique<ModuleActor>(counters));
        fresh.send(1, {});
        eventually([&] { return counters.handled.load(std::memory_order_acquire) == 2; },
            "restarted system did not dispatch");
        ActorSystem::stop();
        require(!ActorSystem::isRunning(), "restarted system did not stop");

        std::cout << "[PASS] process-wide ActorSystem lifecycle, submodule registration, "
                     "non-blocking stop, idempotent stop and restart\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
