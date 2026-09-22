#include "modules/inventory_module.h"

#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"

#include <any>
#include <chrono>
#include <memory>

namespace inventory {
namespace {

using namespace std::chrono_literals;

// 没有用户声明的构造函数：默认构造会绑定进程全局 ActorSystem。
class InventoryActor final : public actor_runtime::Actor {
private:
    void onMessage(actor_runtime::MessagePtr message) override {
        if (message->typeId != QueryStock) return;
        const auto client = message->sender;
        const auto correlation = message->id;
        start([client, correlation](actor_runtime::CoroutineContext& context) -> std::any {
            context.await(actor_runtime::WaitSpec::timer(
                std::chrono::steady_clock::now() + 2ms));
            actor_runtime::ActorSystem::instance().send(client, StockReply, int{7}, correlation);
            return {};
        });
    }
};

} // namespace

actor_runtime::ActorRef install() {
    return actor_runtime::ActorSystem::instance().registerActor(
        std::make_unique<InventoryActor>());
}

} // namespace inventory
