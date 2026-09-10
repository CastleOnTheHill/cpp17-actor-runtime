#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace actor_runtime;
using namespace std::chrono_literals;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void expectInvalid(const std::function<void()>& operation) {
    bool caught = false;
    try { operation(); } catch (const std::invalid_argument&) { caught = true; }
    require(caught, "expected invalid_argument");
}
void expectLogic(const std::function<void()>& operation) {
    bool caught = false;
    try { operation(); } catch (const std::logic_error&) { caught = true; }
    require(caught, "expected logic_error");
}
void expectRuntime(const std::function<void()>& operation) {
    bool caught = false;
    try { operation(); } catch (const std::runtime_error&) { caught = true; }
    require(caught, "expected runtime_error");
}

class ErrorActor final : public Actor {
public:
    ErrorActor(ActorSystem& s, std::atomic<int>& errors, std::atomic<int>& handled)
        : Actor(s), errors_(errors), handled_(handled) {}
    void invalidStart() { start([](CoroutineContext&) -> std::any { return {}; }); }
private:
    void onMessage(MessagePtr message) override {
        ++handled_;
        if (message->typeId == 1) throw std::runtime_error("handler failure");
        if (message->typeId == 2) start([](CoroutineContext&) -> std::any {
            return std::any_cast<int>(std::any(std::string("wrong payload")));
        });
        if (message->typeId == 3) start([this](CoroutineContext&) -> std::any {
            invalidStart(); // start 只能在 onMessage 中调用；协程必须使用 spawn。
            return {};
        });
    }
    void onCoroutineError(FiberId, std::exception_ptr error) override {
        require(static_cast<bool>(error), "missing root error");
        ++errors_;
        throw std::runtime_error("error hook failure");
    }
    std::atomic<int>& errors_;
    std::atomic<int>& handled_;
};
class DefaultErrorActor final : public Actor {
public:
    explicit DefaultErrorActor(ActorSystem& s) : Actor(s) {}
private:
    void onMessage(MessagePtr) override {
        start([](CoroutineContext&) -> std::any { throw std::runtime_error("default hook"); });
    }
};

class RetainMessageActor final : public Actor {
public:
    RetainMessageActor(ActorSystem& system, std::promise<std::vector<MessagePtr>>& done)
        : Actor(system), done_(done) {}
private:
    void onMessage(MessagePtr message) override {
        messages_.push_back(std::move(message));
        if (messages_.size() == 4) done_.set_value(std::move(messages_));
    }
    std::promise<std::vector<MessagePtr>>& done_;
    std::vector<MessagePtr> messages_;
};

void owning_send_entry_points() {
    for (const auto workers : {1U, 4U}) {
        std::promise<std::vector<MessagePtr>> done;
        auto received = done.get_future();
        ActorSystem system({workers});
        auto target = system.registerActor(std::make_unique<RetainMessageActor>(system, done));
        std::any small = 17;
        std::any large = std::string(4096, 'x');
        const auto first = target.send(10, small, 101);
        const auto second = system.send(target.id(), 11, large, 102);
        small = 99;
        large.reset();
        const auto third = target.send(12, std::any(std::string(4096, 'y')), 103);
        const auto fourth = system.send(target.id(), 13, std::any(23), 104);
        require(received.wait_for(3s) == std::future_status::ready, "owning payload send stalled");
        auto messages = received.get();
        system.stopActor(target.id()).get();
        system.shutdown();
        require(std::any_cast<int>(messages[0]->payload) == 17, "lvalue small payload was borrowed");
        require(std::any_cast<const std::string&>(messages[1]->payload) == std::string(4096, 'x'), "lvalue heap payload was borrowed");
        require(std::any_cast<const std::string&>(messages[2]->payload) == std::string(4096, 'y'), "rvalue heap payload lost ownership");
        require(std::any_cast<int>(messages[3]->payload) == 23, "rvalue small payload lost ownership");
        const MessageId ids[] = {first, second, third, fourth};
        for (std::size_t i = 0; i < messages.size(); ++i) {
            require(ids[i] != 0 && messages[i]->id == ids[i], "send returned wrong message id");
            require(messages[i]->typeId == 10 + i && messages[i]->correlationId == 101 + i, "send metadata or FIFO changed");
        }
    }
}

class MetadataSink final : public Actor {
public:
    MetadataSink(ActorSystem& system, std::promise<MessagePtr>& result) : Actor(system), result_(result) {}
private:
    void onMessage(MessagePtr message) override { result_.set_value(std::move(message)); }
    std::promise<MessagePtr>& result_;
};
class MetadataSource final : public Actor {
public:
    MetadataSource(ActorSystem& system, ActorRef target) : Actor(system), target_(target) {}
private:
    void onMessage(MessagePtr message) override { target_.send(message->typeId, message->payload, message->id); }
    ActorRef target_;
};
void sender_metadata_remains_owned() {
    for (const auto workers : {1U, 4U}) {
        for (const bool named : {false, true}) {
            std::promise<MessagePtr> result;
            auto received = result.get_future();
            ActorSystem::Config config; config.workerCount = workers;
            if (named) config.processName = "metadata-test-" + std::to_string(::getpid());
            ActorSystem system(config);
            auto target = system.registerActor(std::make_unique<MetadataSink>(system, result));
            const std::string name = named ? "named-source-with-heap-owned-address" : "";
            auto source = system.registerActor(name, std::make_unique<MetadataSource>(system, target));
            const auto request = source.send(41, std::string("payload"));
            require(received.wait_for(3s) == std::future_status::ready, "metadata forwarding stalled");
            const auto message = received.get();
            system.stopActor(source.id()).get();
            system.shutdown();
            require(message->sender == source.id(), "sender id lost");
            require(message->senderAddress.process == config.processName, "sender process changed");
            require(message->senderAddress.actor == name, "sender name lost ownership");
            require(message->correlationId == request && message->typeId == 41, "forward metadata changed");
            require(std::any_cast<const std::string&>(message->payload) == "payload", "forward payload changed");
        }
    }
}

int main() {
    try {
        sender_metadata_remains_owned();
        owning_send_entry_points();
        expectInvalid([] { WaitSpec::any({}); });
        expectInvalid([] { WaitSpec::all({}); });
        expectInvalid([] { WaitSpec::allSettled({}); });
        expectInvalid([] { WaitSpec::child(0); });
        require(WaitSpec::message({1, 2}).kind() == WaitKind::Message, "message kind");
        require(WaitSpec::child(1).kind() == WaitKind::Child, "child kind");
        require(WaitSpec::timer(std::chrono::steady_clock::now()).kind() == WaitKind::Timer, "timer kind");
        require(WaitSpec::any({WaitSpec::child(1)}).kind() == WaitKind::Any, "any kind");
        require(WaitSpec::all({WaitSpec::child(1)}).kind() == WaitKind::All, "all kind");
        require(WaitSpec::allSettled({WaitSpec::child(1)}).kind() == WaitKind::AllSettled, "settled kind");
        WaitResult emptyResult;
        emptyResult.rethrowIfError();
        expectInvalid([] { ActorRef{}.send(1, {}); });
        CoroutineContext invalidContext(nullptr);
        require(!invalidContext.self() && !invalidContext.cancellationRequested(), "invalid context query");
        invalidContext.throwIfCancelled();
        expectLogic([&] { invalidContext.await(WaitSpec::message({1, 0})); });
        expectLogic([&] { invalidContext.spawn({}, ChildLifetime::CancelWithParent); });
        expectLogic([&] { invalidContext.cancel(1); });
        expectInvalid([] { ActorSystem::Config options; options.fiberStackSize = 1; ActorSystem system(options); });

        std::atomic<int> errors{0}, handled{0};
        ActorSystem system({2});
        ActorSystem other({1});
        expectInvalid([&] { system.registerActor(nullptr); });
        expectInvalid([&] { system.registerActor(std::make_unique<ErrorActor>(other, errors, handled)); });
        expectInvalid([&] { system.ref(9999).send(1, {}); });
        auto actor = std::make_unique<ErrorActor>(system, errors, handled);
        require(actor->id() == 0, "unregistered actor has an id");
        expectLogic([&] { actor->invalidStart(); });
        auto ref = system.registerActor(std::move(actor));
        require(ref.id() != 0 && static_cast<bool>(ref), "registered ref invalid");
        ref.send(1, {});
        ref.send(2, {});
        ref.send(3, {});
        ref.send(4, {});
        auto defaults = system.registerActor(std::make_unique<DefaultErrorActor>(system));
        defaults.send(1, {});
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while ((handled != 4 || errors != 2) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
        require(handled == 4 && errors == 2, "worker stopped after exception");
        system.shutdown();
        system.shutdown();
        require(!system.stats().acceptingMessages && system.stats().liveStacks == 0, "shutdown stats invalid");
        expectRuntime([&] { ref.send(1, {}); });
        expectRuntime([&] { system.registerActor(std::make_unique<ErrorActor>(system, errors, handled)); });
        { ActorSystem defaultSystem; require(defaultSystem.stats().workerCount > 0, "default worker config invalid"); }
        std::cout << "[PASS] public API validation, exception isolation, idempotent shutdown\n";
    } catch (const std::exception& e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
