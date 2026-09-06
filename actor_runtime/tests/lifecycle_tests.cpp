#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <atomic>
#include <future>
#include <iostream>
#include <thread>
using namespace actor_runtime;
using namespace std::chrono_literals;

void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct State { std::promise<void> armed; std::atomic<int> destroyed{0}, unwound{0}; };
class Dynamic final : public Actor {
public:
    Dynamic(ActorSystem& s, std::shared_ptr<State> state) : Actor(s), state_(std::move(state)) {}
    ~Dynamic() override { ++state_->destroyed; }
private:
    void onMessage(MessagePtr m) override {
        if (m->typeId != 1) return;
        start([state = state_](CoroutineContext& c) -> std::any {
            struct Guard { std::shared_ptr<State> s; ~Guard() { ++s->unwound; } } guard{state};
            state->armed.set_value();
            auto result = c.await(WaitSpec::timer(std::chrono::steady_clock::now() + 10s));
            check(result.reason == WakeReason::Cancelled, "stopped fiber did not receive cancellation");
            c.throwIfCancelled();
            return {};
        });
    }
    std::shared_ptr<State> state_;
};
int main() {
    try {
        ActorSystem::Config options;
        options.workerCount = 4;
        options.diagnosticLogging = true;
        ActorSystem system(options);
        for (int i = 0; i < 20; ++i) {
            auto old = std::make_shared<State>();
            auto ready = old->armed.get_future();
            auto first = system.registerActor("session", std::make_unique<Dynamic>(system, old));
            check(system.findActor("session").id() == first.id(), "name lookup failed");
            bool duplicate = false;
            try { system.registerActor("session", std::make_unique<Dynamic>(system, std::make_shared<State>())); }
            catch (const std::invalid_argument&) { duplicate = true; }
            check(duplicate, "duplicate actor name accepted");
            first.send(1, {});
            check(ready.wait_for(2s) == std::future_status::ready, "fiber not started");
            auto stopped = system.stopActor(first.id());
            bool stale = false;
            try { first.send(2, {}); } catch (const std::invalid_argument&) { stale = true; }
            check(stale, "unregistered ref still accepts messages");
            auto next = std::make_shared<State>();
            auto second = system.registerActor("session", std::make_unique<Dynamic>(system, next));
            check(second.id() != first.id(), "actor incarnation reused");
            check(stopped.wait_for(2s) == std::future_status::ready, "stop stalled");
            stopped.get();
            check(old->destroyed == 1 && old->unwound == 1, "destruction preceded stack unwind");
            auto nextStopped = system.stopActor(second.id());
            check(nextStopped.wait_for(2s) == std::future_status::ready, "empty actor stop stalled");
            check(next->destroyed == 1, "new actor leaked");
        }
        bool missing = false;
        try { system.findActor("session"); } catch (const std::invalid_argument&) { missing = true; }
        check(missing, "stale name remains registered");
        system.shutdown();
        check(system.stats().liveFibers == 0 && system.stats().liveStacks == 0, "live runtime objects remain");
        std::cout << "[PASS] dynamic creation, cancellation, name reuse, stale refs, destruction\n";
    } catch (const std::exception& e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
