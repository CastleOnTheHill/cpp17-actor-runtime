#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

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

int main() {
    try {
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
        // 进程级单例契约：未启动时不可用；非法配置不会留下半启动状态。
        require(!ActorSystem::isRunning(), "system reported running before start");
        expectLogic([] { ActorSystem::instance(); });
        expectInvalid([] {
            ActorSystem::Config options;
            options.fiberStackSize = 1;
            ActorSystem::start(options);
        });
        require(!ActorSystem::isRunning(), "failed start left the system running");

        std::atomic<int> errors{0}, handled{0};
        ActorSystem& system = ActorSystem::start({2});
        ActorSystem& other = ActorSystem::instance();
        require(&other == &system, "instance() returned a different system");
        require(ActorSystem::isRunning() && system.stats().workerCount == 2, "start contract invalid");
        expectLogic([] { ActorSystem::start({1}); });
        expectInvalid([&] { system.registerActor(nullptr); });
        expectInvalid([&] { system.ref(9999).send(1, {}); });
        auto actor = std::make_unique<ErrorActor>(system, errors, handled);
        require(actor->id() == 0, "unregistered actor has an id");
        expectLogic([&] { actor->invalidStart(); });
        auto stale = std::make_unique<ErrorActor>(system, errors, handled);
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
        ActorSystem::stop();
        ActorSystem::stop();
        require(!ActorSystem::isRunning() && !system.stats().acceptingMessages &&
            system.stats().liveStacks == 0, "shutdown stats invalid");
        expectRuntime([&] { ref.send(1, {}); });
        expectRuntime([&] { system.registerActor(std::make_unique<ErrorActor>(system, errors, handled)); });
        expectLogic([] { ActorSystem::instance(); });
        {
            ActorSystem& restarted = ActorSystem::start();
            require(restarted.stats().workerCount > 0, "default worker config invalid");
            // 重启前构造的 Actor 属于上一代，不能注册到新实例上。
            expectInvalid([&] { restarted.registerActor(std::move(stale)); });
            auto fresh = restarted.registerActor(std::make_unique<ErrorActor>(restarted, errors, handled));
            require(static_cast<bool>(fresh) && restarted.stats().acceptingMessages, "restart failed");
            ActorSystem::stop();
        }
        std::cout << "[PASS] singleton lifecycle, public API validation, exception isolation, idempotent shutdown\n";
    } catch (const std::exception& e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
