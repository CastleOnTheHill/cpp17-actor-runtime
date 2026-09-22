#include "modules/order_module.h"

#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include "modules/inventory_module.h"

#include <any>
#include <chrono>
#include <memory>
#include <string>

namespace orders {
namespace {

using namespace std::chrono_literals;

class OrderActor final : public actor_runtime::Actor {
public:
    // 显式使用 Actor()：绑定进程全局实例，而不是注入 ActorSystem。
    explicit OrderActor(actor_runtime::ActorRef inventory) : Actor(), inventory_(inventory) {}

private:
    void onMessage(actor_runtime::MessagePtr message) override {
        if (message->typeId != PlaceOrder) return;
        const auto sku = std::any_cast<std::string>(message->payload);
        const auto client = message->sender;
        const auto correlation = message->id;
        start([this, sku, client, correlation](actor_runtime::CoroutineContext& context) -> std::any {
            const auto requestId = inventory_.send(inventory::QueryStock, sku);
            const auto reply = context.await(actor_runtime::WaitSpec::any({
                actor_runtime::WaitSpec::message({inventory::StockReply, requestId}),
                actor_runtime::WaitSpec::timer(std::chrono::steady_clock::now() + 250ms)}));
            std::string summary;
            if (reply.branchIndex == 0) {
                const auto quantity = std::any_cast<int>(reply.as<actor_runtime::MessagePtr>()->payload);
                summary = sku + " x " + std::to_string(quantity) + " 已受理";
            } else {
                summary = sku + " 库存服务超时";
            }
            actor_runtime::ActorSystem::instance().send(client, OrderResult, summary, correlation);
            return {};
        });
    }

    actor_runtime::ActorRef inventory_;
};

} // namespace

actor_runtime::ActorRef install(actor_runtime::ActorRef inventory) {
    return actor_runtime::ActorSystem::instance().registerActor(
        std::make_unique<OrderActor>(inventory));
}

} // namespace orders
