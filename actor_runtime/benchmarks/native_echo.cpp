#include "common.h"
#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <memory>
#include <cstdlib>
namespace ar=actor_runtime;
constexpr ar::TypeId Start=100,Request=101,Response=102;
class Echo final: public ar::Actor {
public:
    Echo(ar::ActorSystem& s,bench::Harness& h): Actor(s),system_(s),h_(h) {}
private:
    void onMessage(ar::MessagePtr m) override {
        try {
            if(m->typeId!=Request) throw std::runtime_error("unexpected request type");
            system_.ref(m->sender).send(Response,m->payload,m->id);
        } catch(const std::exception& e) { h_.fail(e.what()); }
    }
    ar::ActorSystem& system_; bench::Harness& h_;
};
class Client final: public ar::Actor {
public:
    Client(ar::ActorSystem& s,ar::ActorRef echo,bench::Client& d): Actor(s),echo_(echo),data_(d) {}
private:
    void issue() { echo_.send(Request,data_.packet()); }
    void onCoroutineError(ar::FiberId,std::exception_ptr e) override {
        try { std::rethrow_exception(e); } catch(const std::exception& x) { data_.harness.fail(x.what()); }
    }
    void onMessage(ar::MessagePtr m) override {
        try {
            if(m->typeId==Start) {
                data_.reset(std::any_cast<bench::Begin>(m->payload));
#ifdef BENCH_FIBER
                start([this](ar::CoroutineContext& c)->std::any {
                    while(data_.issued<data_.target) {
                        auto id=echo_.send(Request,data_.packet());
                        auto r=c.await(ar::WaitSpec::message({Response,id})); c.throwIfCancelled();
                        data_.accept(std::any_cast<const bench::Packet&>(r.as<ar::MessagePtr>()->payload));
                    }
                    data_.harness.complete(data_.id); return {};
                });
#else
                for(size_t i=0;i<data_.harness.config.window && data_.issued<data_.target;++i) issue();
#endif
            }
#ifndef BENCH_FIBER
            else if(m->typeId==Response) {
                data_.accept(std::any_cast<const bench::Packet&>(m->payload));
                if(data_.issued<data_.target) issue();
                if(data_.received==data_.target) data_.harness.complete(data_.id);
            }
#endif
        } catch(const std::exception& e) { data_.harness.fail(e.what()); }
    }
    ar::ActorRef echo_; bench::Client& data_;
};
int main(int argc,char** argv) {
    try {
        auto c=bench::parse(argc,argv);
        if(c.batch!=1) throw std::invalid_argument("native scheduling batch is fixed at 1");
#ifdef BENCH_FIBER
        if(c.window!=1) throw std::invalid_argument("fiber echo requires window=1");
#endif
        bench::Harness h(c); std::vector<std::unique_ptr<bench::Client>> data;
        ar::ActorSystem::Config options; options.workerCount=c.workers;
        // Diagnostic runs are not accepted throughput measurements.
        options.diagnosticLogging = std::getenv("ACTOR_BENCH_DIAGNOSTICS") != nullptr;
        ar::ActorSystem system(options); // declared after data: destroys actors before data on exceptions
        std::vector<ar::ActorRef> clients;
        auto echo=system.registerActor(std::make_unique<Echo>(system,h));
        for(size_t i=0;i<c.clients;++i) {
            if(i && !c.hotspot) echo=system.registerActor(std::make_unique<Echo>(system,h));
            data.push_back(std::make_unique<bench::Client>(h,i));
            clients.push_back(system.registerActor(std::make_unique<Client>(system,echo,*data.back())));
        }
        for(bool measured:{false,true}) {
            h.beginPhase();
            for(auto client:clients) client.send(Start,bench::Begin{measured?c.rounds:c.warmup,measured});
            h.wait();
        }
        std::vector<double> samples;
        for(const auto& d:data) samples.insert(samples.end(),d->samples.begin(),d->samples.end());
#ifdef BENCH_FIBER
        h.report("actor-runtime","fiber",std::move(samples));
#else
        h.report("actor-runtime","event",std::move(samples));
#endif
        system.shutdown();
        if(system.stats().liveFibers || system.stats().liveStacks) throw std::runtime_error("stacks leaked");
    } catch(const std::exception& e) { std::cerr<<"benchmark failed: "<<e.what()<<'\n'; return 1; }
}
