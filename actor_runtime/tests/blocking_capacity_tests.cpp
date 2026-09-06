#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <future>
#include <iostream>
using namespace actor_runtime;
using namespace std::chrono_literals;
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
struct State {
    std::promise<void> started, release, done;
    std::shared_future<void> gate = release.get_future().share();
};
class Capacity final : public Actor {
public:
    Capacity(ActorSystem& s, std::shared_ptr<State> state) : Actor(s), state_(std::move(state)) {}
private:
    void onMessage(MessagePtr message) override {
        if (message->typeId != 1) return;
        start([state = state_](CoroutineContext& c) -> std::any {
            try {
                auto first = c.submitBlocking([state]() -> std::any {
                    state->started.set_value(); state->gate.wait(); return 1;
                });
                c.await(WaitSpec::message({2, 0})); // 主线程确认唯一线程正在执行 first 后才发消息。
                auto queued = c.submitBlocking([]() -> std::any { return 2; });
                bool rejected = false;
                try { c.submitBlocking([]() -> std::any { return 3; }); }
                catch (const std::runtime_error&) { rejected = true; }
                check(rejected, "full blocking queue was not rejected");
                state->release.set_value();
                auto result = c.await(WaitSpec::all({WaitSpec::task(first), WaitSpec::task(queued)}));
                check(result.children[0].as<int>() == 1 && result.children[1].as<int>() == 2, "queued work lost");
                state->done.set_value();
            } catch (...) {
                try { state->release.set_value(); } catch (...) {}
                state->done.set_exception(std::current_exception());
            }
            return {};
        });
    }
    std::shared_ptr<State> state_;
};
int main() {
    try {
        auto state = std::make_shared<State>();
        auto started = state->started.get_future(), done = state->done.get_future();
        ActorSystem::Config config; config.workerCount = 1; config.blockingThreads = 1; config.blockingQueueCapacity = 1;
        ActorSystem system(config);
        auto actor = system.registerActor("capacity", std::make_unique<Capacity>(system, state));
        actor.send(1, {});
        if (started.wait_for(2s) != std::future_status::ready) { state->release.set_value(); throw std::runtime_error("work not started"); }
        actor.send(2, {});
        if (done.wait_for(2s) != std::future_status::ready) { state->release.set_value(); throw std::runtime_error("capacity test stalled"); }
        done.get(); system.shutdown();
        std::cout << "[PASS] bounded blocking queue backpressure and continued work\n";
    } catch (const std::exception& e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
