// ASan 协程切换 hook 的用例：大量嵌套挂起/恢复，并校验往返后栈上数据仍然有效。
//
// 在 -fsanitize=address 构建下（见 CMake 选项 ACTOR_RUNTIME_ASAN），
// ASAN_OPTIONS 会打开 detect_stack_use_after_return，fake stack 会真正参与
// 协程栈切换。任何 hook 漏调、重复调或时机错误都会让 ASan 误报或直接终止，
// 因此下面的压力负载同时充当 hook 正确性的验证。
#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace actor_runtime;
using namespace std::chrono_literals;

namespace {

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kExpectAsanHooks = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kExpectAsanHooks = true;
#else
constexpr bool kExpectAsanHooks = false;
#endif
#else
constexpr bool kExpectAsanHooks = false;
#endif

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class Predicate>
void eventually(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(predicate(), message);
}

constexpr std::size_t kRounds = 30;
constexpr std::size_t kFibersPerRound = 6;

class SwitchActor final : public Actor {
public:
    struct Counters {
        std::atomic<int> rounds{0};
        std::atomic<int> preservedLocals{0};
        std::atomic<int> siblingRounds{0};
        std::atomic<int> corrupted{0};
        std::atomic<int> coroutineErrors{0};
    };

    explicit SwitchActor(Counters& counters) : counters_(counters) {}

private:
    // 混合“挂起后恢复”和“直接返回”的协程，覆盖两条离开协程栈的路径。
    static std::any leaf(CoroutineContext& child, std::size_t suspends) {
        for (std::size_t step = 0; step < suspends; ++step) {
            child.await(WaitSpec::timer(std::chrono::steady_clock::now() + 1ms));
            child.throwIfCancelled();
        }
        return int{17};
    }

    void runRound(Counters& counters) {
        start([this, &counters](CoroutineContext& root) -> std::any {
            std::vector<FiberId> children;
            std::vector<WaitSpec> waits;
            children.reserve(kFibersPerRound);
            waits.reserve(kFibersPerRound);
            for (std::size_t index = 0; index < kFibersPerRound; ++index) {
                const std::size_t suspends = index % 3;
                const auto lifetime = (index % 2) == 0 ? ChildLifetime::CancelWithParent
                                                       : ChildLifetime::DetachedFromParent;
                const auto child = root.spawn(
                    [suspends](CoroutineContext& context) -> std::any { return leaf(context, suspends); },
                    lifetime);
                children.push_back(child);
                waits.push_back(WaitSpec::child(child));
            }
            // 取消一半子协程：被取消的协程各自在自己的 worker 上解栈退出。
            for (std::size_t index = 1; index < children.size(); index += 2) {
                root.cancel(children[index]);
            }
            const auto settled = root.await(WaitSpec::allSettled(std::move(waits)));
            if (settled.children.size() != kFibersPerRound) {
                counters.corrupted.fetch_add(1, std::memory_order_release);
            }
            // 再挂起一次，保证根协程本身也走“挂起 -> 恢复 -> 正常返回”的路径。
            root.await(WaitSpec::timer(std::chrono::steady_clock::now() + 1ms));
            counters.rounds.fetch_add(1, std::memory_order_release);
            return {};
        });
    }

    // 挂起前留下逃逸的局部变量地址：ASan fake stack 必须随协程一起保留，
    // 恢复后仍然可读。中间插入兄弟协程，制造 fake stack 复用压力。
    void preserveLocals() {
        start([this](CoroutineContext& root) -> std::any {
            std::array<std::uint64_t, 96> buffer{};
            for (std::size_t index = 0; index < buffer.size(); ++index) {
                buffer[index] = index * 7U + 1U;
            }
            const std::uint64_t* pointer = buffer.data();
            auto sibling = root.spawn([](CoroutineContext& child) -> std::any {
                child.await(WaitSpec::timer(std::chrono::steady_clock::now() + 1ms));
                return int{1};
            }, ChildLifetime::CancelWithParent);
            root.await(WaitSpec::all({WaitSpec::timer(std::chrono::steady_clock::now() + 1ms),
                WaitSpec::child(sibling)}));
            for (std::size_t index = 0; index < buffer.size(); ++index) {
                if (pointer[index] != index * 7U + 1U) {
                    counters_.corrupted.fetch_add(1, std::memory_order_release);
                }
            }
            counters_.preservedLocals.fetch_add(1, std::memory_order_release);
            return {};
        });
    }

    // 兄弟协程正常退出（销毁 fake stack）时，仍在挂起的协程栈必须不受影响。
    void siblingExitWhileSuspended() {
        start([this](CoroutineContext& root) -> std::any {
            std::array<std::uint64_t, 32> buffer{};
            buffer[0] = 0xfeedfaceU;
            const std::uint64_t* pointer = buffer.data();
            auto sleeper = root.spawn([](CoroutineContext& child) -> std::any {
                return leaf(child, 0); // 直接返回：走 trampoline 销毁 fake stack 的路径。
            }, ChildLifetime::CancelWithParent);
            root.await(WaitSpec::child(sleeper));
            root.await(WaitSpec::timer(std::chrono::steady_clock::now() + 1ms));
            if (*pointer != 0xfeedfaceU) {
                counters_.corrupted.fetch_add(1, std::memory_order_release);
            }
            counters_.siblingRounds.fetch_add(1, std::memory_order_release);
            return {};
        });
    }

    void onMessage(MessagePtr message) override {
        if (message->typeId == 1) runRound(counters_);
        if (message->typeId == 2) preserveLocals();
        if (message->typeId == 3) siblingExitWhileSuspended();
    }

    void onCoroutineError(FiberId, std::exception_ptr error) override {
        if (error) counters_.coroutineErrors.fetch_add(1, std::memory_order_release);
    }

    Counters& counters_;
};

} // namespace

int main() {
    try {
        SwitchActor::Counters counters;
        ActorSystem& system = ActorSystem::start({4});
        auto actor = system.registerActor(std::make_unique<SwitchActor>(counters));

        for (std::size_t round = 0; round < kRounds; ++round) {
            actor.send(1, {});
            actor.send(2, {});
            actor.send(3, {});
        }
        eventually([&] { return counters.rounds.load(std::memory_order_acquire) == static_cast<int>(kRounds); },
            "rounds did not finish");
        eventually([&] { return counters.preservedLocals.load(std::memory_order_acquire) == static_cast<int>(kRounds); },
            "locals were not preserved across suspension");
        eventually([&] { return counters.siblingRounds.load(std::memory_order_acquire) == static_cast<int>(kRounds); },
            "sibling rounds did not finish");
        eventually([&] {
            const auto snapshot = system.stats();
            return snapshot.liveFibers == 0 && snapshot.liveStacks == 0;
        }, "fiber stacks were not reclaimed");        require(counters.corrupted.load(std::memory_order_acquire) == 0,
            "coroutine stack data was corrupted across switches");
        require(counters.coroutineErrors.load(std::memory_order_acquire) == 0,
            "coroutine raised an unexpected runtime error");

        const auto stats = system.stats();
        require(stats.acceptingMessages, "system stopped accepting messages");
        require(stats.fiberSwitches > 500, "too few annotated fiber switches were observed");
        if (kExpectAsanHooks) {
            require(stats.asanFiberHooks, "ASan fiber hooks were not linked into the process");
        }
        ActorSystem::stop();
        std::cout << "[PASS] " << stats.fiberSwitches << " annotated fiber switches, "
                  << "asanHooks=" << (stats.asanFiberHooks ? "on" : "off") << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
