#pragma once

#include "actor_runtime/message.h"

#include <any>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <vector>

namespace actor_runtime {

struct WaitSpecAccess;

enum class WaitKind { Message, Child, Timer, Any, All, AllSettled, Task };

struct MessageKey {
    TypeId typeId;
    MessageId correlationId;
};

class WaitSpec {
public:
    static WaitSpec message(MessageKey key);
    static WaitSpec child(FiberId id);
    static WaitSpec task(TaskId id);
    static WaitSpec timer(std::chrono::steady_clock::time_point deadline);
    static WaitSpec any(std::vector<WaitSpec> children);
    static WaitSpec all(std::vector<WaitSpec> children);
    static WaitSpec allSettled(std::vector<WaitSpec> children);

    WaitKind kind() const noexcept;

private:
    struct Node;
    explicit WaitSpec(std::shared_ptr<const Node> node);
    std::shared_ptr<const Node> node_;

    friend class CoroutineContext;
    friend struct WaitSpecAccess;
};

enum class WakeReason { Message, ChildCompleted, Timeout, Cancelled, Error, Settled, TaskCompleted };

struct WaitResult {
    WakeReason reason = WakeReason::Error;
    std::size_t branchIndex = 0;
    std::any value;
    std::vector<WaitResult> children;
    std::exception_ptr error;

    template<class T>
    const T& as() const {
        return std::any_cast<const T&>(value);
    }

    void rethrowIfError() const;
};

} // namespace actor_runtime
