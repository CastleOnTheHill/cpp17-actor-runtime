#pragma once

#include "actor_runtime/actor_ref.h"

// 订单子模块：只依赖全局 ActorSystem 和调用方传入的库存引用。
namespace orders {

constexpr actor_runtime::TypeId PlaceOrder = 200;
constexpr actor_runtime::TypeId OrderResult = 201;

// 注册本模块的 Actor 并返回可用的引用。要求活动中的 ActorSystem。
actor_runtime::ActorRef install(actor_runtime::ActorRef inventory);

} // namespace orders
