// No ucontext here: isolate Actor serialization from fiber thread ownership.
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
void require(bool ok, const char* why) { if (!ok) throw std::logic_error(why); }
class CounterActor {
public:
    void handle(bool fail) {
        std::lock_guard<std::mutex> token(execution_); // entire handler, not just dequeue
        struct Active {
            std::atomic<int>& value;
            explicit Active(std::atomic<int>& v) : value(v) { require(value.fetch_add(1) == 0, "overlapping actor handlers"); }
            ~Active() { --value; }
        } guard(active_);
        if (fail) throw std::runtime_error("business failure");
        const int before = value_;
        std::this_thread::yield(); // OS hint only; the Actor token is STILL held
        value_ = before + 1;
    }
    int valueAfterJoin() const { return value_; } // only after all worker joins
private:
    std::mutex execution_;
    std::atomic<int> active_{0};
    int value_ = 0; // non-atomic; all worker access guarded by execution_
};
int main(int argc, char** argv) {
    try {
        const std::string mode = argc == 2 ? argv[1] : "4";
        require(argc <= 2, "one mode argument allowed");
        if (mode == "queue-only-model") {
            // Deterministic sequential model of two interleaved handlers; NO C++ data race.
            int state = 0;
            const int readA = state, readB = state;
            state = readA + 1;
            state = readB + 1;
            require(state == 1, "lost-update model changed");
            std::cout << "two dequeued jobs, two reads of 0, final=1 (desired=2)\n";
        } else {
            require(mode == "1" || mode == "4" || mode == "throw", "1 | 4 | throw | queue-only-model");
            const int workers = mode == "1" ? 1 : 4, jobs = 2000;
            CounterActor actor;
            std::atomic<int> next{0}, errors{0};
            std::vector<std::thread> threads;
            for (int w = 0; w < workers; ++w) threads.emplace_back([&] {
                for (;;) {
                    const int id = next.fetch_add(1); // allocate distinct jobs, not an Actor token
                    if (id >= jobs) break;
                    try { actor.handle(mode == "throw" && id == 0); }
                    catch (const std::runtime_error&) { ++errors; }
                }
            });
            for (auto& thread : threads) thread.join();
            const int failures = mode == "throw" ? 1 : 0;
            require(errors == failures && actor.valueAfterJoin() == jobs - failures, "serialized result or exception cleanup");
            std::cout << "workers=" << workers << " applied=" << actor.valueAfterJoin() << " errors=" << errors << '\n';
        }
        std::cout << "[PASS] ch07 " << mode << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
