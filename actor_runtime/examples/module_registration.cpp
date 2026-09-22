// 进程级 ActorSystem 的推荐用法：
//   main 负责 start/stop，子模块只通过 ActorSystem::instance() 取得全局实例
//   并注册自己的 Actor —— 所有模块共享同一个运行库实例。
#include "modules/inventory_module.h"
#include "modules/order_module.h"

#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"

#include <any>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace ar = actor_runtime;
using namespace std::chrono_literals;

namespace {

// 演示客户端：把跨模块调用的最终结果收回 main。
class Client final : public ar::Actor {
public:
    Client(ar::ActorRef order, std::string& summary, std::atomic<bool>& done)
        : order_(order), summary_(summary), done_(done) {}

private:
    void onMessage(ar::MessagePtr message) override {
        if (message->typeId == 1) {
            order_.send(orders::PlaceOrder, std::string("SKU-42"));
            return;
        }
        if (message->typeId == orders::OrderResult) {
            summary_ = std::any_cast<std::string>(message->payload);
            done_.store(true, std::memory_order_release);
        }
    }

    ar::ActorRef order_;
    std::string& summary_;
    std::atomic<bool>& done_;
};

} // namespace

int main() {
    ar::ActorSystem& system = ar::ActorSystem::start({2});

    // 子模块自己注册 Actor，main 只需要知道它们的入口引用。
    const auto inventory = inventory::install();
    const auto order = orders::install(inventory);

    std::string summary;
    std::atomic<bool> done{false};
    const auto client = system.registerActor(std::make_unique<Client>(order, summary, done));
    client.send(1, {});

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto stats = system.stats();
    ar::ActorSystem::stop();

    if (!done.load(std::memory_order_acquire)) {
        std::cerr << "[FAIL] module call stalled\n";
        return 1;
    }
    std::cout << "[PASS] " << summary << "，worker=" << stats.workerCount
              << "，协程切换=" << stats.fiberSwitches << '\n';
    return 0;
}
