// Diagnostic only: public workloads plus read-only actor-thread introspection.
// No runtime implementation, allocator policy or thread-stack defaults changed.
#include "internal.h"
#include <alloca.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <future>
#include <iostream>
#include <malloc.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
using namespace actor_runtime;
using namespace std::chrono_literals;
struct Options {
    std::string mode;
    std::size_t count, payload, stackKiB, touchKiB, workers, rounds;
};
struct Counts { std::size_t completions=0, tasks=0, waiterKeys=0, waiterRefs=0, timers=0; bool observed=false; };
struct State {
    std::atomic<std::size_t> started{0}, done{0}, received{0}, errors{0};
    std::promise<void> release;
    std::shared_future<void> gate=release.get_future().share();
    bool opened=false;
    void open() { if(!opened) { opened=true; release.set_value(); } }
};
static void waitFor(const std::function<bool()>& condition, const std::shared_ptr<State>& state) {
    const auto end=std::chrono::steady_clock::now()+20s;
    while(!condition()) {
        if(state->errors || std::chrono::steady_clock::now()>end) throw std::runtime_error("memory fixture timeout/error");
        std::this_thread::sleep_for(1ms);
    }
}
static long field(const char* path, const char* key) {
    FILE* f=std::fopen(path,"r"); if(!f) throw std::runtime_error("cannot read proc field");
    char line[512]; long value=-1;
    while(std::fgets(line,sizeof(line),f)) if(std::strncmp(line,key,std::strlen(key))==0) {
        if(std::sscanf(line+std::strlen(key),"%ld",&value)!=1) value=-1;
        break;
    }
    std::fclose(f); if(value<0) throw std::runtime_error("missing proc field"); return value;
}
static void snapshot(const std::string& stage, ActorSystem* system=nullptr, Counts c={}) {
    std::this_thread::sleep_for(20ms); // Quiescent sample, not a profiler peak estimate.
    const auto rss=field("/proc/self/smaps_rollup","Rss:");
    const auto pss=field("/proc/self/smaps_rollup","Pss:");
    const auto priv=field("/proc/self/smaps_rollup","Private_Dirty:");
    const auto vms=field("/proc/self/status","VmSize:");
    const auto hwm=field("/proc/self/status","VmHWM:");
    const auto threads=field("/proc/self/status","Threads:");
    FILE* maps=std::fopen("/proc/self/maps","r"); if(!maps) throw std::runtime_error("maps unavailable");
    char line[1024]; std::size_t vmas=0; while(std::fgets(line,sizeof(line),maps)) ++vmas; std::fclose(maps);
    const auto mi=mallinfo2(); const auto stats=system ? system->stats() : RuntimeStats{};
    std::cout << "{\"stage\":\"" << stage << "\",\"rss_kib\":" << rss << ",\"pss_kib\":" << pss
      << ",\"private_dirty_kib\":" << priv << ",\"vms_kib\":" << vms << ",\"hwm_kib\":" << hwm
      << ",\"threads\":" << threads << ",\"vmas\":" << vmas << ",\"malloc_used_bytes\":" << mi.uordblks
      << ",\"malloc_free_bytes\":" << mi.fordblks << ",\"malloc_mmap_bytes\":" << mi.hblkhd
      << ",\"live_fibers\":" << stats.liveFibers << ",\"live_stacks\":" << stats.liveStacks
      << ",\"completions\":" << c.completions << ",\"tasks\":" << c.tasks
      << ",\"waiter_keys\":" << c.waiterKeys << ",\"waiter_refs\":" << c.waiterRefs
      << ",\"timers\":" << c.timers << ",\"counts_observed\":" << int(c.observed) << "}" << std::endl;
}
class EmptyActor final : public Actor {
public: explicit EmptyActor(ActorSystem& s):Actor(s){}
private: void onMessage(MessagePtr) override {}
};
__attribute__((noinline)) static void waitWithStack(CoroutineContext& ctx, std::size_t bytes,
                                                  const std::shared_ptr<State>& state) {
    auto* memory=static_cast<volatile unsigned char*>(alloca(bytes ? bytes : 1));
    for(std::size_t i=0;i<bytes;i+=4096) memory[i]=static_cast<unsigned char>(i/4096);
    ++state->started;
    ctx.await(WaitSpec::message({2,NoCorrelation})); ctx.throwIfCancelled();
    for(std::size_t i=0;i<bytes;i+=4096) if(memory[i]!=static_cast<unsigned char>(i/4096)) ++state->errors;
}
class ProbeActor final : public Actor {
public:
    ProbeActor(ActorSystem& s, Options o, std::shared_ptr<State> state):Actor(s),o_(std::move(o)),state_(std::move(state)){}
private:
    void onCoroutineError(FiberId,std::exception_ptr) override { ++state_->errors; }
    void onMessage(MessagePtr m) override {
        if(m->typeId==99) {
            Counts c; c.observed=true; c.completions=currentActor->completions.size(); c.tasks=currentActor->tasks.size();
            c.waiterKeys=currentActor->messageWaiters.size(); c.timers=currentWorker->timers.size();
            for(const auto& entry:currentActor->messageWaiters) c.waiterRefs+=entry.second.size();
            std::any_cast<std::shared_ptr<std::promise<Counts>>>(m->payload)->set_value(c); return;
        }
        if(m->typeId==10) { ++state_->started; state_->gate.wait(); return; }
        if(m->typeId==11) { // Every queued payload is owned and content checked.
            if(o_.payload) {
                const auto& p=std::any_cast<const std::vector<unsigned char>&>(m->payload);
                if(p.size()!=o_.payload || p.front()!=0x5a || p.back()!=0x5a) ++state_->errors;
            }
            ++state_->received; return;
        }
        if(m->typeId!=1) return;
        if(o_.mode=="fibers" || o_.mode=="completed") {
            for(std::size_t i=0;i<o_.count;++i) start([o=o_,s=state_](CoroutineContext& ctx)->std::any {
                if(o.mode=="fibers") waitWithStack(ctx,o.touchKiB*1024,s);
                ++s->done;
                if(o.payload) return std::vector<unsigned char>(o.payload,0x5a);
                return {};
            });
        } else start([o=o_,s=state_](CoroutineContext& ctx)->std::any {
            for(std::size_t i=0;i<o.count;++i) {
                if(o.mode=="tasks") {
                    auto id=ctx.submitBlocking([n=o.payload]()->std::any {return std::vector<unsigned char>(n,0x5a);});
                    auto r=ctx.await(WaitSpec::task(id));
                    if(r.as<std::vector<unsigned char>>().size()!=o.payload) ++s->errors;
                } else if(o.mode=="timers") {
                    ctx.await(WaitSpec::any({WaitSpec::timer(std::chrono::steady_clock::now()+10min),
                                            WaitSpec::timer(std::chrono::steady_clock::now()-1s)}));
                } else if(o.mode=="waiters") {
                    ctx.await(WaitSpec::any({WaitSpec::message({77,i+1}),
                                            WaitSpec::timer(std::chrono::steady_clock::now()-1s)}));
                }
            }
            ++s->started; ctx.await(WaitSpec::message({2,NoCorrelation})); ctx.throwIfCancelled();
            ++s->done; return {};
        });
    }
    Options o_; std::shared_ptr<State> state_;
};
static Counts inspect(ActorRef ref) {
    auto promise=std::make_shared<std::promise<Counts>>(); auto future=promise->get_future();
    ref.send(99,promise);
    if(future.wait_for(20s)!=std::future_status::ready) throw std::runtime_error("inspection timeout");
    return future.get(); // Read indexes only while the actor owns execution.
}
int main(int argc,char** argv) {
    try {
        if(argc!=8) throw std::runtime_error("usage: memory_probe mode count payload stackKiB touchKiB workers rounds");
        Options o{argv[1],std::stoull(argv[2]),std::stoull(argv[3]),std::stoull(argv[4]),std::stoull(argv[5]),std::stoull(argv[6]),std::stoull(argv[7])};
        if(o.count>100000 || o.payload>65536 || o.count*o.payload>128*1024*1024 ||
           o.stackKiB<64 || o.stackKiB>1024 || o.touchKiB+32>o.stackKiB || o.workers>32 || !o.rounds || o.rounds>5 ||
           o.count*o.touchKiB>128*1024 || o.count*o.payload*o.rounds>128*1024*1024)
            throw std::runtime_error("fixture safety bounds exceeded");
        if(o.mode!="idle" && o.mode!="actors" && o.mode!="mailbox" && o.mode!="fibers" &&
           o.mode!="completed" && o.mode!="tasks" && o.mode!="timers" && o.mode!="waiters") throw std::runtime_error("unknown mode");
        if((o.mode=="fibers" || o.mode=="completed") && o.count>3000) throw std::runtime_error("fiber mapping bound exceeded");
        if(o.mode!="idle" && o.mode!="actors" && o.workers!=1) throw std::runtime_error("diagnostic owner introspection requires one worker");
        pthread_attr_t attr; pthread_attr_init(&attr); std::size_t stack=0; pthread_attr_getstacksize(&attr,&stack); pthread_attr_destroy(&attr);
        std::cout << "{\"stage\":\"sizes\",\"ActorControl\":" << sizeof(ActorControl) << ",\"EmptyActor\":" << sizeof(EmptyActor)
          << ",\"ActorRef\":" << sizeof(ActorRef) << ",\"Message\":" << sizeof(Message) << ",\"Fiber\":" << sizeof(Fiber)
          << ",\"Worker\":" << sizeof(Worker) << ",\"Job\":" << sizeof(Job) << ",\"ReadyDispatch\":" << sizeof(ReadyDispatch)
          << ",\"RuntimeWaitNode\":" << sizeof(RuntimeWaitNode) << ",\"Completion\":" << sizeof(Completion)
          << ",\"TimerEntry\":" << sizeof(TimerEntry) << ",\"system_impl\":" << sizeof(ActorSystem::Impl)
          << ",\"pthread_default_stack\":" << stack << ",\"page_size\":" << sysconf(_SC_PAGESIZE) << "}" << std::endl;
        snapshot("process_baseline");
        auto state=std::make_shared<State>();
        ActorSystem::Config config; config.workerCount=o.workers; config.fiberStackSize=o.stackKiB*1024;
        if(o.mode=="mailbox") {
            // This scenario measures per-queued-message memory, so it must lift
            // the bounded-mailbox defaults above the requested depth instead of
            // tripping the admission budget that the P0 work introduced.
            const std::size_t perMessage = o.payload + 512;
            config.mailboxMaxMessages = o.count + 8;
            config.mailboxMaxBytes = perMessage * (o.count + 8);
            config.systemMailboxMaxBytes = config.mailboxMaxBytes * 2;
        }
        auto system=std::make_unique<ActorSystem>(config);
        struct Release {std::shared_ptr<State> s; ~Release(){s->open();}} release{state};
        snapshot("system_idle",system.get());
        if(o.mode=="actors") {
            for(std::size_t round=1;round<=o.rounds;++round) {
                std::vector<ActorRef> refs; refs.reserve(o.count);
                for(std::size_t i=0;i<o.count;++i) refs.push_back(system->registerActor(std::make_unique<EmptyActor>(*system)));
                snapshot("actors_loaded_"+std::to_string(round),system.get());
                for(auto ref:refs) system->stopActor(ref.id()).get();
                std::vector<ActorRef>().swap(refs);
                snapshot("actors_stopped_"+std::to_string(round),system.get());
            }
        } else if(o.mode!="idle") {
            auto actor=system->registerActor(std::make_unique<ProbeActor>(*system,o,state));
            snapshot("actor_ready",system.get(),inspect(actor));
            if(o.mode=="mailbox") {
                actor.send(10,{}); waitFor([&]{return state->started==1;},state);
                for(std::size_t i=0;i<o.count;++i) {
                    if(o.payload) actor.send(11,std::vector<unsigned char>(o.payload,0x5a)); else actor.send(11,{});
                }
                if(state->received) throw std::runtime_error("mailbox was not gated");
                snapshot("mailbox_full",system.get()); state->open();
                waitFor([&]{return state->received==o.count;},state);
                snapshot("drained",system.get(),inspect(actor));
            } else if(o.mode=="completed") {
                for(std::size_t round=1;round<=o.rounds;++round) {
                    actor.send(1,{}); waitFor([&]{return state->done==round*o.count && system->stats().liveStacks==0;},state);
                    // P0 releases root results instead of caching every finished
                    // coroutine, so the retained table must plateau rather than
                    // grow with the number of completed rounds. The old check
                    // asserted the pre-P0 leak (completions == round * count).
                    auto c=inspect(actor); if(c.completions>o.count) throw std::runtime_error("retained completions grew with rounds");
                    snapshot("completed_"+std::to_string(round),system.get(),c);
                }
            } else {
                actor.send(1,{}); const auto expected=o.mode=="fibers" ? o.count : 1;
                waitFor([&]{return state->started==expected;},state);
                auto c=inspect(actor); snapshot("held",system.get(),c);
                if(system->stats().liveFibers!=expected) throw std::runtime_error("live fiber count mismatch");
                if(o.mode=="tasks" && c.tasks!=o.count) throw std::runtime_error("task count mismatch");
                // P0 unregisters abandoned waits, so a held wait never needs more
                // heap entries than were requested; the earlier exact ==count
                // check encoded the pre-P0 retention instead.
                if(o.mode=="timers" && c.timers>o.count) throw std::runtime_error("timer count mismatch");
                // Each Any() race between a message registration and an already
                // expired timer leaves the losing message registration behind in
                // the pre-P0 runtime. P0 unregisters losers, so only the single
                // final pending wait survives; count+1 asserted the accumulation.
                if(o.mode=="waiters" && (c.waiterKeys!=1 || c.waiterRefs!=1)) throw std::runtime_error("waiter count mismatch");
                actor.send(2,{}); waitFor([&]{return state->done==expected && system->stats().liveStacks==0;},state);
                auto after=inspect(actor);
                // The contract that matters is cleanup: once the wait ends, no
                // timer entry may stay behind. Expecting count here asserted the
                // pre-P0 leak of 10000 lingering entries.
                if(after.tasks || (o.mode=="timers" && after.timers!=0) ||
                   (o.mode=="waiters" && (after.waiterRefs!=0 || after.waiterKeys!=0))) throw std::runtime_error("release counts mismatch");
                snapshot("released",system.get(),after);
            }
            system->stopActor(actor.id()).get(); snapshot("actor_stopped",system.get());
            if(o.mode=="timers") {
                auto observer=system->registerActor(std::make_unique<ProbeActor>(*system,o,state));
                auto counts=inspect(observer);
                if(counts.timers!=0) throw std::runtime_error("post-stop timer count mismatch");
                snapshot("post_stop_observer",system.get(),counts);
                system->stopActor(observer.id()).get();
            }
        }
        if(state->errors) throw std::runtime_error("payload/stack check failed");
        system->shutdown(); snapshot("shutdown",system.get());
        if(system->stats().liveFibers || system->stats().liveStacks) throw std::runtime_error("live stack after shutdown");
        system.reset(); snapshot("destroyed");
        malloc_trim(0); snapshot("trimmed_diagnostic"); // Never proposed as a hidden production fix.
        std::cout << "{\"stage\":\"pass\"}" << std::endl;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
