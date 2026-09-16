#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <future>
#include <iostream>
#include <thread>

using namespace actor_runtime;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 测试从公共接口观察结果；所有协程异常都通过 promise 返回测试线程。
class ScenarioActor final : public Actor {
public:
    ScenarioActor(ActorSystem& s, CoroutineBody body, std::promise<void>& done)
        : Actor(s), body_(std::move(body)), done_(done) {}
private:
    void onMessage(MessagePtr message) override {
        if (message->typeId != 1) return;
        start([this](CoroutineContext& c) -> std::any {
            try { body_(c); done_.set_value(); }
            catch (...) { done_.set_exception(std::current_exception()); }
            return {};
        });
    }
    CoroutineBody body_;
    std::promise<void>& done_;
};

void run(const char* name, Actor::CoroutineBody body) {
    std::promise<void> done;
    auto future = done.get_future();
    ActorSystem& system = ActorSystem::start({3});
    auto actor = system.registerActor(std::make_unique<ScenarioActor>(system, std::move(body), done));
    actor.send(1, {});
    const bool ready = future.wait_for(3s) == std::future_status::ready;
    std::exception_ptr failure;
    if (ready) {
        try {
            future.get();
        } catch (...) {
            failure = std::current_exception();
        }
    }
    // 等所有协程栈真正退出后再断言，然后结束进程级单例。
    // 注意 liveFibers 与 liveStacks 是先后递减的，必须两者都归零。
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((system.stats().liveFibers != 0 || system.stats().liveStacks != 0) &&
        std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto stats = system.stats();
    ActorSystem::stop();
    if (failure) std::rethrow_exception(failure);
    check(ready, "scenario timed out");
    check(stats.liveFibers == 0 && stats.liveStacks == 0, "leaked resources");
    std::cout << "[PASS] " << name << '\n';
}

int main() {
    try {
        run("immediate timers and already-completed child outcomes", [](CoroutineContext& c) -> std::any {
            check(c.self().id() != 0 && !c.cancellationRequested(), "wrong coroutine identity");
            c.throwIfCancelled();
            auto success = c.spawn([](CoroutineContext&) -> std::any { return 81; }, ChildLifetime::CancelWithParent);
            auto failure = c.spawn([](CoroutineContext&) -> std::any { throw std::runtime_error("cached error"); }, ChildLifetime::CancelWithParent);
            auto cancelled = c.spawn([](CoroutineContext&) -> std::any { return 0; }, ChildLifetime::CancelWithParent);
            c.cancel(cancelled);
            c.await(WaitSpec::allSettled({WaitSpec::child(success), WaitSpec::child(failure), WaitSpec::child(cancelled)}));
            check(c.await(WaitSpec::child(success)).as<int>() == 81, "late join lost success");
            check(c.await(WaitSpec::child(failure)).reason == WakeReason::Error, "late join lost error");
            check(c.await(WaitSpec::child(cancelled)).reason == WakeReason::Cancelled, "late join lost cancellation");
            auto result = c.await(WaitSpec::allSettled({
                WaitSpec::all({WaitSpec::child(success), WaitSpec::child(cancelled)}),
                WaitSpec::any({WaitSpec::message({900, 0}), WaitSpec::timer(std::chrono::steady_clock::now())})}));
            check(result.children[0].reason == WakeReason::Cancelled && result.children[1].branchIndex == 1,
                  "immediate nested settlement incorrect");
            return {};
        });
        run("invalid waits are recoverable; next generation still works", [](CoroutineContext& c) -> std::any {
            bool rejected = false;
            try { c.await(WaitSpec::all({WaitSpec::message({950, 0}), WaitSpec::child(999999)})); }
            catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "unknown child not rejected");
            rejected = false;
            try { c.cancel(999999); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "unknown cancel not rejected");
            rejected = false;
            try { c.spawn({}, ChildLifetime::CancelWithParent); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "empty body not rejected");
            for (int i = 0; i < 20; ++i) {
                auto value = c.await(WaitSpec::any({WaitSpec::message({950, 0}), WaitSpec::timer(std::chrono::steady_clock::now())}));
                check(value.branchIndex == 1, "stale registration corrupted immediate wait");
            }
            c.self().send(950, 37);
            auto result = c.await(WaitSpec::message({950, 0}));
            check(std::any_cast<int>(result.as<MessagePtr>()->payload) == 37, "recovered wait lost message");
            return {};
        });
        run("cancel suspended child unwinds RAII; cancellation is sticky", [](CoroutineContext& c) -> std::any {
            auto destroyed = std::make_shared<int>(0);
            auto child = c.spawn([destroyed](CoroutineContext& sub) -> std::any {
                struct Guard { std::shared_ptr<int> n; ~Guard() { ++*n; } } guard{destroyed};
                sub.self().send(970, {}); // 通知父协程：资源已经创建，马上进入等待。
                auto wake = sub.await(WaitSpec::allSettled({
                    WaitSpec::all({WaitSpec::message({971, 0}), WaitSpec::timer(std::chrono::steady_clock::now() + 5s)}),
                    WaitSpec::timer(std::chrono::steady_clock::now() + 5s)}));
                check(wake.reason == WakeReason::Cancelled, "wrong cancellation wake reason");
                check(wake.children.empty(), "parent cancellation must override unfinished AllSettled");
                check(sub.await(WaitSpec::timer(std::chrono::steady_clock::now())).reason == WakeReason::Cancelled,
                      "cancellation was not sticky");
                bool rejected = false;
                try { sub.spawn([](CoroutineContext&) -> std::any { return {}; }, ChildLifetime::DetachedFromParent); }
                catch (const OperationCancelled&) { rejected = true; }
                check(rejected, "cancelled coroutine spawned new work");
                sub.throwIfCancelled();
                return {};
            }, ChildLifetime::CancelWithParent);
            c.await(WaitSpec::message({970, 0}));
            check(c.cancel(child), "cancel suspended child failed");
            auto done = c.await(WaitSpec::child(child));
            check(done.reason == WakeReason::Cancelled && *destroyed == 1, "stack cleanup did not happen");
            return {};
        });
        run("nested Any winner remains fixed while outer All waits", [](CoroutineContext& c) -> std::any {
            auto notifier = c.spawn([](CoroutineContext& sub) -> std::any {
                sub.self().send(980, 1);
                sub.await(WaitSpec::timer(std::chrono::steady_clock::now() + 2ms));
                sub.self().send(980, 2); // 同一个 All 叶子的重复消息必须无副作用。
                sub.self().send(981, 3);
                return 9;
            }, ChildLifetime::CancelWithParent);
            auto done = c.await(WaitSpec::all({WaitSpec::message({980, 0}),
                WaitSpec::any({WaitSpec::timer(std::chrono::steady_clock::now()), WaitSpec::message({981, 0})}),
                WaitSpec::child(notifier)}));
            check(done.children.size() == 3, "nested All returned wrong shape");
            check(std::any_cast<int>(done.children[0].as<MessagePtr>()->payload) == 1, "duplicate message overwrote result");
            check(done.children[1].reason == WakeReason::Timeout && done.children[1].branchIndex == 0,
                  "late losing branch changed nested Any winner");
            return {};
        });
        run("allSettled preserves success/error/cancellation in input order", [](CoroutineContext& c) -> std::any {
            auto good = c.spawn([](CoroutineContext&) -> std::any { return 42; }, ChildLifetime::CancelWithParent);
            auto bad = c.spawn([](CoroutineContext&) -> std::any { throw std::runtime_error("declined"); }, ChildLifetime::CancelWithParent);
            auto cancelled = c.spawn([](CoroutineContext&) -> std::any { throw std::logic_error("must not start"); }, ChildLifetime::CancelWithParent);
            check(c.cancel(cancelled), "first cancellation must succeed");
            check(!c.cancel(cancelled), "repeated cancellation must be idempotent");
            auto result = c.await(WaitSpec::allSettled({WaitSpec::child(good), WaitSpec::child(bad), WaitSpec::child(cancelled)}));
            check(result.reason == WakeReason::Settled && !result.error, "settled container must not fail");
            check(result.children.size() == 3 && result.children[0].as<int>() == 42, "success lost");
            check(result.children[1].reason == WakeReason::Error && result.children[1].error, "error lost");
            check(result.children[2].reason == WakeReason::Cancelled, "cancelled child lost");
            check(!c.cancel(good), "completed child cancellation should do nothing");
            return {};
        });
        run("late timer from a losing Any cannot complete the next wait generation", [](CoroutineContext& c) -> std::any {
            auto quick = c.spawn([](CoroutineContext&) -> std::any { return 11; }, ChildLifetime::CancelWithParent);
            auto first = c.await(WaitSpec::any({WaitSpec::child(quick),
                WaitSpec::timer(std::chrono::steady_clock::now() + 30ms)}));
            check(first.reason == WakeReason::ChildCompleted && first.as<int>() == 11, "quick child did not win");
            auto later = c.spawn([](CoroutineContext& sub) -> std::any {
                sub.await(WaitSpec::timer(std::chrono::steady_clock::now() + 60ms));
                sub.self().send(990, 22);
                return {};
            }, ChildLifetime::CancelWithParent);
            auto second = c.await(WaitSpec::message({990, 0}));
            check(second.reason == WakeReason::Message && std::any_cast<int>(second.as<MessagePtr>()->payload) == 22,
                  "stale timer woke a newer wait generation");
            c.await(WaitSpec::child(later));
            return {};
        });
        run("All aggregates a rethrowable child error after all children finish", [](CoroutineContext& c) -> std::any {
            auto bad = c.spawn([](CoroutineContext&) -> std::any { throw std::runtime_error("failure"); }, ChildLifetime::CancelWithParent);
            auto result = c.await(WaitSpec::all({WaitSpec::child(bad), WaitSpec::timer(std::chrono::steady_clock::now() + 2ms)}));
            check(result.children.size() == 2 && result.children[1].reason == WakeReason::Timeout, "All did not settle all branches");
            bool thrown = false;
            try { result.rethrowIfError(); } catch (const std::runtime_error&) { thrown = true; }
            check(thrown, "All lost aggregate exception");
            return {};
        });
    } catch (const std::exception& error) { std::cerr << "[FAIL] " << error.what() << '\n'; return 1; }
}
