#pragma once

#include "actor_runtime/wait.h"

namespace actor_runtime {

struct WaitSpec::Node final {
    WaitKind kind;
    MessageKey messageKey{};
    FiberId childId = 0;
    std::chrono::steady_clock::time_point deadline{};
    std::vector<WaitSpec> children;
};

struct WaitSpecAccess final {
    static const std::shared_ptr<const WaitSpec::Node>& node(const WaitSpec& spec) {
        return spec.node_;
    }
};

} // namespace actor_runtime
