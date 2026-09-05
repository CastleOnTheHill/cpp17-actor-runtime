#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Predicate>
void eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(predicate(), "condition did not become true");
}

class CountingActor final : public actor_runtime::Actor {
public:
    CountingActor(actor_runtime::ActorSystem& system, std::atomic<int>& count)
        : Actor(system), count_(count) {}

private:
    void onMessage(actor_runtime::MessagePtr) override {
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<int>& count_;
};

void basic_post_calls_on_message_exactly_once() {
    actor_runtime::ActorSystem system({2});
    std::atomic<int> count{0};
    auto ref = system.registerActor(std::make_unique<CountingActor>(system, count));
    ref.send(1, 42);
    eventually([&] { return count.load(std::memory_order_relaxed) == 1; });
    std::this_thread::sleep_for(20ms);
    require(count.load(std::memory_order_relaxed) == 1, "message was dispatched more than once");
    system.shutdown();
}

class TimerActor final : public actor_runtime::Actor {
public:
    TimerActor(actor_runtime::ActorSystem& system, std::atomic<int>& result)
        : Actor(system), result_(result) {}

private:
    static std::any nestedAwait(actor_runtime::CoroutineContext& context, std::atomic<int>& result) {
        int local = 41;
        const auto wake = context.await(actor_runtime::WaitSpec::timer(
            std::chrono::steady_clock::now() + 10ms));
        require(wake.reason == actor_runtime::WakeReason::Timeout, "timer returned wrong wake reason");
        result.store(local + 1, std::memory_order_release);
        return {};
    }

    void onMessage(actor_runtime::MessagePtr message) override {
        if (message->typeId == 1) {
            start([this](actor_runtime::CoroutineContext& context) {
                return nestedAwait(context, result_);
            });
        }
    }

    std::atomic<int>& result_;
};

void ucontext_timer_preserves_nested_stack_locals() {
    actor_runtime::ActorSystem system({2});
    std::atomic<int> result{0};
    auto ref = system.registerActor(std::make_unique<TimerActor>(system, result));
    ref.send(1, {});
    eventually([&] { return result.load(std::memory_order_acquire) == 42; });
    system.shutdown();
    const auto stats = system.stats();
    require(stats.liveFibers == 0, "fiber leaked after shutdown");
    require(stats.liveStacks == 0, "fiber stack leaked after shutdown");
}

class MessageWaitActor final : public actor_runtime::Actor {
public:
    MessageWaitActor(actor_runtime::ActorSystem& system, std::atomic<int>& armed,
        std::atomic<int>& received, std::atomic<int>& observed)
        : Actor(system), armed_(armed), received_(received), observed_(observed) {}

private:
    void onMessage(actor_runtime::MessagePtr message) override {
        observed_.fetch_add(1, std::memory_order_relaxed);
        if (message->typeId != 10) return;
        const auto requestId = message->id;
        start([this, requestId](actor_runtime::CoroutineContext& context) -> std::any {
            armed_.store(1, std::memory_order_release);
            const auto result = context.await(actor_runtime::WaitSpec::message({20, requestId}));
            require(result.reason == actor_runtime::WakeReason::Message, "message wait returned wrong reason");
            const auto& response = result.as<actor_runtime::MessagePtr>();
            received_.store(std::any_cast<int>(response->payload), std::memory_order_release);
            return {};
        });
    }

    std::atomic<int>& armed_;
    std::atomic<int>& received_;
    std::atomic<int>& observed_;
};

void message_wait_matches_type_and_correlation() {
    actor_runtime::ActorSystem system({3});
    std::atomic<int> armed{0};
    std::atomic<int> received{0};
    std::atomic<int> observed{0};
    auto ref = system.registerActor(std::make_unique<MessageWaitActor>(system, armed, received, observed));
    const auto requestId = ref.send(10, {});
    eventually([&] { return armed.load(std::memory_order_acquire) == 1; });
    ref.send(21, 11, requestId);
    ref.send(20, 12, requestId + 1);
    std::this_thread::sleep_for(20ms);
    require(received.load(std::memory_order_acquire) == 0, "wrong message woke the fiber");
    ref.send(20, 77, requestId);
    eventually([&] { return received.load(std::memory_order_acquire) == 77; });
    eventually([&] { return observed.load(std::memory_order_relaxed) == 4; });
    system.shutdown();
}

class BroadcastActor final : public actor_runtime::Actor {
public:
    BroadcastActor(actor_runtime::ActorSystem& system, std::atomic<int>& armed,
        std::atomic<int>& done, std::mutex& mutex, std::vector<const actor_runtime::Message*>& addresses)
        : Actor(system), armed_(armed), done_(done), mutex_(mutex), addresses_(addresses) {}

private:
    void onMessage(actor_runtime::MessagePtr message) override {
        if (message->typeId != 30) return;
        for (int index = 0; index < 2; ++index) {
            start([this](actor_runtime::CoroutineContext& context) -> std::any {
                armed_.fetch_add(1, std::memory_order_release);
                const auto result = context.await(actor_runtime::WaitSpec::message({31, 0}));
                const auto& message = result.as<actor_runtime::MessagePtr>();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    addresses_.push_back(message.get());
                }
                done_.fetch_add(1, std::memory_order_release);
                return {};
            });
        }
    }

    std::atomic<int>& armed_;
    std::atomic<int>& done_;
    std::mutex& mutex_;
    std::vector<const actor_runtime::Message*>& addresses_;
};

void message_wait_is_broadcast() {
    actor_runtime::ActorSystem system({2});
    std::atomic<int> armed{0};
    std::atomic<int> done{0};
    std::mutex mutex;
    std::vector<const actor_runtime::Message*> addresses;
    auto ref = system.registerActor(std::make_unique<BroadcastActor>(system, armed, done, mutex, addresses));
    ref.send(30, {});
    eventually([&] { return armed.load(std::memory_order_acquire) == 2; });
    ref.send(31, 99);
    eventually([&] { return done.load(std::memory_order_acquire) == 2; });
    {
        std::lock_guard<std::mutex> lock(mutex);
        require(addresses.size() == 2 && addresses[0] == addresses[1],
            "waiters did not observe the same immutable Message instance");
    }
    system.shutdown();
}

class DispatchSnapshotActor final : public actor_runtime::Actor {
public:
    DispatchSnapshotActor(actor_runtime::ActorSystem& system, std::atomic<int>& armed,
        std::atomic<int>& oldDone, std::atomic<int>& newDone)
        : Actor(system), armed_(armed), oldDone_(oldDone), newDone_(newDone) {}

private:
    void makeWaiter(std::atomic<int>& done) {
        auto* donePtr = &done;
        start([this, donePtr](actor_runtime::CoroutineContext& context) -> std::any {
            armed_.fetch_add(1, std::memory_order_release);
            context.await(actor_runtime::WaitSpec::message({41, 0}));
            donePtr->fetch_add(1, std::memory_order_release);
            return {};
        });
    }

    void onMessage(actor_runtime::MessagePtr message) override {
        if (message->typeId == 40) makeWaiter(oldDone_);
        if (message->typeId == 41 && !createdNew_) {
            createdNew_ = true;
            makeWaiter(newDone_);
        }
    }

    bool createdNew_ = false;
    std::atomic<int>& armed_;
    std::atomic<int>& oldDone_;
    std::atomic<int>& newDone_;
};

void dispatch_snapshot_excludes_new_waiters() {
    actor_runtime::ActorSystem system({3});
    std::atomic<int> armed{0};
    std::atomic<int> oldDone{0};
    std::atomic<int> newDone{0};
    auto ref = system.registerActor(
        std::make_unique<DispatchSnapshotActor>(system, armed, oldDone, newDone));
    ref.send(40, {});
    eventually([&] { return armed.load(std::memory_order_acquire) == 1; });
    ref.send(41, {});
    eventually([&] { return oldDone.load(std::memory_order_acquire) == 1 &&
        armed.load(std::memory_order_acquire) == 2; });
    require(newDone.load(std::memory_order_acquire) == 0,
        "waiter created during dispatch observed the current message");
    ref.send(41, {});
    eventually([&] { return newDone.load(std::memory_order_acquire) == 1; });
    system.shutdown();
}

class ChildActor final : public actor_runtime::Actor {
public:
    ChildActor(actor_runtime::ActorSystem& system, std::atomic<int>& value,
        std::atomic<int>& errorSeen, std::atomic<int>& messages)
        : Actor(system), value_(value), errorSeen_(errorSeen), messages_(messages) {}

private:
    void onMessage(actor_runtime::MessagePtr message) override {
        messages_.fetch_add(1, std::memory_order_relaxed);
        if (message->typeId == 70) {
            start([this](actor_runtime::CoroutineContext& context) -> std::any {
                const auto child = context.spawn(
                    [](actor_runtime::CoroutineContext&) -> std::any { return 123; },
                    actor_runtime::ChildLifetime::CancelWithParent);
                const auto result = context.await(actor_runtime::WaitSpec::child(child));
                value_.store(result.as<int>(), std::memory_order_release);
                return {};
            });
        }
        if (message->typeId == 71) {
            start([this](actor_runtime::CoroutineContext& context) -> std::any {
                const auto child = context.spawn(
                    [](actor_runtime::CoroutineContext&) -> std::any {
                        throw std::runtime_error("child boom");
                    },
                    actor_runtime::ChildLifetime::CancelWithParent);
                const auto result = context.await(actor_runtime::WaitSpec::child(child));
                require(result.reason == actor_runtime::WakeReason::Error, "child error reason lost");
                try {
                    result.rethrowIfError();
                } catch (const std::runtime_error&) {
                    errorSeen_.store(1, std::memory_order_release);
                }
                return {};
            });
        }
    }

    std::atomic<int>& value_;
    std::atomic<int>& errorSeen_;
    std::atomic<int>& messages_;
};

void child_join_and_error_propagation() {
    actor_runtime::ActorSystem system({2});
    std::atomic<int> value{0};
    std::atomic<int> errorSeen{0};
    std::atomic<int> messages{0};
    auto ref = system.registerActor(std::make_unique<ChildActor>(system, value, errorSeen, messages));
    ref.send(70, {});
    eventually([&] { return value.load(std::memory_order_acquire) == 123; });
    ref.send(71, {});
    eventually([&] { return errorSeen.load(std::memory_order_acquire) == 1; });
    ref.send(72, {});
    eventually([&] { return messages.load(std::memory_order_relaxed) == 3; });
    system.shutdown();
}

class CompositeActor final : public actor_runtime::Actor {
public:
    CompositeActor(actor_runtime::ActorSystem& system, std::atomic<int>& armed,
        std::atomic<int>& anyResumes, std::atomic<int>& allDone, std::atomic<int>& nestedDone)
        : Actor(system), armed_(armed), anyResumes_(anyResumes), allDone_(allDone), nestedDone_(nestedDone) {}

private:
    void onMessage(actor_runtime::MessagePtr message) override {
        if (message->typeId == 80) {
            start([this](actor_runtime::CoroutineContext& context) -> std::any {
                const auto child = context.spawn([](actor_runtime::CoroutineContext& childContext) -> std::any {
                    childContext.await(actor_runtime::WaitSpec::timer(
                        std::chrono::steady_clock::now() + 50ms));
                    return 8;
                }, actor_runtime::ChildLifetime::CancelWithParent);
                armed_.fetch_add(1, std::memory_order_release);
                const auto result = context.await(actor_runtime::WaitSpec::any({
                    actor_runtime::WaitSpec::message({81, 0}),
                    actor_runtime::WaitSpec::child(child),
                    actor_runtime::WaitSpec::timer(std::chrono::steady_clock::now() + 100ms)}));
                require(result.branchIndex == 0 && result.reason == actor_runtime::WakeReason::Message,
                    "Any chose the wrong branch");
                anyResumes_.fetch_add(1, std::memory_order_release);
                return {};
            });
        }
        if (message->typeId == 82) {
            start([this](actor_runtime::CoroutineContext& context) -> std::any {
                const auto child = context.spawn([](actor_runtime::CoroutineContext& childContext) -> std::any {
                    childContext.await(actor_runtime::WaitSpec::timer(
                        std::chrono::steady_clock::now() + 30ms));
                    return 17;
                }, actor_runtime::ChildLifetime::CancelWithParent);
                armed_.fetch_add(1, std::memory_order_release);
                const auto result = context.await(actor_runtime::WaitSpec::all({
                    actor_runtime::WaitSpec::timer(std::chrono::steady_clock::now() + 10ms),
                    actor_runtime::WaitSpec::message({83, 0}),
                    actor_runtime::WaitSpec::child(child)}));
                require(result.children.size() == 3, "All lost child results");
                require(result.children[0].reason == actor_runtime::WakeReason::Timeout,
                    "All result 0 is out of specification order");
                require(result.children[1].reason == actor_runtime::WakeReason::Message,
                    "All result 1 is out of specification order");
                require(result.children[2].as<int>() == 17,
                    "All result 2 lost the child value");
                allDone_.store(1, std::memory_order_release);
                return {};
            });
        }
        if (message->typeId == 84) {
            start([this](actor_runtime::CoroutineContext& context) -> std::any {
                const auto child = context.spawn([](actor_runtime::CoroutineContext& childContext) -> std::any {
                    childContext.await(actor_runtime::WaitSpec::timer(
                        std::chrono::steady_clock::now() + 50ms));
                    return 1;
                }, actor_runtime::ChildLifetime::CancelWithParent);
                armed_.fetch_add(1, std::memory_order_release);
                const auto result = context.await(actor_runtime::WaitSpec::all({
                    actor_runtime::WaitSpec::message({85, 0}),
                    actor_runtime::WaitSpec::any({
                        actor_runtime::WaitSpec::child(child),
                        actor_runtime::WaitSpec::timer(std::chrono::steady_clock::now() + 10ms)})}));
                require(result.children.size() == 2, "nested All shape is wrong");
                require(result.children[1].branchIndex == 1 &&
                    result.children[1].reason == actor_runtime::WakeReason::Timeout,
                    "nested Any selected the wrong direct branch");
                nestedDone_.store(1, std::memory_order_release);
                return {};
            });
        }
    }

    std::atomic<int>& armed_;
    std::atomic<int>& anyResumes_;
    std::atomic<int>& allDone_;
    std::atomic<int>& nestedDone_;
};

void any_all_and_nested_waits() {
    actor_runtime::ActorSystem system({3});
    std::atomic<int> armed{0};
    std::atomic<int> anyResumes{0};
    std::atomic<int> allDone{0};
    std::atomic<int> nestedDone{0};
    auto ref = system.registerActor(
        std::make_unique<CompositeActor>(system, armed, anyResumes, allDone, nestedDone));

    ref.send(80, {});
    eventually([&] { return armed.load(std::memory_order_acquire) >= 1; });
    ref.send(81, {});
    eventually([&] { return anyResumes.load(std::memory_order_acquire) == 1; });
    std::this_thread::sleep_for(130ms);
    require(anyResumes.load(std::memory_order_acquire) == 1, "a losing Any branch resumed twice");

    ref.send(82, {});
    eventually([&] { return armed.load(std::memory_order_acquire) >= 2; });
    std::this_thread::sleep_for(15ms);
    ref.send(83, {});
    eventually([&] { return allDone.load(std::memory_order_acquire) == 1; });

    ref.send(84, {});
    eventually([&] { return armed.load(std::memory_order_acquire) >= 3; });
    std::this_thread::sleep_for(15ms);
    ref.send(85, {});
    eventually([&] { return nestedDone.load(std::memory_order_acquire) == 1; });
    system.shutdown();
}

class RaceActor final : public actor_runtime::Actor {
public:
    RaceActor(actor_runtime::ActorSystem& system, std::atomic<int>& armed, std::atomic<int>& resumes)
        : Actor(system), armed_(armed), resumes_(resumes) {}

private:
    void onMessage(actor_runtime::MessagePtr message) override {
        if (message->typeId != 90) return;
        const auto correlation = message->id;
        start([this, correlation](actor_runtime::CoroutineContext& context) -> std::any {
            armed_.fetch_add(1, std::memory_order_release);
            context.await(actor_runtime::WaitSpec::any({
                actor_runtime::WaitSpec::message({91, correlation}),
                actor_runtime::WaitSpec::timer(std::chrono::steady_clock::now() + 1ms)}));
            resumes_.fetch_add(1, std::memory_order_release);
            return {};
        });
    }

    std::atomic<int>& armed_;
    std::atomic<int>& resumes_;
};

void response_timeout_race_never_double_resumes() {
    actor_runtime::ActorSystem system({4});
    std::atomic<int> armed{0};
    std::atomic<int> resumes{0};
    auto ref = system.registerActor(std::make_unique<RaceActor>(system, armed, resumes));
    constexpr int iterations = 120;
    for (int index = 0; index < iterations; ++index) {
        const auto request = ref.send(90, {});
        eventually([&] { return armed.load(std::memory_order_acquire) >= index + 1; });
        std::thread sender([&, request, index] {
            if ((index % 3) != 0) std::this_thread::sleep_for(1ms);
            ref.send(91, index, request);
        });
        sender.join();
        eventually([&] { return resumes.load(std::memory_order_acquire) >= index + 1; });
        require(resumes.load(std::memory_order_acquire) == index + 1,
            "one Any registration resumed more than once");
    }
    std::this_thread::sleep_for(20ms);
    require(resumes.load(std::memory_order_acquire) == iterations,
        "late response or timer resumed an old registration");
    system.shutdown();
}

class LifetimeActor final : public actor_runtime::Actor {
public:
    LifetimeActor(actor_runtime::ActorSystem& system, std::atomic<int>& armed,
        std::atomic<int>& cancelled, std::atomic<int>& detachedDone,
        std::atomic<int>& shutdownCancelled)
        : Actor(system), armed_(armed), cancelled_(cancelled), detachedDone_(detachedDone),
          shutdownCancelled_(shutdownCancelled) {}

private:
    void onMessage(actor_runtime::MessagePtr message) override {
        if (message->typeId == 100) {
            start([this](actor_runtime::CoroutineContext& context) -> std::any {
                context.spawn([this](actor_runtime::CoroutineContext& child) -> std::any {
                    armed_.fetch_add(1, std::memory_order_release);
                    const auto result = child.await(actor_runtime::WaitSpec::timer(
                        std::chrono::steady_clock::now() + 500ms));
                    if (result.reason == actor_runtime::WakeReason::Cancelled) {
                        cancelled_.store(1, std::memory_order_release);
                    }
                    return {};
                }, actor_runtime::ChildLifetime::CancelWithParent);
                context.spawn([this](actor_runtime::CoroutineContext& child) -> std::any {
                    armed_.fetch_add(1, std::memory_order_release);
                    const auto result = child.await(actor_runtime::WaitSpec::timer(
                        std::chrono::steady_clock::now() + 30ms));
                    if (result.reason == actor_runtime::WakeReason::Timeout) {
                        detachedDone_.store(1, std::memory_order_release);
                    }
                    return {};
                }, actor_runtime::ChildLifetime::DetachedFromParent);
                context.await(actor_runtime::WaitSpec::timer(
                    std::chrono::steady_clock::now() + 5ms));
                return {};
            });
        }
        if (message->typeId == 101) {
            start([this](actor_runtime::CoroutineContext& context) -> std::any {
                context.spawn([this](actor_runtime::CoroutineContext& child) -> std::any {
                    armed_.fetch_add(1, std::memory_order_release);
                    const auto result = child.await(actor_runtime::WaitSpec::timer(
                        std::chrono::steady_clock::now() + 5s));
                    if (result.reason == actor_runtime::WakeReason::Cancelled) {
                        shutdownCancelled_.store(1, std::memory_order_release);
                    }
                    return {};
                }, actor_runtime::ChildLifetime::DetachedFromParent);
                context.await(actor_runtime::WaitSpec::timer(
                    std::chrono::steady_clock::now() + 5ms));
                return {};
            });
        }
    }

    std::atomic<int>& armed_;
    std::atomic<int>& cancelled_;
    std::atomic<int>& detachedDone_;
    std::atomic<int>& shutdownCancelled_;
};

void child_lifetime_and_shutdown_cancellation() {
    actor_runtime::ActorSystem system({3});
    std::atomic<int> armed{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> detachedDone{0};
    std::atomic<int> shutdownCancelled{0};
    auto ref = system.registerActor(std::make_unique<LifetimeActor>(
        system, armed, cancelled, detachedDone, shutdownCancelled));
    ref.send(100, {});
    eventually([&] { return armed.load(std::memory_order_acquire) >= 2; });
    eventually([&] { return cancelled.load(std::memory_order_acquire) == 1; });
    eventually([&] { return detachedDone.load(std::memory_order_acquire) == 1; });

    ref.send(101, {});
    eventually([&] { return armed.load(std::memory_order_acquire) >= 3; });
    std::this_thread::sleep_for(10ms);
    system.shutdown();
    require(shutdownCancelled.load(std::memory_order_acquire) == 1,
        "detached child survived Actor/System shutdown");
    require(system.stats().liveFibers == 0 && system.stats().liveStacks == 0,
        "shutdown did not reclaim all fibers and stacks");
}

class ConcurrencyActor final : public actor_runtime::Actor {
public:
    ConcurrencyActor(actor_runtime::ActorSystem& system, std::atomic<int>& count,
        std::atomic<int>& maxActive, std::mutex& mutex, std::set<std::thread::id>& threads,
        std::chrono::microseconds delay)
        : Actor(system), count_(count), maxActive_(maxActive), mutex_(mutex), threads_(threads), delay_(delay) {}

private:
    void onMessage(actor_runtime::MessagePtr) override {
        const int now = active_.fetch_add(1, std::memory_order_acq_rel) + 1;
        int previous = maxActive_.load(std::memory_order_relaxed);
        while (previous < now && !maxActive_.compare_exchange_weak(previous, now)) {}
        {
            std::lock_guard<std::mutex> lock(mutex_);
            threads_.insert(std::this_thread::get_id());
        }
        std::this_thread::sleep_for(delay_);
        count_.fetch_add(1, std::memory_order_release);
        active_.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::atomic<int> active_{0};
    std::atomic<int>& count_;
    std::atomic<int>& maxActive_;
    std::mutex& mutex_;
    std::set<std::thread::id>& threads_;
    std::chrono::microseconds delay_;
};

void actor_is_serial_but_migrates_across_workers() {
    actor_runtime::ActorSystem system({4});
    std::atomic<int> count{0};
    std::atomic<int> maxActive{0};
    std::mutex mutex;
    std::set<std::thread::id> threads;
    auto ref = system.registerActor(std::make_unique<ConcurrencyActor>(
        system, count, maxActive, mutex, threads, 100us));
    constexpr int clients = 4;
    constexpr int perClient = 100;
    std::vector<std::thread> senders;
    for (int client = 0; client < clients; ++client) {
        senders.emplace_back([ref] {
            for (int index = 0; index < perClient; ++index) ref.send(1, index);
        });
    }
    for (auto& sender : senders) sender.join();
    eventually([&] { return count.load(std::memory_order_acquire) == clients * perClient; });
    require(maxActive.load(std::memory_order_acquire) == 1,
        "two workers executed the same actor concurrently");
    {
        std::lock_guard<std::mutex> lock(mutex);
        require(threads.size() > 1, "actor never migrated between worker threads");
    }
    system.shutdown();
}

class OwnerActor final : public actor_runtime::Actor {
public:
    OwnerActor(actor_runtime::ActorSystem& system, std::atomic<int>& sameOwner)
        : Actor(system), sameOwner_(sameOwner) {}

private:
    void onMessage(actor_runtime::MessagePtr message) override {
        if (message->typeId != 110) return;
        start([this](actor_runtime::CoroutineContext& context) -> std::any {
            const auto before = std::this_thread::get_id();
            context.await(actor_runtime::WaitSpec::timer(
                std::chrono::steady_clock::now() + 2ms));
            const auto after = std::this_thread::get_id();
            if (before == after) sameOwner_.fetch_add(1, std::memory_order_release);
            return {};
        });
    }

    std::atomic<int>& sameOwner_;
};

void fiber_stays_on_owner_worker_and_stack_is_reclaimed() {
    actor_runtime::ActorSystem system({4});
    std::atomic<int> sameOwner{0};
    auto ref = system.registerActor(std::make_unique<OwnerActor>(system, sameOwner));
    constexpr int fibers = 80;
    for (int index = 0; index < fibers; ++index) ref.send(110, index);
    eventually([&] { return sameOwner.load(std::memory_order_acquire) == fibers; });
    eventually([&] {
        const auto stats = system.stats();
        return stats.liveFibers == 0 && stats.liveStacks == 0;
    });
    system.shutdown();
}

class FlagActor final : public actor_runtime::Actor {
public:
    FlagActor(actor_runtime::ActorSystem& system, std::atomic<int>& count)
        : Actor(system), count_(count) {}
private:
    void onMessage(actor_runtime::MessagePtr) override {
        count_.fetch_add(1, std::memory_order_release);
    }
    std::atomic<int>& count_;
};

void busy_actor_does_not_starve_another_actor() {
    actor_runtime::ActorSystem system({2});
    std::atomic<int> busyCount{0};
    std::atomic<int> busyMax{0};
    std::mutex mutex;
    std::set<std::thread::id> threads;
    std::atomic<int> quietCount{0};
    auto busy = system.registerActor(std::make_unique<ConcurrencyActor>(
        system, busyCount, busyMax, mutex, threads, 200us));
    auto quiet = system.registerActor(std::make_unique<FlagActor>(system, quietCount));
    constexpr int busyMessages = 600;
    for (int index = 0; index < busyMessages; ++index) busy.send(1, index);
    quiet.send(1, {});
    eventually([&] { return quietCount.load(std::memory_order_acquire) == 1; });
    require(busyCount.load(std::memory_order_acquire) < busyMessages,
        "quiet actor only ran after the busy actor drained completely");
    system.shutdown();
}

void invalid_inputs_have_clear_errors() {
    bool emptyAnyRejected = false;
    try {
        actor_runtime::WaitSpec::any({});
    } catch (const std::invalid_argument&) {
        emptyAnyRejected = true;
    }
    require(emptyAnyRejected, "empty Any was accepted");

    actor_runtime::ActorRef empty;
    bool emptyRefRejected = false;
    try {
        empty.send(1, {});
    } catch (const std::invalid_argument&) {
        emptyRefRejected = true;
    }
    require(emptyRefRejected, "empty ActorRef was accepted");
}

} // namespace

int main() {
    try {
        basic_post_calls_on_message_exactly_once();
        std::cout << "[PASS] basic post calls onMessage exactly once\n";
        ucontext_timer_preserves_nested_stack_locals();
        std::cout << "[PASS] ucontext timer preserves nested stack locals\n";
        message_wait_matches_type_and_correlation();
        std::cout << "[PASS] message wait matches type and correlation\n";
        message_wait_is_broadcast();
        std::cout << "[PASS] message wait is broadcast\n";
        dispatch_snapshot_excludes_new_waiters();
        std::cout << "[PASS] dispatch snapshot excludes new waiters\n";
        child_join_and_error_propagation();
        std::cout << "[PASS] child join and error propagation\n";
        any_all_and_nested_waits();
        std::cout << "[PASS] Any, All, and nested waits\n";
        response_timeout_race_never_double_resumes();
        std::cout << "[PASS] response/timeout race never double resumes\n";
        child_lifetime_and_shutdown_cancellation();
        std::cout << "[PASS] child lifetime and shutdown cancellation\n";
        actor_is_serial_but_migrates_across_workers();
        std::cout << "[PASS] actor is serial but migrates across workers\n";
        fiber_stays_on_owner_worker_and_stack_is_reclaimed();
        std::cout << "[PASS] fiber owner is stable and stack is reclaimed\n";
        busy_actor_does_not_starve_another_actor();
        std::cout << "[PASS] busy actor does not starve another actor\n";
        invalid_inputs_have_clear_errors();
        std::cout << "[PASS] invalid inputs have clear errors\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
