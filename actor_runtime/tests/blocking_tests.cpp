#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
using namespace actor_runtime;
using namespace std::chrono_literals;
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
struct State {
    std::promise<void> done, armed;
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<int> ping{0}, backgroundDone{0};
};
class WorkerActor final : public Actor {
public:
    WorkerActor(ActorSystem& s, std::shared_ptr<State> state) : Actor(s), state_(std::move(state)) {}
private:
    void onCoroutineError(FiberId, std::exception_ptr e) override { state_->done.set_exception(e); }
    void onMessage(MessagePtr message) override {
        if (message->typeId == 2) { ++state_->ping; return; }
        if (message->typeId != 1) return;
        start([state = state_](CoroutineContext& c) -> std::any {
            auto workerThread = std::this_thread::get_id();
            auto value = c.blocking([workerThread]() -> std::any {
                check(std::this_thread::get_id() != workerThread, "blocking work ran on actor worker");
                return 42;
            }, std::chrono::steady_clock::now() + 2s);
            check(value.reason == WakeReason::TaskCompleted && value.as<int>() == 42, "blocking result lost");
            auto failure = c.submitBlocking([]() -> std::any { throw std::runtime_error("I/O failed"); });
            auto settled = c.await(WaitSpec::allSettled({WaitSpec::task(failure), WaitSpec::timer(std::chrono::steady_clock::now())}));
            check(settled.children[0].reason == WakeReason::Error && settled.children[0].error, "blocking error lost");
            auto longTask = c.submitBlocking([state]() -> std::any {
                state->armed.set_value();
                state->gate.wait();
                ++state->backgroundDone;
                return 7;
            });
            c.self().send(2, {});
            auto timeout = c.await(WaitSpec::any({WaitSpec::task(longTask),
                WaitSpec::timer(std::chrono::steady_clock::now() + 20ms)}));
            check(timeout.reason == WakeReason::Timeout, "blocking task did not time out");
            check(state->ping == 1 && state->backgroundDone == 0, "actor worker was blocked by external function");
            state->release.set_value();
            auto joined = c.await(WaitSpec::task(longTask));
            check(joined.as<int>() == 7 && state->backgroundDone == 1, "late completion cannot be joined");
            check(c.await(WaitSpec::task(longTask)).as<int>() == 7, "cached task completion lost");
            bool unknown = false;
            try { c.await(WaitSpec::task(99999)); } catch (const std::invalid_argument&) { unknown = true; }
            check(unknown, "unknown blocking task accepted");
            state->done.set_value();
            return {};
        });
    }
    std::shared_ptr<State> state_;
};

class StopDuringWork final : public Actor {
public:
    StopDuringWork(ActorSystem& s, std::shared_ptr<State> state) : Actor(s), state_(std::move(state)) {}
private:
    void onMessage(MessagePtr) override {
        start([state = state_](CoroutineContext& c) -> std::any {
            c.blocking([state]() -> std::any {
                state->armed.set_value(); state->gate.wait(); ++state->backgroundDone; return 1;
            }, std::chrono::steady_clock::now() + 10s);
            c.throwIfCancelled(); return {};
        });
    }
    std::shared_ptr<State> state_;
};
int main() {
    try {
        auto state = std::make_shared<State>();
        auto done = state->done.get_future();
        ActorSystem::Config config; config.workerCount = 1; config.blockingThreads = 1; config.diagnosticLogging = true;
        ActorSystem system(config);
        auto actor = system.registerActor("work", std::make_unique<WorkerActor>(system, state));
        actor.send(1, {});
        if (done.wait_for(3s) != std::future_status::ready) { state->release.set_value(); throw std::runtime_error("work scenario stalled"); }
        done.get();
        auto second = std::make_shared<State>(); auto armed = second->armed.get_future();
        auto retiring = system.registerActor("retire", std::make_unique<StopDuringWork>(system, second));
        retiring.send(1, {});
        check(armed.wait_for(2s) == std::future_status::ready, "background did not start");
        auto stopped = system.stopActor(retiring.id());
        bool promptly = stopped.wait_for(1s) == std::future_status::ready;
        second->release.set_value();
        check(promptly, "stopping actor waited for a blocked external function");
        system.shutdown();
        check(second->backgroundDone == 1 && system.stats().liveStacks == 0, "late work or stacks leaked");
        std::cout << "[PASS] isolated blocking work, timeout, ping fairness, late join, actor stop during I/O\n";
    } catch (const std::exception& e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
