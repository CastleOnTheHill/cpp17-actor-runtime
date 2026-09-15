#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <functional>
#include <iostream>
using namespace actor_runtime;
using namespace std::chrono_literals;

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct State {
    std::weak_ptr<int> result;
    std::promise<void> finished;
};
class Scenario final : public Actor {
public:
    using Body = std::function<std::any(CoroutineContext&, State&)>;
    Scenario(ActorSystem& system, std::shared_ptr<State> state, Body body)
        : Actor(system), state_(std::move(state)), body_(std::move(body)) {}
private:
    void onMessage(MessagePtr message) override {
        if (message->typeId == 1) {
            start([this](CoroutineContext& context) { return body_(context, *state_); });
        } else if (message->typeId == 2) {
            try {
                check(state_->result.expired(), "finished root/parent still owns payload");
                state_->finished.set_value();
            } catch (...) { state_->finished.set_exception(std::current_exception()); }
        }
    }
    void onCoroutineError(FiberId, std::exception_ptr error) override {
        state_->finished.set_exception(error);
    }
    std::shared_ptr<State> state_;
    Body body_;
};
static void run(const char* name, Scenario::Body body) {
    ActorSystem::Config config;
    config.workerCount = 1;
    ActorSystem system(config);
    auto state = std::make_shared<State>();
    auto finished = state->finished.get_future();
    auto actor = system.registerActor(std::make_unique<Scenario>(system, state, std::move(body)));
    actor.send(1, {});
    check(finished.wait_for(5s) == std::future_status::ready, "scenario timed out");
    finished.get();
    system.shutdown();
    check(system.stats().liveStacks == 0, "stack still live");
    std::cout << "PASS " << name << '\n';
}
int main() {
    int failures = 0;
    const auto test = [&](const char* name, Scenario::Body body) {
        try { run(name, std::move(body)); }
        catch (const std::exception& error) {
            ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    test("root result released without stopping actor", [](CoroutineContext& c, State& s) -> std::any {
        auto result = std::make_shared<int>(42); s.result = result;
        c.self().send(2, {});
        return result;
    });
    test("child result released when parent exits", [](CoroutineContext& c, State& s) -> std::any {
        auto child = c.spawn([&s](CoroutineContext&) -> std::any {
            auto result = std::make_shared<int>(42); s.result = result; return result;
        }, ChildLifetime::CancelWithParent);
        check(*c.await(WaitSpec::child(child)).as<std::shared_ptr<int>>() == 42, "lost child result");
        check(*c.await(WaitSpec::child(child)).as<std::shared_ptr<int>>() == 42, "lost repeated join");
        c.self().send(2, {});
        return {};
    });
    test("blocking convenience releases before parent exits", [](CoroutineContext& c, State& s) -> std::any {
        auto input = std::make_shared<int>(42); s.result = input;
        {
            auto result = c.blocking([value = std::move(input)]() -> std::any { return value; },
                                     std::chrono::steady_clock::now() + 2s);
            check(*result.as<std::shared_ptr<int>>() == 42, "lost blocking result");
        }
        check(s.result.expired(), "blocking retained payload in live parent");
        c.self().send(2, {});
        return {};
    });
    test("forgetChild releases the completed child result", [](CoroutineContext& c, State& s) -> std::any {
        const auto child = c.spawn([&s](CoroutineContext&) -> std::any {
            auto result = std::make_shared<int>(7); s.result = result; return result;
        }, ChildLifetime::CancelWithParent);
        { auto joined = c.await(WaitSpec::child(child)); check(*joined.as<std::shared_ptr<int>>() == 7, "lost child result"); }
        c.forgetChild(child);
        check(s.result.expired(), "forgotten child result retained");
        bool rejected = false;
        try { c.await(WaitSpec::child(child)); } catch (const std::exception&) { rejected = true; }
        check(rejected, "await after forgetChild did not fail");
        c.self().send(2, {});
        return {};
    });
    test("forgetChild rejects a child that is still running", [](CoroutineContext& c, State& s) -> std::any {
        const auto child = c.spawn([&s](CoroutineContext&) -> std::any {
            auto result = std::make_shared<int>(7); s.result = result; return result;
        }, ChildLifetime::CancelWithParent);
        bool rejected = false;
        try { c.forgetChild(child); } catch (const std::logic_error&) { rejected = true; }
        check(rejected, "forgetChild accepted a running child");
        { auto joined = c.await(WaitSpec::child(child)); check(*joined.as<std::shared_ptr<int>>() == 7, "lost child result"); }
        c.self().send(2, {});
        return {};
    });
    test("forgetTask releases an explicit blocking result", [](CoroutineContext& c, State& s) -> std::any {
        const auto id = c.submitBlocking([&s]() -> std::any {
            auto result = std::make_shared<int>(9); s.result = result; return result;
        });
        { auto waited = c.await(WaitSpec::task(id)); check(*waited.as<std::shared_ptr<int>>() == 9, "lost task result"); }
        c.forgetTask(id);
        check(s.result.expired(), "forgotten task result retained");
        bool rejected = false;
        try { c.await(WaitSpec::task(id)); } catch (const std::exception&) { rejected = true; }
        check(rejected, "await after forgetTask did not fail");
        c.self().send(2, {});
        return {};
    });
    return failures ? 1 : 0;
}
