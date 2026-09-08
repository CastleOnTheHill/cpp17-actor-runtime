#include "common.h"
#include "actor_runtime/c_api.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

struct Fd {
    int value=-1;
    explicit Fd(int fd):value(fd) { if(fd<0) throw std::runtime_error("socket/pipe failed"); }
    ~Fd() { if(value>=0) ::close(value); }
};
struct Address { sockaddr_un value{}; socklen_t size; };
Address address(const std::string& name) {
    Address a{}; a.value.sun_family=AF_UNIX;
    if(name.size()+1>sizeof(a.value.sun_path)) throw std::runtime_error("Unix name too long");
    std::memcpy(a.value.sun_path+1,name.data(),name.size());
    a.size=static_cast<socklen_t>(offsetof(sockaddr_un,sun_path)+1+name.size()); return a;
}
void bindSocket(int fd,const std::string& name) {
    auto a=address(name);
    if(::bind(fd,reinterpret_cast<sockaddr*>(&a.value),a.size)) throw std::runtime_error("Unix bind failed");
    timeval timeout{5,0};
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))) throw std::runtime_error("socket timeout failed");
}
std::atomic<bool> serverStop{false},serverError{false};
void cEcho(void*,ar_system* system,const ar_message* request) {
    if(ar_reply(system,request,2,request->data,request->size)!=AR_OK) serverError=true;
    if(request->type==9) serverStop=true;
}
void server(const bench::Config& c,const std::string& name,int signal) {
#ifdef BENCH_RAW_UNIX
    Fd fd(::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0)); bindSocket(fd.value,name);
    if(::write(signal,"R",1)!=1) throw std::runtime_error("readiness write"); ::close(signal);
    std::vector<unsigned char> bytes(c.payload+16);
    for(;;) {
        sockaddr_un peer{}; socklen_t length=sizeof(peer);
        auto n=::recvfrom(fd.value,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&peer),&length);
        if(n<0) { if(errno==EINTR || errno==EAGAIN) continue; throw std::runtime_error("server receive failed"); }
        if(!n) break;
        if(static_cast<size_t>(n)!=bytes.size() || ::sendto(fd.value,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&peer),length)!=n)
            throw std::runtime_error("raw echo size/send failed");
    }
#else
    ar_system* system=nullptr; uint64_t actor=0;
    if(ar_system_create(name.c_str(),c.workers,&system)!=AR_OK) throw std::runtime_error(ar_last_error());
    std::unique_ptr<ar_system,decltype(&ar_system_destroy)> owner(system,ar_system_destroy);
    if(ar_actor_register(system,"echo",cEcho,nullptr,nullptr,&actor)!=AR_OK) throw std::runtime_error(ar_last_error());
    if(::write(signal,"R",1)!=1) throw std::runtime_error("readiness write"); ::close(signal);
    while(!serverStop && !serverError) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(serverError) throw std::runtime_error("C server reply failed");
#endif
}
int main(int argc,char** argv) {
    try {
        auto c=bench::parse(argc,argv); c.hotspot=true;
        if(c.window!=1 || c.batch!=1 || c.clients>64) throw std::invalid_argument("IPC uses window=1, batch=1, clients<=64");
#ifdef BENCH_RAW_UNIX
        if(c.workers!=1) throw std::invalid_argument("raw Unix server has one thread; use workers=1");
#endif
        const auto name="bench_echo_"+std::to_string(::getpid());
        int pipefd[2]; if(::pipe(pipefd)) throw std::runtime_error("pipe failed");
        const auto child=::fork();
        if(child<0) throw std::runtime_error("fork failed");
        if(!child) {
            ::close(pipefd[0]);
            try { server(c,name,pipefd[1]); std::exit(0); }
            catch(const std::exception& e) { std::cerr<<"server failed: "<<e.what()<<'\n'; std::exit(1); }
        }
        ::close(pipefd[1]); Fd ready(pipefd[0]);
        struct Child {
            pid_t pid;
            ~Child() { if(pid>0) { ::kill(pid,SIGTERM); while(::waitpid(pid,nullptr,0)<0 && errno==EINTR) {} } }
        } cleanup{child};
        pollfd pfd{ready.value,POLLIN,0}; char byte=0;
        if(::poll(&pfd,1,5000)!=1 || ::read(ready.value,&byte,1)!=1 || byte!='R') throw std::runtime_error("server readiness timed out");
        bench::Harness h(c);
        std::vector<std::unique_ptr<bench::Client>> data;
#ifndef BENCH_RAW_UNIX
        ar_system* system=nullptr;
        if(ar_system_create((name+"_client").c_str(),c.workers,&system)!=AR_OK) throw std::runtime_error(ar_last_error());
        std::unique_ptr<ar_system,decltype(&ar_system_destroy)> owner(system,ar_system_destroy);
        std::vector<std::unique_ptr<ar_client,decltype(&ar_client_destroy)>> bridges;
#else
        std::vector<std::unique_ptr<Fd>> sockets;
#endif
        for(size_t i=0;i<c.clients;++i) {
            data.push_back(std::make_unique<bench::Client>(h,i));
#ifndef BENCH_RAW_UNIX
            ar_client* bridge=nullptr;
            if(ar_client_create(system,("client_"+std::to_string(i)).c_str(),&bridge)!=AR_OK) throw std::runtime_error(ar_last_error());
            bridges.emplace_back(bridge,ar_client_destroy);
#else
            auto fd=std::make_unique<Fd>(::socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0));
            bindSocket(fd->value,name+"_client_"+std::to_string(i)); auto a=address(name);
            if(::connect(fd->value,reinterpret_cast<sockaddr*>(&a.value),a.size)) throw std::runtime_error("connect failed");
            sockets.push_back(std::move(fd));
#endif
        }
        for(bool measured:{false,true}) {
            h.beginPhase(); std::vector<std::thread> threads;
            struct Join {
                std::vector<std::thread>& threads;
                ~Join() { for(auto& t:threads) if(t.joinable()) t.join(); }
            } join{threads};
            for(size_t i=0;i<c.clients;++i) threads.emplace_back([&,i] {
                try {
                    auto& d=*data[i]; d.reset({measured?c.rounds:c.warmup,measured});
                    std::vector<unsigned char> input(c.payload+16,'x'),output(input.size());
                    while(d.issued<d.target) {
                        auto packet=d.packet();
                        std::memcpy(input.data(),&packet.sequence,8); std::memcpy(input.data()+8,&packet.sent,8);
#ifndef BENCH_RAW_UNIX
                        size_t size=0;
                        auto status=ar_client_request(bridges[i].get(),name.c_str(),"echo",1,input.data(),input.size(),2,5000,output.data(),output.size(),&size);
                        if(status!=AR_OK || size!=input.size()) throw std::runtime_error(std::string("C request failed: ")+ar_last_error());
#else
                        if(::send(sockets[i]->value,input.data(),input.size(),0)!=static_cast<ssize_t>(input.size()) ||
                           ::recv(sockets[i]->value,output.data(),output.size(),0)!=static_cast<ssize_t>(output.size())) throw std::runtime_error("raw request failed");
#endif
                        if(output!=input) throw std::runtime_error("IPC echo bytes mismatch");
                        d.accept(packet);
                    }
                    h.complete(i);
                } catch(const std::exception& e) { h.fail(e.what()); }
            });
            for(auto& thread:threads) thread.join();
            h.wait();
        }
        std::vector<double> samples;
        for(const auto& d:data) samples.insert(samples.end(),d->samples.begin(),d->samples.end());
#ifdef BENCH_RAW_UNIX
        h.report("raw-unix-dgram","ipc",std::move(samples));
        if(::send(sockets[0]->value,nullptr,0,0)!=0) throw std::runtime_error("stop send failed");
#else
        h.report("actor-runtime-c-api","ipc",std::move(samples));
        size_t size=0;
        if(ar_client_request(bridges[0].get(),name.c_str(),"echo",9,nullptr,0,2,5000,nullptr,0,&size)!=AR_OK) throw std::runtime_error("stop request failed");
#endif
        int status=0;
        if(::waitpid(child,&status,0)!=child) throw std::runtime_error("waitpid failed");
        cleanup.pid=-1;
        if(!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("server unsuccessful");
    } catch(const std::exception& e) { std::cerr<<"benchmark failed: "<<e.what()<<'\n'; return 1; }
}
