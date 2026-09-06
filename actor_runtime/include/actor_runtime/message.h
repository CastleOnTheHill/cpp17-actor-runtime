#pragma once

#include <any>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace actor_runtime {

using ActorId = std::uint64_t;
using MessageId = std::uint64_t;
using TypeId = std::uint64_t;
using FiberId = std::uint64_t;
using TaskId = std::uint64_t;

constexpr MessageId NoCorrelation = 0;

struct ActorAddress final {
    std::string process;
    std::string actor;
};
using Bytes = std::vector<std::uint8_t>;

struct Message final {
    TypeId typeId;
    MessageId id;
    MessageId correlationId;
    ActorId sender;
    std::any payload;
    ActorAddress senderAddress;
};

using MessagePtr = std::shared_ptr<const Message>;

} // namespace actor_runtime
