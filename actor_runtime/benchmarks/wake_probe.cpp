// Diagnostic only: an unrelated global-queue pulse tests the local wakeup
// hypothesis. This is NOT a proposed production fix or a throughput competitor.
#include "common.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <atomic>
#include <memory>
#include <thread>
namespace ar=actor_runtime;
class Echo final: public ar::Actor {
public:
    explicit Echo(ar::ActorSystem& s):Actor(s),system(s) {}
    void onMessage(ar::MessagePtr m) override { system.ref(m->sender).send(2,m->payload,m->id); }
    ar::ActorSystem& system;
};
class Pulse final: public ar::Actor {
public:
    explicit Pulse(ar::ActorSystem& s):Actor(s) {}
    void onMessage(ar::MessagePtr) override {}
};
class Client final: public ar::Actor {
public:
    Client(ar::ActorSystem& s,ar::ActorRef e,bench::Client& d):Actor(s),echo(e),data(d) {}
    void onCoroutineError(ar::FiberId,std::exception_ptr e) override {
        try { std::rethrow_exception(e); } catch(const std::exception& x) { data.harness.fail(x.what()); }
    }
    void onMessage(ar::MessagePtr m) override {
        if(m->typeId!=3) return;
        data.reset(std::any_cast<bench::Begin>(m->payload));
        start([this](ar::CoroutineContext& ctx)->std::any {
            while(data.issued<data.target) {
                auto id=echo.send(1,data.packet());
                auto result=ctx.await(ar::WaitSpec::message({2,id})); ctx.throwIfCancelled();
                data.accept(std::any_cast<const bench::Packet&>(result.as<ar::MessagePtr>()->payload));
            }
            data.harness.complete(0); return {};
        });
    }
    ar::ActorRef echo; bench::Client& data;
};
int main(int argc,char** argv) {
    try {
        bool pulse=false; std::vector<char*> args;
        for(int i=0;i<argc;++i) {
            if(std::string(argv[i])=="--pulse") pulse=true;
            else args.push_back(argv[i]);
        }
        auto config=bench::parse(static_cast<int>(args.size()),args.data());
        if(config.clients!=1 || config.window!=1) throw std::invalid_argument("one client/window required");
        bench::Harness h(config); bench::Client data(h,0);
        ar::ActorSystem::Config options; options.workerCount=config.workers;
        ar::ActorSystem system(options);
        auto echo=system.registerActor(std::make_unique<Echo>(system));
        auto client=system.registerActor(std::make_unique<Client>(system,echo,data));
        auto noop=system.registerActor(std::make_unique<Pulse>(system));
        std::atomic<bool> stop{false}; std::thread helper;
        struct Join { std::atomic<bool>& stop; std::thread& thread;
            ~Join() { stop=true; if(thread.joinable()) thread.join(); }
        } join{stop,helper};
        if(pulse) helper=std::thread([&] {
            try {
                while(!stop) { noop.send(4,{}); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            } catch(const std::exception& e) { h.fail(e.what()); }
        });
        for(bool measured:{false,true}) {
            h.beginPhase(); client.send(3,bench::Begin{measured?config.rounds:config.warmup,measured}); h.wait();
        }
        h.report(pulse?"wake-probe-pulse-1ms":"wake-probe-control","fiber",data.samples);
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
