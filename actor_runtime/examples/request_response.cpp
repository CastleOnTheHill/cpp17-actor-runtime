#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace ar = actor_runtime;
using namespace std::chrono_literals;

constexpr ar::TypeId Start = 1;
constexpr ar::TypeId Lookup = 2;
constexpr ar::TypeId LookupResult = 3;

class Catalog final : public ar::Actor {
public:
    explicit Catalog(ar::ActorSystem& system) : Actor(system), system_(system) {}

private:
    void onMessage(ar::MessagePtr message) override {
        if (message->typeId != Lookup) return;
        const auto sku = std::any_cast<std::string>(message->payload);
        system_.ref(message->sender).send(
            LookupResult, std::string("available: ") + sku, message->id);
    }

    ar::ActorSystem& system_;
};

class Checkout final : public ar::Actor {
public:
    Checkout(ar::ActorSystem& system, ar::ActorRef catalog, std::atomic<bool>& done)
        : Actor(system), catalog_(catalog), done_(done) {}

private:
    void onMessage(ar::MessagePtr message) override {
        if (message->typeId != Start) return;
        start([this](ar::CoroutineContext& context) -> std::any {
            const auto requestId = catalog_.send(Lookup, std::string("SKU-42"));
            const auto result = context.await(ar::WaitSpec::any({
                ar::WaitSpec::message({LookupResult, requestId}),
                ar::WaitSpec::timer(std::chrono::steady_clock::now() + 250ms)}));
            if (result.branchIndex == 0) {
                const auto& response = result.as<ar::MessagePtr>();
                std::cout << std::any_cast<std::string>(response->payload) << '\n';
            } else {
                std::cout << "catalog request timed out\n";
            }
            done_.store(true, std::memory_order_release);
            return {};
        });
    }

    ar::ActorRef catalog_;
    std::atomic<bool>& done_;
};

int main() {
    ar::ActorSystem& system = ar::ActorSystem::start({2});
    std::atomic<bool> done{false};
    const auto catalog = system.registerActor(std::make_unique<Catalog>(system));
    const auto checkout = system.registerActor(std::make_unique<Checkout>(system, catalog, done));
    checkout.send(Start, {});
    while (!done.load(std::memory_order_acquire)) std::this_thread::sleep_for(1ms);
    ar::ActorSystem::stop();
}
