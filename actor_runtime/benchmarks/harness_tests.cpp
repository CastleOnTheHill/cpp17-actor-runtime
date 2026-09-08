#include "common.h"
#include <functional>
#include <sstream>

void check(bool condition) { if(!condition) throw std::runtime_error("test assertion failed"); }
void rejects(const std::function<void()>& f) {
    bool caught=false;
    try { f(); } catch(const std::exception&) { caught=true; }
    check(caught);
}
int main() {
    bench::Config config; config.rounds=3; config.warmup=1; config.sample=2; config.timeout=1;
    bench::Harness h(config); bench::Client d(h,0);
    d.reset({3,true});
    auto p=d.packet(); check(p.sequence==0 && p.sent && p.body.size()==64);
    auto bad=p; bad.body="corrupt"; rejects([&]{d.accept(bad);});
    bad=p; bad.sequence=1; rejects([&]{d.accept(bad);});
    d.accept(p); rejects([&]{d.accept(p);});
    p=d.packet(); check(!p.sent); d.accept(p);
    p=d.packet(); check(p.sent); d.accept(p);
    check(d.received==3 && d.samples.size()==2);
    d.reset({1,false}); p=d.packet(); check(!p.sent && p.sequence==0 && d.samples.empty());
    h.beginPhase(); h.complete(0); h.wait();
    h.complete(0); rejects([&]{h.wait();});
    h.beginPhase(); h.complete(1); rejects([&]{h.wait();});
    h.beginPhase(); h.fail("injected failure"); rejects([&]{h.wait();});
    h.beginPhase(); rejects([&]{h.wait();}); // Real deadline path, not a mock.
    char app[]="bench", option[]="--workers", zero[]="0", unknown[]="--invalid", number[]="1";
    char* a[]={app,option,zero}; rejects([&]{bench::parse(3,a);});
    char* b[]={app,option}; rejects([&]{bench::parse(2,b);});
    char* c[]={app,unknown,number}; rejects([&]{bench::parse(3,c);});
    std::cout<<"PASS harness: corruption, sequence, duplicate, sample, reset, failure, timeout, CLI\n";
}
