// 复杂业务模拟：订单履约（所有“远程服务”都在进程内，不使用网络）。
//
// 1. 报价、库存、风控并行执行，根协程用 All 收齐三个必要结果。
//    报价协程再创建两个供应商子协程，用 Any 选最快报价，显式 cancel
//    慢供应商，并用 AllSettled 确认输家的清理结束。风控也包含一层子协程。
// 2. 支付协程嵌套“银行授权”和“合规检查”，根协程使用
//    Any(支付完成, 用户撤单消息, 交易超时) 决定成交还是提前取消。
//    银行模拟器收到授权请求后才发送撤单消息，因此取消发生在业务进行中。
// 3. 无论成功还是失败，都执行审计与通知；通知故意抛异常，AllSettled
//    保留成功/异常/超时结果，不使订单收尾被某个通知失败打断。
// 4. 同一程序也是集成测试：分别以 1、4 个 worker 跑成交、缺货、
//    支付超时、用户撤单四条路径，校验最终状态、取消状态与 RAII 清理。

#include "actor_runtime/actor_system.h"
#include "actor_runtime/coroutine_context.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace ar = actor_runtime;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr ar::TypeId Begin = 200, Query = 201, Reply = 202, CancelOrder = 203;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

enum class Mode { Success, OutOfStock, PaymentTimeout, CustomerCancel };
struct QueryData {
    int value;
    bool fail = false;
    bool silent = false;
    bool cancelOrder = false;
};
struct Counters { std::atomic<int> opened{0}, closed{0}, cancelled{0}; };
struct Resource {
    std::shared_ptr<Counters> counts;
    explicit Resource(std::shared_ptr<Counters> c) : counts(std::move(c)) { ++counts->opened; }
    ~Resource() { ++counts->closed; }
};
struct Report { std::string status; bool loserCancelled = false; bool paymentCancelled = false; };

// 服务 Actor 保留请求 id，响应 correlationId 必须指回该请求。
class Services final : public ar::Actor {
public:
    explicit Services(ar::ActorSystem& s) : Actor(s), system_(s) {}
private:
    void onMessage(ar::MessagePtr m) override {
        if (m->typeId != Query) return;
        auto request = std::any_cast<QueryData>(m->payload);
        auto target = system_.ref(m->sender);
        if (request.cancelOrder) target.send(CancelOrder, {});
        if (request.silent) return;
        start([request, target, correlation = m->id](ar::CoroutineContext& c) -> std::any {
            c.await(ar::WaitSpec::timer(Clock::now() + 1ms));
            c.throwIfCancelled();
            target.send(Reply, request.fail ? -1 : request.value, correlation);
            return {};
        });
    }
    ar::ActorSystem& system_;
};

int callService(ar::CoroutineContext& c, ar::ActorRef service, QueryData query,
                std::shared_ptr<Counters> counts) {
    Resource resource(counts); // 模拟连接/库存预留句柄，取消必须正常展开析构。
    auto id = service.send(Query, query);
    auto result = c.await(ar::WaitSpec::any({
        ar::WaitSpec::message({Reply, id}), ar::WaitSpec::timer(Clock::now() + 5s)}));
    if (c.cancellationRequested()) { ++counts->cancelled; c.throwIfCancelled(); }
    if (result.reason == ar::WakeReason::Timeout) throw std::runtime_error("service timeout");
    auto value = std::any_cast<int>(result.as<ar::MessagePtr>()->payload);
    if (value < 0) throw std::runtime_error("inventory unavailable");
    return value;
}

class Order final : public ar::Actor {
public:
    Order(ar::ActorSystem& s, ar::ActorRef service, Mode mode,
          std::shared_ptr<Counters> counts, std::promise<Report>& done)
        : Actor(s), service_(service), mode_(mode), counts_(std::move(counts)), done_(done) {}
private:
    void onCoroutineError(ar::FiberId, std::exception_ptr error) override { done_.set_exception(error); }
    void onMessage(ar::MessagePtr m) override {
        if (m->typeId != Begin) return; // 响应与撤单消息也会经过这里，不重复创建订单。
        start([this](ar::CoroutineContext& c) -> std::any {
            Report report;
            auto quote = c.spawn([service = service_, counts = counts_](ar::CoroutineContext& q) -> std::any {
                auto fast = q.spawn([service, counts](ar::CoroutineContext& v) -> std::any {
                    return callService(v, service, {199}, counts);
                }, ar::ChildLifetime::CancelWithParent);
                auto slow = q.spawn([service, counts](ar::CoroutineContext& v) -> std::any {
                    return callService(v, service, {210, false, true}, counts);
                }, ar::ChildLifetime::CancelWithParent);
                auto first = q.await(ar::WaitSpec::any({ar::WaitSpec::child(fast), ar::WaitSpec::child(slow)}));
                require(first.branchIndex == 0 && first.as<int>() == 199, "wrong winning quote");
                q.cancel(slow);
                auto settled = q.await(ar::WaitSpec::allSettled({ar::WaitSpec::child(fast), ar::WaitSpec::child(slow)}));
                require(settled.children[1].reason == ar::WakeReason::Cancelled, "quote loser not cancelled");
                return 199;
            }, ar::ChildLifetime::CancelWithParent);
            auto inventory = c.spawn([this](ar::CoroutineContext& i) -> std::any {
                return callService(i, service_, {1, mode_ == Mode::OutOfStock}, counts_);
            }, ar::ChildLifetime::CancelWithParent);
            auto risk = c.spawn([](ar::CoroutineContext& r) -> std::any {
                auto score = r.spawn([](ar::CoroutineContext& s) -> std::any {
                    s.await(ar::WaitSpec::timer(Clock::now() + 1ms));
                    s.throwIfCancelled();
                    return 7;
                }, ar::ChildLifetime::CancelWithParent);
                return r.await(ar::WaitSpec::child(score)).as<int>();
            }, ar::ChildLifetime::CancelWithParent);
            auto prerequisites = c.await(ar::WaitSpec::all({
                ar::WaitSpec::child(quote), ar::WaitSpec::child(inventory), ar::WaitSpec::child(risk)}));
            require(prerequisites.children[0].as<int>() == 199 && prerequisites.children[2].as<int>() == 7,
                    "prerequisite order/values changed");
            report.loserCancelled = true;
            if (prerequisites.reason == ar::WakeReason::Error) {
                require(mode_ == Mode::OutOfStock && prerequisites.error, "unexpected prerequisite error");
                report.status = "缺货，订单拒绝";
            } else {
                auto payment = c.spawn([this](ar::CoroutineContext& p) -> std::any {
                    auto bank = p.spawn([this](ar::CoroutineContext& b) -> std::any {
                        bool silent = mode_ != Mode::Success;
                        return callService(b, service_, {1, false, silent, mode_ == Mode::CustomerCancel}, counts_);
                    }, ar::ChildLifetime::CancelWithParent);
                    auto compliance = p.spawn([counts = counts_](ar::CoroutineContext& f) -> std::any {
                        Resource resource(counts);
                        f.await(ar::WaitSpec::timer(Clock::now() + 2ms));
                        f.throwIfCancelled();
                        return true;
                    }, ar::ChildLifetime::CancelWithParent);
                    auto approvals = p.await(ar::WaitSpec::all({ar::WaitSpec::child(bank), ar::WaitSpec::child(compliance)}));
                    p.throwIfCancelled();
                    approvals.rethrowIfError();
                    return true;
                }, ar::ChildLifetime::CancelWithParent);
                auto decision = c.await(ar::WaitSpec::any({
                    ar::WaitSpec::child(payment), ar::WaitSpec::message({CancelOrder, 0}),
                    ar::WaitSpec::timer(Clock::now() + (mode_ == Mode::PaymentTimeout ? 20ms : 2s))}));
                if (decision.branchIndex == 0) {
                    require(decision.as<bool>(), "payment rejected");
                    report.status = "成交";
                } else {
                    c.cancel(payment); // 同时取消该支付协程的 CancelWithParent 孙协程。
                    auto cleanup = c.await(ar::WaitSpec::child(payment));
                    require(cleanup.reason == ar::WakeReason::Cancelled, "payment did not unwind");
                    report.paymentCancelled = true;
                    report.status = decision.branchIndex == 1 ? "用户撤单" : "支付超时";
                }
            }
            auto audit = c.spawn([](ar::CoroutineContext&) -> std::any { return std::string("audit saved"); },
                                 ar::ChildLifetime::CancelWithParent);
            auto notification = c.spawn([](ar::CoroutineContext&) -> std::any { throw std::runtime_error("email unavailable"); },
                                        ar::ChildLifetime::CancelWithParent);
            auto cleanup = c.await(ar::WaitSpec::allSettled({
                ar::WaitSpec::child(audit), ar::WaitSpec::child(notification), ar::WaitSpec::timer(Clock::now())}));
            require(cleanup.reason == ar::WakeReason::Settled && cleanup.children[0].as<std::string>() == "audit saved" &&
                    cleanup.children[1].reason == ar::WakeReason::Error && cleanup.children[2].reason == ar::WakeReason::Timeout,
                    "settlement lost individual outcomes");
            done_.set_value(report);
            return {};
        });
    }
    ar::ActorRef service_;
    Mode mode_;
    std::shared_ptr<Counters> counts_;
    std::promise<Report>& done_;
};

int main() {
    try {
        for (std::size_t workers : {1U, 4U}) {
            for (Mode mode : {Mode::Success, Mode::OutOfStock, Mode::PaymentTimeout, Mode::CustomerCancel}) {
                std::promise<Report> done;
                auto future = done.get_future();
                auto counts = std::make_shared<Counters>();
                ar::ActorSystem& system = ar::ActorSystem::start({workers});
                auto service = system.registerActor(std::make_unique<Services>(system));
                auto order = system.registerActor(std::make_unique<Order>(system, service, mode, counts, done));
                order.send(Begin, {});
                require(future.wait_for(5s) == std::future_status::ready, "order stalled");
                auto result = future.get();
                // 等所有协程栈退出后再断言，然后结束进程级单例。
                const auto deadline = Clock::now() + 2s;
                while ((system.stats().liveFibers != 0 || system.stats().liveStacks != 0) &&
                    Clock::now() < deadline) {
                    std::this_thread::sleep_for(1ms);
                }
                const auto stats = system.stats();
                ar::ActorSystem::stop();
                const char* expected = mode == Mode::Success ? "成交" : mode == Mode::OutOfStock ? "缺货，订单拒绝" :
                                       mode == Mode::PaymentTimeout ? "支付超时" : "用户撤单";
                require(result.status == expected && result.loserCancelled, "wrong business outcome");
                bool cancelledPayment = mode == Mode::PaymentTimeout || mode == Mode::CustomerCancel;
                require(result.paymentCancelled == cancelledPayment, "wrong payment cancellation outcome");
                require(counts->cancelled.load() == (cancelledPayment ? 2 : 1), "nested service cancellation not observed");
                require(counts->opened.load() == counts->closed.load(), "RAII resource leaked");
                require(stats.liveFibers == 0 && stats.liveStacks == 0, "runtime resource leaked");
                std::cout << "[PASS] workers=" << workers << "，订单结果=" << result.status
                          << "，已释放资源=" << counts->closed << '\n';
            }
        }
    } catch (const std::exception& e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
