#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <algorithm>
#include <array>
#include <future>
#include <iostream>
#include <thread>
using namespace actor_runtime;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr std::size_t repetitions = 30;
struct Round {
    std::promise<void> started, release;
    std::shared_future<void> gate = release.get_future().share();
    Clock::time_point releasedAt;
    bool released = false;
    void open() {
        if (!released) { releasedAt = Clock::now(); released = true; release.set_value(); }
    }
};
struct State {
    std::array<Round, repetitions> rounds;
    std::array<double, repetitions> latency{};
    std::promise<void> done;
};
class Awaiter final : public Actor {
public:
    Awaiter(ActorSystem& system, std::shared_ptr<State> s) : Actor(system), state(std::move(s)) {}
private:
    void onCoroutineError(FiberId, std::exception_ptr error) override { state->done.set_exception(error); }
    void onMessage(MessagePtr) override {
        start([s=state](CoroutineContext& ctx) -> std::any {
            for (std::size_t i=0; i<repetitions; ++i) {
                auto task = ctx.submitBlocking([s,i]() -> std::any {
                    s->rounds[i].started.set_value(); s->rounds[i].gate.wait(); return 42;
                });
                auto result = ctx.await(WaitSpec::task(task)); // No timer/global message.
                ctx.throwIfCancelled();
                if (result.as<int>() != 42) throw std::runtime_error("completion payload lost");
                s->latency[i] = std::chrono::duration<double,std::micro>(Clock::now()-s->rounds[i].releasedAt).count();
            }
            s->done.set_value(); return {};
        });
    }
    std::shared_ptr<State> state;
};
int main() {
    try {
        auto state = std::make_shared<State>(); auto done = state->done.get_future();
        ActorSystem::Config options; options.workerCount=1; options.blockingThreads=1;
        ActorSystem system(options);
        struct ReleaseOnExit {
            std::shared_ptr<State> state;
            ~ReleaseOnExit() { for (auto& r:state->rounds) r.open(); }
        } release{state};
        auto actor = system.registerActor(std::make_unique<Awaiter>(system,state));
        actor.send(1,{});
        for (auto& round:state->rounds) {
            if (round.started.get_future().wait_for(2s)!=std::future_status::ready)
                throw std::runtime_error("blocking operation did not start");
            // Let the only scheduler become idle, without any timer/Actor traffic.
            std::this_thread::sleep_for(2ms); round.open();
        }
        if (done.wait_for(2s)!=std::future_status::ready) throw std::runtime_error("local completion stalled");
        done.get(); std::sort(state->latency.begin(),state->latency.end());
        const double median=state->latency[repetitions/2];
        std::cout << "blocking completion -> coroutine resume median_us=" << median << '\n';
        // Wide allowance detects repeated ~8ms periodic recovery, not one outlier.
        if (median>=4000) throw std::runtime_error("local completion relies on periodic polling instead of notification");
        system.shutdown();
        if (system.stats().liveFibers || system.stats().liveStacks) throw std::runtime_error("fiber leaked");
        std::cout << "PASS local-only completion wakes sleeping owner\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
