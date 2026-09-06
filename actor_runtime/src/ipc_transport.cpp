#include "ipc_transport.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <cstddef>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace actor_runtime {
namespace {
constexpr std::uint32_t Magic = 0x41525431;
struct Header {
    std::uint32_t magic, version, size, payloadSize;
    std::uint64_t type, id, correlation, senderId, targetId;
    char sourceProcess[64], sourceActor[64], targetActor[64];
};
struct Address { sockaddr_un value{}; socklen_t size = 0; };
Address address(const std::string& process) {
    validateName(process);
    Address result;
    result.value.sun_family = AF_UNIX;
    auto name = "actor-runtime." + std::to_string(::getuid()) + "." + process;
    if (name.size() + 1 > sizeof(result.value.sun_path)) throw std::invalid_argument("process socket name too long");
    std::memcpy(result.value.sun_path + 1, name.data(), name.size());
    result.size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    return result;
}
std::string field(const char* data) {
    const auto end = static_cast<const char*>(std::memchr(data, '\0', 64));
    if (!end) throw std::invalid_argument("unterminated wire name");
    return {data, end};
}
void fail(const char* action) { throw std::system_error(errno, std::generic_category(), action); }
}

void validateName(const std::string& name) {
    if (name.empty() || name.size() > 63) throw std::invalid_argument("name length must be 1..63");
    for (unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            throw std::invalid_argument("names may only contain ASCII letters, digits, dot, underscore, hyphen");
        }
    }
}

IpcTransport::IpcTransport(std::string process, Receiver receive, Logger log)
    : process_(std::move(process)), receive_(std::move(receive)), log_(std::move(log)) {
    auto bound = address(process_);
    try {
        fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_ < 0) fail("socket");
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&bound.value), bound.size) < 0) fail("bind process name");
        wake_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake_ < 0) fail("eventfd");
        thread_ = std::thread([this] { loop(); });
    } catch (...) {
        if (fd_ >= 0) ::close(fd_);
        if (wake_ >= 0) ::close(wake_);
        throw;
    }
}
IpcTransport::~IpcTransport() {
    stop();
    if (fd_ >= 0) ::close(fd_);
    if (wake_ >= 0) ::close(wake_);
}
void IpcTransport::stop() {
    if (stopping_.exchange(true)) return;
    std::uint64_t one = 1;
    (void)::write(wake_, &one, sizeof(one));
    if (thread_.joinable()) thread_.join();
}
void IpcTransport::send(const ActorAddress& target, ActorId incarnation, const Message& message) {
    if (stopping_) throw std::runtime_error("IPC is stopping");
    validateName(target.actor);
    auto destination = address(target.process);
    const auto& payload = std::any_cast<const Bytes&>(message.payload);
    if (payload.size() > MaxPayloadBytes) throw std::length_error("datagram payload exceeds 60 KiB");
    Header header{};
    header.magic = Magic; header.version = 1; header.size = sizeof(header);
    header.payloadSize = static_cast<std::uint32_t>(payload.size());
    header.type = message.typeId; header.id = message.id; header.correlation = message.correlationId;
    header.senderId = message.sender; header.targetId = incarnation;
    std::memcpy(header.sourceProcess, process_.data(), process_.size());
    if (!message.senderAddress.actor.empty()) validateName(message.senderAddress.actor);
    std::memcpy(header.sourceActor, message.senderAddress.actor.data(), message.senderAddress.actor.size());
    std::memcpy(header.targetActor, target.actor.data(), target.actor.size());
    iovec vectors[2]{{&header, sizeof(header)}, {const_cast<std::uint8_t*>(payload.data()), payload.size()}};
    msghdr envelope{};
    envelope.msg_name = &destination.value; envelope.msg_namelen = destination.size;
    envelope.msg_iov = vectors; envelope.msg_iovlen = 2;
    auto count = ::sendmsg(fd_, &envelope, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count < 0) fail("sendmsg");
    if (static_cast<std::size_t>(count) != sizeof(header) + payload.size()) throw std::runtime_error("incomplete datagram");
    log_("ipc.send");
}
void IpcTransport::loop() {
    std::array<std::uint8_t, sizeof(Header) + MaxPayloadBytes> buffer{};
    pollfd descriptors[2]{{fd_, POLLIN, 0}, {wake_, POLLIN, 0}};
    while (!stopping_) {
        auto ready = ::poll(descriptors, 2, -1);
        if (ready < 0) { if (errno == EINTR) continue; log_("ipc.poll_error"); return; }
        if (descriptors[1].revents) return;
        if (!(descriptors[0].revents & POLLIN)) continue;
        sockaddr_un source{};
        iovec vector{buffer.data(), buffer.size()};
        msghdr envelope{};
        envelope.msg_name = &source; envelope.msg_namelen = sizeof(source);
        envelope.msg_iov = &vector; envelope.msg_iovlen = 1;
        auto count = ::recvmsg(fd_, &envelope, MSG_DONTWAIT);
        if (count < 0) { if (errno != EAGAIN && errno != EINTR) log_("ipc.receive_error"); continue; }
        try {
            if ((envelope.msg_flags & MSG_TRUNC) || static_cast<std::size_t>(count) < sizeof(Header))
                throw std::invalid_argument("truncated datagram");
            Header header{};
            std::memcpy(&header, buffer.data(), sizeof(header));
            if (header.magic != Magic || header.version != 1 || header.size != sizeof(Header) || !header.id ||
                header.payloadSize > MaxPayloadBytes || sizeof(header) + header.payloadSize != static_cast<std::size_t>(count))
                throw std::invalid_argument("invalid header");
            auto process = field(header.sourceProcess);
            auto sourceActor = field(header.sourceActor);
            auto targetActor = field(header.targetActor);
            auto expected = address(process);
            validateName(targetActor);
            if (!sourceActor.empty()) validateName(sourceActor);
            if (envelope.msg_namelen != expected.size ||
                std::memcmp(&source, &expected.value, expected.size) != 0)
                throw std::invalid_argument("sender address mismatch");
            Bytes payload(header.payloadSize);
            if (!payload.empty()) std::memcpy(payload.data(), buffer.data() + sizeof(header), payload.size());
            auto message = std::make_shared<const Message>(Message{header.type, header.id, header.correlation,
                header.senderId, std::move(payload), {std::move(process), std::move(sourceActor)}});
            receive_(targetActor, header.targetId, std::move(message));
            log_("ipc.receive");
        } catch (...) { log_("ipc.reject"); }
    }
}
}
