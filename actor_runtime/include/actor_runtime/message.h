#pragma once

#include <any>
#include <cstddef>
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

enum class SendStatus { Accepted, Full, Stopped, MessageTooLarge };
struct SendResult final {
    SendStatus status = SendStatus::Stopped;
    MessageId id = 0; // Nonzero only when admitted; not a delivery acknowledgement.
};

// Conservative accounting allowance, not a measurement of allocator/RSS usage.
constexpr std::size_t MailboxEnvelopeBytes = 256;

struct Message final {
    TypeId typeId;
    MessageId id;
    MessageId correlationId;
    ActorId sender;
    std::any payload;
    ActorAddress senderAddress;
    // Assigned once by local admission before immutable publication.
    std::size_t estimatedMailboxBytes = 0;
};

using MessagePtr = std::shared_ptr<const Message>;

} // namespace actor_runtime
