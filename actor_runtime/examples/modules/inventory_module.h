#pragma once

#include "actor_runtime/actor_ref.h"

// 库存子模块：不接收 ActorSystem 参数，只通过全局实例工作。
namespace inventory {

constexpr actor_runtime::TypeId QueryStock = 100;
constexpr actor_runtime::TypeId StockReply = 101;

// 注册本模块的 Actor 并返回可用的引用。要求活动中的 ActorSystem。
actor_runtime::ActorRef install();

} // namespace inventory
