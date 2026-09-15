#include "actor_runtime/actor.h"
#include "actor_runtime/actor_system.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace actor_runtime;
using namespace std::chrono_literals;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class Predicate> void eventually(Predicate ready) {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!ready()) {
        require(std::chrono::steady_clock::now() < deadline, "fixture timed out");
        std::this_thread::yield();
    }
}
struct State {
    std::promise<void> entered, release;
    std::future<void> entry = entered.get_future();
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<int> received{0};
    std::mutex mutex;
    std::set<MessageId> ids;
    bool duplicate = false;
};
struct GateGuard {
    std::shared_ptr<State> state;
    bool opened = false;
    void open() { if (!opened) { opened = true; state->release.set_value(); } }
    ~GateGuard() { open(); }
};
class Sink final : public Actor {
public:
    Sink(ActorSystem& s, std::shared_ptr<State> state) : Actor(s), state_(std::move(state)) {}
private:
    void onMessage(MessagePtr m) override {
        if (m->typeId == 1) {
            state_->entered.set_value();
            state_->gate.wait(); // Test-only stalled consumer, released even on assertion failure.
        } else {
            { std::lock_guard<std::mutex> lock(state_->mutex);
              if (!state_->ids.insert(m->id).second) state_->duplicate = true; }
            ++state_->received;
        }
    }
    std::shared_ptr<State> state_;
};
void enter(ActorRef target, State& state) {
    target.send(1, {});
    require(state.entry.wait_for(5s) == std::future_status::ready, "gate did not enter");
}
void ready_token() {
    ActorSystem::Config c; c.workerCount = 1; c.mailboxMaxMessages = 1;
    ActorSystem s(c);
    auto blocked = std::make_shared<State>(), state = std::make_shared<State>();
    auto blocker = s.registerActor(std::make_unique<Sink>(s, blocked));
    auto target = s.registerActor(std::make_unique<Sink>(s, state));
    GateGuard gate{blocked}; enter(blocker, *blocked);
    auto first = target.trySend(2, {});
    require(first.status == SendStatus::Accepted && first.id, "initial admission");
    require(s.stats().queuedMailboxBytes == MailboxEnvelopeBytes, "ready token uncharged");
    auto full = target.trySend(2, {});
    require(full.status == SendStatus::Full && full.id == 0, "ready token bypassed limit");
    bool threw = false;
    try { target.send(2, {}); } catch (const std::runtime_error&) { threw = true; }
    require(threw, "send silently dropped overflow");
    gate.open(); eventually([&] { return state->received == 1; });
    require(s.stats().queuedMailboxBytes == 0, "consumed charge retained");
    require(target.trySend(2, {}).status == SendStatus::Accepted, "capacity not reusable");
    eventually([&] { return state->received == 2; });
}
void concurrent(std::size_t workers) {
    ActorSystem::Config c; c.workerCount = workers; c.mailboxMaxMessages = 64;
    ActorSystem s(c); auto state = std::make_shared<State>();
    auto target = s.registerActor(std::make_unique<Sink>(s, state));
    GateGuard gate{state}; enter(target, *state);
    std::atomic<int> accepted{0}, full{0}, unexpected{0};
    std::mutex mutex; std::set<MessageId> ids; std::vector<std::thread> producers;
    for (int t = 0; t < 8; ++t) producers.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            auto r = target.trySend(2, {});
            if (r.status == SendStatus::Accepted) {
                ++accepted; std::lock_guard<std::mutex> lock(mutex); ids.insert(r.id);
            } else if (r.status == SendStatus::Full && r.id == 0) ++full;
            else ++unexpected;
        }
    });
    for (auto& p : producers) p.join();
    require(accepted == 64 && full == 736 && unexpected == 0, "concurrent count limit");
    require(s.stats().queuedMailboxBytes == 64 * MailboxEnvelopeBytes, "concurrent accounting");
    gate.open(); eventually([&] { return state->received == 64; });
    { std::lock_guard<std::mutex> lock(state->mutex);
      require(!state->duplicate && state->ids == ids, "accepted message lost or duplicated"); }
    require(s.stats().queuedMailboxBytes == 0, "concurrent charge leaked");
}
void bytes() {
    constexpr auto cost = MailboxEnvelopeBytes + sizeof(Bytes) + 64;
    ActorSystem::Config c; c.workerCount = 1; c.mailboxMaxBytes = 2 * cost; c.maxMessageBytes = cost;
    ActorSystem s(c); auto state = std::make_shared<State>();
    auto target = s.registerActor("sink", std::make_unique<Sink>(s, state));
    GateGuard gate{state}; enter(target, *state);
    require(target.trySend(2, Bytes(65)).status == SendStatus::MessageTooLarge, "oversized accepted");
    require(s.stats().queuedMailboxBytes == 0, "rejection leaked credit");
    require(target.trySend(2, Bytes(64)).status == SendStatus::Accepted, "exact message limit");
    std::uint8_t data[64]{}; s.sendBytes({{}, "sink"}, 2, data, sizeof(data));
    require(s.stats().queuedMailboxBytes == 2 * cost, "Bytes charge");
    require(target.trySend(2, {}).status == SendStatus::Full, "byte limit bypassed");
    bool threw = false;
    try { s.sendBytes({{}, "sink"}, 2, data, sizeof(data)); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw, "local sendBytes bypassed limit");
    gate.open(); eventually([&] { return state->received == 2; });
    require(s.stats().queuedMailboxBytes == 0, "Bytes charge leaked");
}
void estimator() {
    struct Estimate { std::size_t bytes; };
    ActorSystem::Config c; c.workerCount = 1; c.mailboxMaxBytes = MailboxEnvelopeBytes + 100;
    c.payloadSizer = [](const std::any& p) -> std::size_t {
        if (!p.has_value()) return 0;
        if (p.type() == typeid(int)) throw std::runtime_error("estimate failed");
        return std::any_cast<Estimate>(p).bytes;
    };
    ActorSystem s(c); auto state = std::make_shared<State>();
    auto target = s.registerActor(std::make_unique<Sink>(s, state));
    GateGuard gate{state}; enter(target, *state);
    bool threw = false;
    try { target.trySend(2, 42); } catch (const std::runtime_error&) { threw = true; }
    require(threw && s.stats().queuedMailboxBytes == 0, "estimate exception leaked credit");
    require(target.trySend(2, Estimate{std::numeric_limits<std::size_t>::max()}).status ==
        SendStatus::MessageTooLarge, "size wrapped");
    require(target.trySend(2, Estimate{100}).status == SendStatus::Accepted, "custom estimate rejected");
    require(target.trySend(2, {}).status == SendStatus::Full, "custom estimate not charged");
    gate.open(); eventually([&] { return state->received == 1; });
    require(s.stats().queuedMailboxBytes == 0, "custom charge leaked");
}
void retained_capacity() {
    ActorSystem::Config c; c.workerCount = 1;
    ActorSystem s(c); auto state = std::make_shared<State>();
    auto target = s.registerActor(std::make_unique<Sink>(s, state));
    GateGuard gate{state}; enter(target, *state);
    Bytes buffer(1); buffer.reserve(4096);
    const auto byteCost = MailboxEnvelopeBytes + sizeof(Bytes) + buffer.capacity();
    require(target.trySend(2, std::move(buffer)).status == SendStatus::Accepted, "reserved Bytes rejected");
    std::string text(1, 'x'); text.reserve(2048);
    const auto stringCost = MailboxEnvelopeBytes + sizeof(std::string) + text.capacity() + 1;
    require(target.trySend(2, std::move(text)).status == SendStatus::Accepted, "reserved string rejected");
    require(s.stats().queuedMailboxBytes == byteCost + stringCost, "size substituted for retained capacity");
    gate.open(); eventually([&] { return state->received == 2; });
    require(s.stats().queuedMailboxBytes == 0, "retained capacity charge leaked");
}
void zero_limits() {
    for (int limit = 0; limit < 4; ++limit) {
        ActorSystem::Config c; c.workerCount = 1;
        if (limit == 0) c.mailboxMaxMessages = 0;
        if (limit == 1) c.mailboxMaxBytes = 0;
        if (limit == 2) c.systemMailboxMaxBytes = 0;
        if (limit == 3) c.maxMessageBytes = 0;
        ActorSystem s(c); auto state = std::make_shared<State>();
        auto target = s.registerActor(std::make_unique<Sink>(s, state));
        auto result = target.trySend(2, {});
        require(result.status == (limit == 3 ? SendStatus::MessageTooLarge : SendStatus::Full), "zero limit disabled admission bound");
        require(result.id == 0 && s.stats().queuedMailboxBytes == 0, "zero limit leaked credit");
        s.shutdown(); require(state->received == 0, "zero limit delivered message");
    }
}
void concurrent_system_budget() {
    ActorSystem::Config c; c.workerCount = 1;
    c.mailboxMaxMessages = 32; c.systemMailboxMaxBytes = 64 * MailboxEnvelopeBytes;
    ActorSystem s(c); auto blocked = std::make_shared<State>();
    auto blocker = s.registerActor(std::make_unique<Sink>(s, blocked));
    GateGuard gate{blocked}; enter(blocker, *blocked);
    std::vector<std::shared_ptr<State>> states;
    std::vector<ActorRef> targets;
    for (int i = 0; i < 8; ++i) {
        states.push_back(std::make_shared<State>());
        targets.push_back(s.registerActor(std::make_unique<Sink>(s, states.back())));
    }
    std::atomic<int> accepted{0}, full{0}, unexpected{0};
    std::vector<std::thread> producers;
    for (const auto target : targets) producers.emplace_back([&, target] {
        for (int i = 0; i < 16; ++i) {
            auto result = target.trySend(2, {});
            if (result.status == SendStatus::Accepted) ++accepted;
            else if (result.status == SendStatus::Full) ++full;
            else ++unexpected;
        }
    });
    for (auto& producer : producers) producer.join();
    require(accepted == 64 && full == 64 && unexpected == 0, "parallel Actors exceeded system budget");
    require(s.stats().queuedMailboxBytes == c.systemMailboxMaxBytes, "system reservation mismatch");
    gate.open();
    eventually([&] {
        int received = 0; for (const auto& state : states) received += state->received.load();
        return received == 64;
    });
    require(s.stats().queuedMailboxBytes == 0, "parallel Actors retained system credit");
}
void stop(std::size_t workers) {
    ActorSystem::Config c; c.workerCount = workers; c.systemMailboxMaxBytes = 2 * MailboxEnvelopeBytes;
    ActorSystem s(c); auto a = std::make_shared<State>(), b = std::make_shared<State>();
    auto first = s.registerActor(std::make_unique<Sink>(s, a));
    auto second = s.registerActor(std::make_unique<Sink>(s, b));
    GateGuard gate{a}; enter(first, *a);
    require(first.trySend(2, {}).status == SendStatus::Accepted, "global first");
    require(first.trySend(2, {}).status == SendStatus::Accepted, "global second");
    require(second.trySend(2, {}).status == SendStatus::Full, "global budget bypassed");
    auto done = s.stopActor(first.id());
    require(first.trySend(2, {}).status == SendStatus::Stopped, "stop accepted new message");
    gate.open();
    require(done.wait_for(5s) == std::future_status::ready, "full mailbox blocked stop");
    eventually([&] { return s.stats().queuedMailboxBytes == 0; });
    require(second.trySend(2, {}).status == SendStatus::Accepted, "stop retained budget");
    eventually([&] { return b->received == 1; });
    s.shutdown();
    require(s.stats().queuedMailboxBytes == 0, "shutdown retained budget");
    require(second.trySend(2, {}).status == SendStatus::Stopped, "shutdown accepted message");
    require(ActorRef{}.trySend(2, {}).status == SendStatus::Stopped, "empty ref status");
}
void concurrent_drain(std::size_t workers) {
    ActorSystem::Config c; c.workerCount = workers;
    c.mailboxMaxMessages = 4; c.systemMailboxMaxBytes = 4 * MailboxEnvelopeBytes;
    ActorSystem s(c); auto state = std::make_shared<State>();
    auto target = s.registerActor(std::make_unique<Sink>(s, state));
    std::mutex mutex; std::set<MessageId> acceptedIds;
    std::atomic<bool> failed{false};
    std::vector<std::thread> producers;
    // An external producer may retry with a bounded deadline; no Actor worker
    // blocks waiting for capacity. Consumption overlaps every producer.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (int t = 0; t < 4; ++t) producers.emplace_back([&] {
        try {
            for (int i = 0; i < 2000 && !failed; ) {
                const auto result = target.trySend(2, {});
                if (result.status == SendStatus::Accepted) {
                    std::lock_guard<std::mutex> lock(mutex); acceptedIds.insert(result.id); ++i;
                } else if (result.status == SendStatus::Full && result.id == 0) {
                    if (std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
                    std::this_thread::yield();
                } else { failed = true; break; }
                if (s.stats().queuedMailboxBytes > c.systemMailboxMaxBytes) failed = true;
            }
        } catch (...) { failed = true; }
    });
    for (auto& producer : producers) producer.join();
    require(!failed && acceptedIds.size() == 8000, "concurrent drain failed or exceeded budget");
    eventually([&] { return state->received == 8000; });
    { std::lock_guard<std::mutex> lock(state->mutex);
      require(!state->duplicate && state->ids == acceptedIds, "concurrent drain lost or duplicated messages"); }
    require(s.stats().queuedMailboxBytes == 0, "concurrent drain retained credit");
}

// Opt-in Reserve mode must never exceed the system budget, must still deliver
// every accepted message, must keep early rejection bounded by one chunk, and
// must hand the held chunk back once the Actor stops.
void reserve_mode(std::size_t workers) {
    ActorSystem::Config c; c.workerCount = workers;
    c.mailboxMaxMessages = 4096;
    c.systemMailboxMaxBytes = 16 * MailboxEnvelopeBytes;
    c.mailboxBudget = ActorSystem::Config::MailboxBudget::Reserve;
    c.reserveChunkBytes = 4 * MailboxEnvelopeBytes;
    ActorSystem s(c);
    auto blocked = std::make_shared<State>();
    auto target = s.registerActor(std::make_unique<Sink>(s, blocked));
    GateGuard gate{blocked}; enter(target, *blocked);
    const std::size_t budgetMessages = c.systemMailboxMaxBytes / MailboxEnvelopeBytes;
    std::size_t accepted = 0, full = 0;
    for (std::size_t i = 0; i < budgetMessages * 4; ++i) {
        const auto result = target.trySend(2, {});
        if (result.status == SendStatus::Accepted) {
            ++accepted;
            require(s.stats().queuedMailboxBytes <= c.systemMailboxMaxBytes,
                "reserve mode exceeded the system budget");
        } else if (result.status == SendStatus::Full) {
            ++full;
        } else {
            require(false, "reserve mode returned an unexpected send status");
        }
    }
    require(full > 0, "reserve mode never applied backpressure");
    require(accepted + c.reserveChunkBytes / MailboxEnvelopeBytes >= budgetMessages,
        "reserve mode rejected far below the system budget");
    gate.open();
    eventually([&] { return blocked->received.load() == static_cast<int>(accepted); });
    auto done = s.stopActor(target.id());
    require(done.wait_for(5s) == std::future_status::ready, "reserve mode stop timed out");
    eventually([&] { return s.stats().queuedMailboxBytes == 0; });
    auto otherState = std::make_shared<State>();
    auto other = s.registerActor(std::make_unique<Sink>(s, otherState));
    require(other.trySend(2, {}).status == SendStatus::Accepted, "stopped Actor kept the system budget");
    eventually([&] { return otherState->received.load() == 1; });
    s.shutdown();
}
}
int main() {
    try {
        ready_token(); bytes(); estimator(); retained_capacity(); zero_limits(); concurrent_system_budget();
        for (std::size_t workers : {1U, 4U}) { concurrent(workers); stop(workers); concurrent_drain(workers); reserve_mode(workers); }
        std::cout << "mailbox capacity scenarios passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
