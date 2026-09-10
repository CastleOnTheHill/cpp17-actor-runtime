#include "actor_runtime/actor_system.h"
#include "actor_runtime/actor.h"
#include <array>
#include <atomic>
#include <ctime>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace actor_runtime;
using namespace std::chrono_literals;
constexpr int senders=8, perSender=2000;
struct State {
    std::array<std::atomic<int>,senders> next{};
    std::atomic<int> active{0}, received{0};
    std::atomic<bool> invalid{false};
    std::promise<void> entered, release, done;
    std::shared_future<void> gate=release.get_future().share();
};
class Sink final: public Actor {
public:
    Sink(ActorSystem& s,std::shared_ptr<State> value):Actor(s),state(std::move(value)) {}
private:
    void onMessage(MessagePtr m) override {
        if (m->typeId==1) { state->entered.set_value(); state->gate.wait(); return; }
        if (state->active.fetch_add(1)!=0) state->invalid=true;
        auto p=std::any_cast<std::pair<int,int>>(m->payload);
        if (state->next[static_cast<std::size_t>(p.first)].fetch_add(1)!=p.second) state->invalid=true;
        state->active.fetch_sub(1);
        if (state->received.fetch_add(1)+1==senders*perSender) state->done.set_value();
    }
    std::shared_ptr<State> state;
};
class Bounce final: public Actor {
public:
    Bounce(ActorSystem& s,std::promise<void>& d):Actor(s),system(s),done(d) {}
private:
    void onMessage(MessagePtr m) override {
        auto remaining=std::any_cast<int>(m->payload);
        if (remaining) system.ref(id()).send(1,remaining-1);
        else done.set_value();
    }
    ActorSystem& system; std::promise<void>& done;
};

struct IdlePeerState {
    std::promise<void> received;
    std::shared_future<void> receipt = received.get_future().share();
    std::promise<bool> checked;
};

class IdlePeerReceiver final: public Actor {
public:
    IdlePeerReceiver(ActorSystem& system, std::shared_ptr<IdlePeerState> state)
        : Actor(system), state_(std::move(state)) {}
private:
    void onMessage(MessagePtr) override { state_->received.set_value(); }
    std::shared_ptr<IdlePeerState> state_;
};

class BusyPublisher final: public Actor {
public:
    BusyPublisher(ActorSystem& system, ActorRef receiver, std::shared_ptr<IdlePeerState> state)
        : Actor(system), receiver_(receiver), state_(std::move(state)) {}
private:
    void onMessage(MessagePtr) override {
        // Let the otherwise idle peer park before publishing exactly one job.
        // The bounded wait deliberately occupies this worker: an independent
        // Actor must be runnable on the idle worker before this handler returns.
        // This is a scheduler stress fixture, not recommended application code.
        std::this_thread::sleep_for(30ms);
        receiver_.send(1, {});
        state_->checked.set_value(state_->receipt.wait_for(500ms) == std::future_status::ready);
    }
    ActorRef receiver_;
    std::shared_ptr<IdlePeerState> state_;
};

void lone_dispatch_wakes_idle_peer() {
    for (int round = 0; round < 3; ++round) {
        auto state = std::make_shared<IdlePeerState>();
        auto checked = state->checked.get_future();
        ActorSystem::Config config; config.workerCount = 2;
        ActorSystem system(config);
        auto receiver = system.registerActor(std::make_unique<IdlePeerReceiver>(system, state));
        auto publisher = system.registerActor(std::make_unique<BusyPublisher>(system, receiver, state));
        publisher.send(1, {});
        if (checked.wait_for(2s) != std::future_status::ready)
            throw std::runtime_error("idle-peer scheduler fixture stalled");
        const bool ranBeforePublisherReturned = checked.get();
        system.shutdown();
        if (!ranBeforePublisherReturned)
            throw std::runtime_error("one local dispatch did not wake an idle peer while its owner was busy");
    }
}

struct FairState {
    bool olderRan = false; // Single scheduler worker only.
    int newerRuns = 0;
    std::promise<bool> done;
};
class OlderReady final: public Actor {
public:
    OlderReady(ActorSystem& system, std::shared_ptr<FairState> state)
        : Actor(system), state_(std::move(state)) {}
private:
    void onMessage(MessagePtr) override { state_->olderRan = true; }
    std::shared_ptr<FairState> state_;
};
class NewerChain final: public Actor {
public:
    NewerChain(ActorSystem& system, ActorRef older, std::shared_ptr<FairState> state)
        : Actor(system), system_(system), older_(older), state_(std::move(state)) {}
private:
    void onMessage(MessagePtr message) override {
        if (message->typeId == 1) {
            older_.send(1, {});
        } else if (message->typeId == 3) {
            // Force publication through globalQueue before continuing the local
            // chain. A local-ready fast path must not starve external traffic.
            auto external = std::async(std::launch::async, [older = older_] { older.send(1, {}); });
            external.get();
        } else {
            ++state_->newerRuns;
            if (state_->olderRan || state_->newerRuns == 1000) {
                state_->done.set_value(state_->olderRan && state_->newerRuns <= 32);
                return;
            }
        }
        system_.ref(id()).send(2, {});
    }
    ActorSystem& system_;
    ActorRef older_;
    std::shared_ptr<FairState> state_;
};
void older_ready_is_not_starved_by_newer_chain(TypeId startType = 1) {
    auto state = std::make_shared<FairState>();
    auto done = state->done.get_future();
    ActorSystem::Config config; config.workerCount = 1;
    ActorSystem system(config);
    auto older = system.registerActor(std::make_unique<OlderReady>(system, state));
    auto chain = system.registerActor(std::make_unique<NewerChain>(system, older, state));
    chain.send(startType, {});
    if (done.wait_for(2s) != std::future_status::ready)
        throw std::runtime_error("newer-chain fairness fixture stalled");
    const bool fair = done.get();
    system.shutdown();
    if (!fair) throw std::runtime_error("newer local continuations starved an older ready Actor");
}

void idle_workers_park_after_bounded_backoff() {
    ActorSystem::Config config; config.workerCount = 4;
    ActorSystem system(config);
    std::this_thread::sleep_for(30ms);
    const auto before = std::clock();
    std::this_thread::sleep_for(200ms);
    const auto after = std::clock();
    system.shutdown();
    if (before == static_cast<std::clock_t>(-1) || after == static_cast<std::clock_t>(-1))
        throw std::runtime_error("cannot read process CPU time for idle regression");
    // Deliberately generous: idle workers should consume almost no process CPU.
    // This catches an accidentally unbounded polling loop, not a production SLA.
    const double cpuSeconds = static_cast<double>(after - before) / CLOCKS_PER_SEC;
    if (cpuSeconds > 0.1) throw std::runtime_error("idle scheduler workers kept consuming CPU instead of parking");
}

struct StopRaceState {
    std::atomic<int> received{0}, destroyed{0};
    std::atomic<bool> stopping{false}, invalid{false};
    std::promise<void> first;
};
class StopRaceActor final: public Actor {
public:
    StopRaceActor(ActorSystem& system, std::shared_ptr<StopRaceState> state)
        : Actor(system), state_(std::move(state)) {}
    ~StopRaceActor() override { ++state_->destroyed; }
private:
    void onMessage(MessagePtr) override {
        if (state_->received.fetch_add(1) == 0) state_->first.set_value();
    }
    std::shared_ptr<StopRaceState> state_;
};
void concurrent_send_stop_and_name_reuse() {
    ActorSystem::Config config; config.workerCount = 4;
    ActorSystem system(config);
    for (int round = 0; round < 20; ++round) {
        auto state = std::make_shared<StopRaceState>();
        auto first = state->first.get_future();
        auto ref = system.registerActor("stop-race", std::make_unique<StopRaceActor>(system, state));
        std::vector<std::thread> producers;
        struct Join {
            std::vector<std::thread>& threads;
            ~Join() { for (auto& thread : threads) if (thread.joinable()) thread.join(); }
        } join{producers};
        for (int sender = 0; sender < 3; ++sender) producers.emplace_back([ref, state] {
            try { for (int message = 0; message < 1000; ++message) ref.send(1, message); }
            catch (const std::invalid_argument&) { if (!state->stopping) state->invalid = true; }
            catch (...) { state->invalid = true; }
        });
        if (first.wait_for(2s) != std::future_status::ready)
            throw std::runtime_error("concurrent-stop fixture never dispatched its first message");
        state->stopping = true;
        auto stopped = system.stopActor(ref.id());
        for (auto& producer : producers) producer.join();
        if (stopped.wait_for(2s) != std::future_status::ready)
            throw std::runtime_error("concurrent sends prevented Actor stop from completing");
        stopped.get();
        if (state->invalid || state->destroyed != 1)
            throw std::runtime_error("send/stop admission or exactly-once destruction violated");
        bool staleRejected = false;
        try { ref.send(1, 0); } catch (const std::invalid_argument&) { staleRejected = true; }
        if (!staleRejected) throw std::runtime_error("stale ActorRef survived concurrent stop");
    }
    system.shutdown();
}

class BurstCreditActor final : public Actor {
public:
    BurstCreditActor(ActorSystem& system, std::promise<void>& done) : Actor(system), done_(done) {}
private:
    void onMessage(MessagePtr message) override {
        if (message->typeId == 1) {
            for (int i = 0; i < 128; ++i) self().send(2, i);
            // The scheduler must finish accounting even after a handler throws.
            throw std::runtime_error("intentional error after posting successors");
        }
        if (std::any_cast<int>(message->payload) != received_ % 128) invalid_ = true;
        if (++received_ % 128 == 0) {
            if (received_ == 128 * 100) {
                if (invalid_) done_.set_exception(std::make_exception_ptr(std::runtime_error("burst FIFO changed")));
                else done_.set_value();
            } else self().send(1, {});
        }
    }
    std::promise<void>& done_;
    int received_ = 0;
    bool invalid_ = false;
};
void burst_successors_and_exception_drain() {
    for (const auto workers : {1U, 4U}) {
        std::promise<void> done;
        auto finished = done.get_future();
        ActorSystem system({workers});
        auto actor = system.registerActor(std::make_unique<BurstCreditActor>(system, done));
        actor.send(1, {});
        if (finished.wait_for(3s) != std::future_status::ready)
            throw std::runtime_error("burst successors stalled");
        finished.get();
        // Detect lost/duplicated drain credits after first-successor transfers,
        // multiple posts, pending dispatch handoff and isolated exceptions.
        system.shutdown();
    }
}

struct RegistryStressState {
    std::atomic<int> received{0};
    std::atomic<bool> invalid{false};
    std::promise<void> done;
};
class RegistryStressActor final : public Actor {
public:
    RegistryStressActor(ActorSystem& system, std::shared_ptr<RegistryStressState> state)
        : Actor(system), state_(std::move(state)) {}
private:
    void onMessage(MessagePtr) override {
        if (state_->received.fetch_add(1) + 1 == 8000) state_->done.set_value();
    }
    std::shared_ptr<RegistryStressState> state_;
};
void concurrent_registry_growth_and_lookup() {
    auto state = std::make_shared<RegistryStressState>();
    auto done = state->done.get_future();
    ActorSystem system({4});
    std::vector<ActorRef> stable;
    for (int i = 0; i < 64; ++i)
        stable.push_back(system.registerActor(std::make_unique<RegistryStressActor>(system, state)));
    std::vector<std::thread> senders;
    struct Join {
        std::vector<std::thread>& threads;
        ~Join() { for (auto& thread : threads) if (thread.joinable()) thread.join(); }
    } join{senders};
    for (int sender = 0; sender < 4; ++sender) senders.emplace_back([&, sender] {
        try {
            for (int i = 0; i < 2000; ++i) stable[(i + sender) % stable.size()].send(1, i);
        } catch (...) { state->invalid = true; }
    });
    // Force map growth/rehash and erasure while lookups use different stripes.
    // The registry must remain authoritative; no per-reference cache is used.
    std::vector<ActorRef> transient;
    for (int i = 0; i < 256; ++i)
        transient.push_back(system.registerActor(std::make_unique<RegistryStressActor>(system, state)));
    for (const auto ref : transient) system.stopActor(ref.id());
    for (auto& sender : senders) sender.join();
    if (done.wait_for(5s) != std::future_status::ready || state->invalid || state->received != 8000)
        throw std::runtime_error("concurrent registry growth/lookup lost messages or failed");
    done.get();
    system.shutdown();
}

int main() {
    try {
        concurrent_registry_growth_and_lookup();
        burst_successors_and_exception_drain();
        lone_dispatch_wakes_idle_peer();
        older_ready_is_not_starved_by_newer_chain();
        older_ready_is_not_starved_by_newer_chain(3);
        idle_workers_park_after_bounded_backoff();
        concurrent_send_stop_and_name_reuse();
        auto state=std::make_shared<State>(); auto entered=state->entered.get_future(); auto done=state->done.get_future();
        ActorSystem::Config c; c.workerCount=4; ActorSystem system(c);
        auto sink=system.registerActor("sink",std::make_unique<Sink>(system,state));
        struct Release {
            std::shared_ptr<State> state; bool released=false;
            void open() { if(!released) { released=true; state->release.set_value(); } }
            ~Release() { open(); }
        } release{state};
        sink.send(1,{});
        if (entered.wait_for(2s)!=std::future_status::ready) throw std::runtime_error("handler did not enter");
        std::vector<std::thread> producers;
        struct Join { std::vector<std::thread>& threads; ~Join(){for(auto& t:threads) if(t.joinable()) t.join();} } join{producers};
        for(int i=0;i<senders;++i) producers.emplace_back([&,i]{
            try { for(int n=0;n<perSender;++n) sink.send(2,std::make_pair(i,n)); }
            catch(...) { state->invalid=true; }
        });
        for(auto& t:producers) t.join();
        release.open();
        if(done.wait_for(3s)!=std::future_status::ready) throw std::runtime_error("queued messages lost or stalled");
        done.get();
        if(state->invalid || state->received!=senders*perSender) throw std::runtime_error("FIFO/count/serial handler contract violated");
        for(auto& count:state->next) if(count!=perSender) throw std::runtime_error("sender sequence incomplete");
        auto stopped=system.stopActor(sink.id()); stopped.get();
        bool staleRejected=false;
        try { sink.send(2,std::make_pair(0,0)); } catch(const std::invalid_argument&) { staleRejected=true; }
        if(!staleRejected) throw std::runtime_error("stale reference accepted after stop");
        std::promise<void> bounced; auto bounceDone=bounced.get_future();
        auto replacement=system.registerActor("sink",std::make_unique<Bounce>(system,bounced));
        replacement.send(1,10000);
        if(bounceDone.wait_for(3s)!=std::future_status::ready) throw std::runtime_error("self-send ready token lost");
        bounceDone.get(); system.shutdown();
        std::cout << "PASS idle-peer wakeup, oldest-ready fairness, idle parking, concurrent FIFO, serial dispatch, queued drain, stale ref, name reuse, self-send\n";
    } catch(const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
