#include "common.h"
#include <caf/all.hpp>
#include <memory>

caf::behavior echo(caf::event_based_actor* self) {
    return {[self](caf::actor reply,uint64_t sequence,uint64_t sent,const std::string& body) {
        self->send(reply,sequence,sent,body);
    }};
}
caf::behavior client(caf::event_based_actor* self,caf::actor server,bench::Client* data) {
    auto issue=[self,server,data] {
        auto p=data->packet();
        self->send(server,caf::actor_cast<caf::actor>(self),p.sequence,p.sent,p.body);
    };
    return {
        [data,issue](uint64_t count,bool measured) {
            try {
                data->reset({count,measured});
                for(size_t i=0;i<data->harness.config.window && data->issued<data->target;++i) issue();
            } catch(const std::exception& e) { data->harness.fail(e.what()); }
        },
        [data,issue](uint64_t sequence,uint64_t sent,const std::string& body) {
            try {
                data->accept(sequence,sent,body);
                if(data->issued<data->target) issue();
                if(data->received==data->target) data->harness.complete(data->id);
            } catch(const std::exception& e) { data->harness.fail(e.what()); }
        }
    };
}
int main(int argc,char** argv) {
    try {
        caf::core::init_global_meta_objects();
        auto c=bench::parse(argc,argv); bench::Harness h(c);
        std::vector<std::unique_ptr<bench::Client>> data;
        caf::actor_system_config options;
        options.set("caf.scheduler.max-threads",c.workers);
        options.set("caf.scheduler.max-throughput",c.batch);
        caf::actor_system system(options);
        std::vector<caf::actor> actors,clients;
        struct Cleanup {
            caf::actor_system& system; std::vector<caf::actor>& actors;
            ~Cleanup() { for(auto& a:actors) caf::anon_send_exit(a,caf::exit_reason::user_shutdown); system.await_all_actors_done(); }
        } cleanup{system,actors};
        auto server=system.spawn(echo); actors.push_back(server);
        for(size_t i=0;i<c.clients;++i) {
            if(i && !c.hotspot) { server=system.spawn(echo); actors.push_back(server); }
            data.push_back(std::make_unique<bench::Client>(h,i));
            auto a=system.spawn(client,server,data.back().get()); clients.push_back(a); actors.push_back(a);
        }
        for(bool measured:{false,true}) {
            h.beginPhase();
            for(auto& a:clients) caf::anon_send(a,static_cast<uint64_t>(measured?c.rounds:c.warmup),measured);
            h.wait();
        }
        std::vector<double> samples;
        for(auto& d:data) samples.insert(samples.end(),d->samples.begin(),d->samples.end());
        h.report("caf-0.19.6","event",std::move(samples));
    } catch(const std::exception& e) { std::cerr<<"benchmark failed: "<<e.what()<<'\n'; return 1; }
}
