#pragma once
#include "actor_runtime/message.h"
#include <atomic>
#include <functional>
#include <thread>

namespace actor_runtime {
constexpr std::size_t MaxPayloadBytes = 60U * 1024U;
void validateName(const std::string& name);

// 每个进程名独占一个 Linux abstract Unix 数据报地址，无目录服务或中转。
class IpcTransport final {
public:
    using Receiver = std::function<void(const std::string&, ActorId, MessagePtr)>;
    using Logger = std::function<void(const char*)>;
    IpcTransport(std::string process, Receiver receive, Logger log);
    ~IpcTransport();
    void send(const ActorAddress& target, ActorId incarnation, const Message& message);
    void stop();
private:
    void loop();
    std::string process_;
    Receiver receive_;
    Logger log_;
    int fd_ = -1;
    int wake_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
}
