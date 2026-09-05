#pragma once

#include <any>
#include <cstdint>
#include <memory>

namespace actor_runtime {

using ActorId = std::uint64_t;
using MessageId = std::uint64_t;
using TypeId = std::uint64_t;
using FiberId = std::uint64_t;

constexpr MessageId NoCorrelation = 0;

struct Message final {
    TypeId typeId;
    MessageId id;
    MessageId correlationId;
    ActorId sender;
    std::any payload;
};

using MessagePtr = std::shared_ptr<const Message>;

} // namespace actor_runtime
