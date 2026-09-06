#include "actor_runtime/wait.h"

#include "wait_internal.h"

#include <stdexcept>
#include <utility>

namespace actor_runtime {

WaitSpec::WaitSpec(std::shared_ptr<const Node> node) : node_(std::move(node)) {}

WaitSpec WaitSpec::message(MessageKey key) {
    auto node = std::make_shared<Node>();
    node->kind = WaitKind::Message;
    node->messageKey = key;
    return WaitSpec(std::move(node));
}

WaitSpec WaitSpec::child(FiberId id) {
    if (id == 0) throw std::invalid_argument("child fiber id must be non-zero");
    auto node = std::make_shared<Node>();
    node->kind = WaitKind::Child;
    node->childId = id;
    return WaitSpec(std::move(node));
}

WaitSpec WaitSpec::timer(std::chrono::steady_clock::time_point deadline) {
    auto node = std::make_shared<Node>();
    node->kind = WaitKind::Timer;
    node->deadline = deadline;
    return WaitSpec(std::move(node));
}

WaitSpec WaitSpec::any(std::vector<WaitSpec> children) {
    if (children.empty()) throw std::invalid_argument("Any requires at least one child");
    auto node = std::make_shared<Node>();
    node->kind = WaitKind::Any;
    node->children = std::move(children);
    return WaitSpec(std::move(node));
}

WaitSpec WaitSpec::all(std::vector<WaitSpec> children) {
    if (children.empty()) throw std::invalid_argument("All requires at least one child");
    auto node = std::make_shared<Node>();
    node->kind = WaitKind::All;
    node->children = std::move(children);
    return WaitSpec(std::move(node));
}

WaitKind WaitSpec::kind() const noexcept { return node_->kind; }

WaitSpec WaitSpec::task(TaskId id) {
    if (!id) throw std::invalid_argument("task id must be nonzero");
    auto node = std::make_shared<Node>();
    node->kind = WaitKind::Task;
    node->childId = id;
    return WaitSpec(std::move(node));
}

WaitSpec WaitSpec::allSettled(std::vector<WaitSpec> children) {
    if (children.empty()) throw std::invalid_argument("AllSettled requires at least one child");
    auto node = std::make_shared<Node>();
    node->kind = WaitKind::AllSettled;
    node->children = std::move(children);
    return WaitSpec(std::move(node));
}

void WaitResult::rethrowIfError() const {
    if (error) std::rethrow_exception(error);
}

} // namespace actor_runtime
