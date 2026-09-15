// Resource lifecycle diagnostics complement the public wait_semantics_tests.
// Reads occur on the owning worker while this Actor holds execution.
#include "internal.h"
#include <iostream>
using namespace actor_runtime;
using namespace std::chrono_literals;
static void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
class CleanupActor final : public Actor {
public:
    CleanupActor(ActorSystem& system, std::shared_ptr<std::promise<void>> done)
        : Actor(system), done_(std::move(done)) {}
private:
    static void clean() {
        check(currentActor->messageWaiters.empty(), "message registration retained");
        check(currentWorker->timers.empty(), "losing timer retained");
        // Ten thousand cancelled long timers must not leave the heap holding a
        // capacity proportional to the historical peak. The queue shrinks once
        // the live set drops below a quarter of the capacity, so the bound stays
        // far below the number of cancellations this scenario performs.
        check(currentWorker->timers.capacity() <= 1024, "timer heap capacity not reclaimed");
    }
    void onMessage(MessagePtr message) override {
        if (message->typeId != 1) return;
        start([this](CoroutineContext& c) -> std::any {
            for (MessageId i = 1; i <= 10000; ++i) {
                auto result = c.await(WaitSpec::any({
                    WaitSpec::message({77, i}),
                    WaitSpec::timer(std::chrono::steady_clock::now() + 10min),
                    WaitSpec::timer(std::chrono::steady_clock::now() - 1s)}));
                check(result.reason == WakeReason::Timeout && result.branchIndex == 2,
                      "wrong immediate winner");
                clean();
            }
            for (MessageId i = 1; i <= 1000; ++i) {
                bool rejected = false;
                try {
                    c.await(WaitSpec::all({
                        WaitSpec::message({88, i}),
                        WaitSpec::timer(std::chrono::steady_clock::now() + 10min),
                        WaitSpec::child(99999999)}));
                } catch (const std::invalid_argument&) { rejected = true; }
                check(rejected, "invalid child was not rejected");
                clean();
            }
            // A matching ordinary message resumes the fiber. The queued message
            // cannot execute its handler until this fiber yields Actor execution.
            c.self().send(99, {});
            auto reply = c.await(WaitSpec::any({WaitSpec::message({99, NoCorrelation}),
                WaitSpec::timer(std::chrono::steady_clock::now() + 10min)}));
            check(reply.reason == WakeReason::Message && reply.branchIndex == 0,
                  "message did not win suspended Any");
            clean();
            // Actual timeout, not an already-expired immediate branch.
            auto timeout = c.await(WaitSpec::any({WaitSpec::message({98, 123}),
                WaitSpec::timer(std::chrono::steady_clock::now() + 1ms)}));
            check(timeout.reason == WakeReason::Timeout && timeout.branchIndex == 1,
                  "timer did not win suspended Any");
            clean();
            done_->set_value();
            return {};
        });
    }
    void onCoroutineError(FiberId, std::exception_ptr error) override {
        done_->set_exception(error);
    }
    std::shared_ptr<std::promise<void>> done_;
};
int main() {
    try {
        for (const auto workers : {1U, 4U}) {
            ActorSystem::Config config; config.workerCount = workers;
            ActorSystem system(config);
            auto done = std::make_shared<std::promise<void>>();
            auto future = done->get_future();
            auto actor = system.registerActor(std::make_unique<CleanupActor>(system, done));
            actor.send(1, {});
            check(future.wait_for(10s) == std::future_status::ready, "cleanup scenario timed out");
            future.get();
            system.shutdown();
            check(system.stats().liveStacks == 0, "live stacks after shutdown");
            std::cout << "PASS wait cleanup workers=" << workers
                      << " immediate=10000 rollback=1000 message=1 timeout=1\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
