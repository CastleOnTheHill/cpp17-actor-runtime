#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/resource.h>
namespace bench {
using Clock=std::chrono::steady_clock;
inline uint64_t now() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); }
struct Config {
    size_t workers=1,clients=1,window=1,payload=64,rounds=10000,warmup=1000,sample=64,batch=1;
    unsigned timeout=60;
    bool hotspot=false;
};
inline Config parse(int argc,char** argv) {
    Config c;
    for(int i=1;i<argc;++i) {
        std::string k=argv[i];
        if(k=="--hotspot") { c.hotspot=true; continue; }
        if(++i==argc) throw std::invalid_argument("option requires value");
        std::string s=argv[i]; size_t used=0;
        if(s.empty() || s[0]=='-') throw std::invalid_argument("nonnegative integer required");
        auto v=std::stoull(s,&used);
        if(used!=s.size() || v>100000000) throw std::invalid_argument("invalid number");
        if(k=="--workers") c.workers=v;
        else if(k=="--clients") c.clients=v;
        else if(k=="--window") c.window=v;
        else if(k=="--payload") c.payload=v;
        else if(k=="--rounds") c.rounds=v;
        else if(k=="--warmup") c.warmup=v;
        else if(k=="--sample") c.sample=v;
        else if(k=="--batch") c.batch=v;
        else if(k=="--timeout") c.timeout=static_cast<unsigned>(v);
        else throw std::invalid_argument("unknown option: "+k);
    }
    if(!c.workers || c.workers>64 || !c.clients || c.clients>1024 || !c.window || c.window>4096 || !c.rounds ||
       !c.warmup || !c.sample || !c.batch || !c.timeout || c.payload>60000) throw std::invalid_argument("unsupported configuration");
    return c;
}
struct Packet { uint64_t sequence=0,sent=0; std::string body; };
struct Begin { uint64_t count; bool measured; };
struct Harness {
    explicit Harness(Config c): config(c),expected(c.payload,'x'),seen(c.clients,false) {}
    Config config; const std::string expected;
    std::mutex mutex; std::condition_variable ready; std::vector<bool> seen;
    size_t finished=0; std::string error; uint64_t startTime=0,endTime=0; rusage before{},after{};
    void beginPhase() {
        std::lock_guard<std::mutex> lock(mutex); finished=0;
        std::fill(seen.begin(),seen.end(),false); error.clear(); getrusage(RUSAGE_SELF,&before); startTime=now(); endTime=0;
    }
    void complete(size_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        if(id>=seen.size() || seen[id]) { error="duplicate client completion"; ready.notify_one(); return; }
        seen[id]=true;
        if(++finished==config.clients) { endTime=now(); getrusage(RUSAGE_SELF,&after); ready.notify_one(); }
    }
    void fail(std::string s) { std::lock_guard<std::mutex> lock(mutex); error=std::move(s); ready.notify_one(); }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        if(!ready.wait_for(lock,std::chrono::seconds(config.timeout),[&]{return finished==config.clients || !error.empty();}))
            throw std::runtime_error("benchmark phase timed out");
        if(!error.empty()) throw std::runtime_error(error);
    }
    void report(const char* framework,const char* mode,std::vector<double> samples) const {
        if(samples.empty() || endTime<=startTime) throw std::runtime_error("empty measurement");
        std::sort(samples.begin(),samples.end());
        auto pct=[&](double q){return samples[static_cast<size_t>(q*static_cast<double>(samples.size()-1))];};
        const double seconds=static_cast<double>(endTime-startTime)/1e9;
        auto cpu=[](const rusage& r){return r.ru_utime.tv_sec+r.ru_stime.tv_sec+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1e6;};
        std::cout.precision(10);
        std::cout<<"{\"framework\":\""<<framework<<"\",\"mode\":\""<<mode
          <<"\",\"workers\":"<<config.workers<<",\"clients\":"<<config.clients<<",\"window\":"<<config.window
          <<",\"payload_bytes\":"<<config.payload<<",\"batch\":"<<config.batch<<",\"hotspot\":"<<(config.hotspot?"true":"false")
          <<",\"rounds_per_client\":"<<config.rounds<<",\"warmup_per_client\":"<<config.warmup
          <<",\"round_trips\":"<<config.rounds*config.clients<<",\"seconds\":"<<seconds
          <<",\"rps\":"<<static_cast<double>(config.rounds*config.clients)/seconds
          <<",\"messages_per_second\":"<<2.0*static_cast<double>(config.rounds*config.clients)/seconds
          <<",\"rtt_samples\":"<<samples.size()<<",\"rtt_p50_us\":"<<pct(.50)<<",\"rtt_p95_us\":"<<pct(.95)<<",\"rtt_p99_us\":"<<pct(.99)
          <<",\"cpu_seconds\":"<<cpu(after)-cpu(before)<<",\"peak_rss_kib\":"<<after.ru_maxrss
          <<",\"voluntary_context_switches\":"<<after.ru_nvcsw-before.ru_nvcsw
          <<",\"involuntary_context_switches\":"<<after.ru_nivcsw-before.ru_nivcsw<<"}\n";
    }
};
struct Client {
    Harness& harness; size_t id; uint64_t target=0,issued=0,received=0; bool measured=false; std::vector<double> samples;
    Client(Harness& h,size_t i): harness(h),id(i) {}
    Packet packet() {
        Packet p; p.sequence=issued++; p.body=harness.expected;
        if(measured && p.sequence%harness.config.sample==0) p.sent=now(); return p;
    }
    void reset(Begin b) {
        target=b.count; issued=received=0; measured=b.measured; samples.clear(); samples.reserve(target/harness.config.sample+1);
    }
    void accept(uint64_t sequence,uint64_t sent,const std::string& body) {
        if(sequence!=received || body!=harness.expected) throw std::runtime_error("echo sequence/payload mismatch");
        if(sent) samples.push_back(static_cast<double>(now()-sent)/1000.0); ++received;
    }
    void accept(const Packet& p) { accept(p.sequence,p.sent,p.body); }
};
}
