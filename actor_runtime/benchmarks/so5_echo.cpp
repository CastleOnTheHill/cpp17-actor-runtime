#include "common.h"
#include <so_5/all.hpp>
#include <so_5/disp/thread_pool/pub.hpp>
#include <future>
#include <memory>
struct Request final: so_5::message_t {
    so_5::mbox_t reply; bench::Packet packet;
    Request(so_5::mbox_t r,bench::Packet p): reply(std::move(r)),packet(std::move(p)) {}
};
struct Response final: so_5::message_t {
    bench::Packet packet;
    explicit Response(bench::Packet p): packet(std::move(p)) {}
};
struct Start final: so_5::message_t {
    bench::Begin begin;
    Start(uint64_t count,bool measured): begin{count,measured} {}
};
class Echo final: public so_5::agent_t {
public:
    Echo(context_t ctx,bench::Harness& h): so_5::agent_t(ctx),h_(h) {}
    void so_define_agent() override {
        so_subscribe_self().event([this](const Request& r) {
            try { so_5::send<Response>(r.reply,r.packet); } catch(const std::exception& e) { h_.fail(e.what()); }
        });
    }
private: bench::Harness& h_;
};
class Client final: public so_5::agent_t {
public:
    Client(context_t ctx,so_5::mbox_t server,bench::Client& d): so_5::agent_t(ctx),server_(std::move(server)),data_(d) {}
    void so_define_agent() override {
        so_subscribe_self().event([this](const Start& start) {
            try {
                data_.reset(start.begin);
                for(size_t i=0;i<data_.harness.config.window && data_.issued<data_.target;++i) issue();
            } catch(const std::exception& e) { data_.harness.fail(e.what()); }
        });
        so_subscribe_self().event([this](const Response& r) {
            try {
                data_.accept(r.packet);
                if(data_.issued<data_.target) issue();
                if(data_.received==data_.target) data_.harness.complete(data_.id);
            } catch(const std::exception& e) { data_.harness.fail(e.what()); }
        });
    }
private:
    void issue() { so_5::send<Request>(server_,so_direct_mbox(),data_.packet()); }
    so_5::mbox_t server_; bench::Client& data_;
};
int main(int argc,char** argv) {
    try {
        auto c=bench::parse(argc,argv); bench::Harness h(c);
        std::vector<std::unique_ptr<bench::Client>> data;
        std::vector<so_5::mbox_t> clients;
        std::promise<void> initialized; auto started=initialized.get_future();
        so_5::wrapped_env_t system([&](so_5::environment_t& env) {
            try {
                using namespace so_5::disp::thread_pool;
                auto pool=make_dispatcher(env,c.workers);
                env.introduce_coop(pool.binder(bind_params_t{}.fifo(fifo_t::individual).max_demands_at_once(c.batch)),[&](so_5::coop_t& coop) {
                    auto server=coop.make_agent<Echo>(h)->so_direct_mbox();
                    for(size_t i=0;i<c.clients;++i) {
                        if(i && !c.hotspot) server=coop.make_agent<Echo>(h)->so_direct_mbox();
                        data.push_back(std::make_unique<bench::Client>(h,i));
                        clients.push_back(coop.make_agent<Client>(server,*data.back())->so_direct_mbox());
                    }
                });
                initialized.set_value();
            } catch(...) { initialized.set_exception(std::current_exception()); }
        });
        started.get();
        for(bool measured:{false,true}) {
            h.beginPhase();
            for(auto& a:clients) so_5::send<Start>(a,measured?c.rounds:c.warmup,measured);
            h.wait();
        }
        std::vector<double> samples;
        for(auto& d:data) samples.insert(samples.end(),d->samples.begin(),d->samples.end());
        h.report("sobjectizer-5.8.3","event",std::move(samples));
    } catch(const std::exception& e) { std::cerr<<"benchmark failed: "<<e.what()<<'\n'; return 1; }
}
