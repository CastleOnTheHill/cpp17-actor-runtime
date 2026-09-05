#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace ar = actor_runtime;
using namespace std::chrono_literals;

class OrderWorkflow final : public ar::Actor {
public:
    OrderWorkflow(ar::ActorSystem& system, std::atomic<bool>& done)
        : Actor(system), done_(done) {}

private:
    void onMessage(ar::MessagePtr) override {
        start([this](ar::CoroutineContext& context) -> std::any {
            const auto price = context.spawn([](ar::CoroutineContext& child) -> std::any {
                child.await(ar::WaitSpec::timer(std::chrono::steady_clock::now() + 15ms));
                return 199;
            }, ar::ChildLifetime::CancelWithParent);
            const auto stock = context.spawn([](ar::CoroutineContext& child) -> std::any {
                child.await(ar::WaitSpec::timer(std::chrono::steady_clock::now() + 25ms));
                return true;
            }, ar::ChildLifetime::CancelWithParent);

            const auto result = context.await(ar::WaitSpec::all({
                ar::WaitSpec::child(price), ar::WaitSpec::child(stock)}));
            std::cout << "order ready: price=" << result.children[0].as<int>()
                      << ", in_stock=" << std::boolalpha << result.children[1].as<bool>() << '\n';
            done_.store(true, std::memory_order_release);
            return {};
        });
    }

    std::atomic<bool>& done_;
};

int main() {
    ar::ActorSystem system({3});
    std::atomic<bool> done{false};
    const auto workflow = system.registerActor(std::make_unique<OrderWorkflow>(system, done));
    workflow.send(1, {});
    while (!done.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    system.shutdown();
}
