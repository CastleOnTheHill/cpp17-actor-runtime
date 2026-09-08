#include "actor_runtime/actor_system.h"
#include "actor_runtime/actor.h"
#include <array>
#include <atomic>
#include <future>
#include <iostream>
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
int main() {
    try {
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
        std::cout << "PASS concurrent FIFO, serial dispatch, queued drain, stale ref, name reuse, self-send\n";
    } catch(const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
