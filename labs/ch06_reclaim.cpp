#include "support/fiber_lab.h"
#include <iostream>
#include <string>
struct Probe {
    int& live;
    explicit Probe(int& count) : live(count) { ++live; }
    ~Probe() { --live; }
};
int main(int argc, char** argv) {
    try {
        const std::string mode = argc == 2 ? argv[1] : "normal";
        course::require(argc <= 2 && (mode == "normal" || mode == "throw" || mode == "many"), "normal | throw | many");
        course::Loop loop(false); // collect manually so lifetime boundaries are observable
        int live = 0;
        const int count = mode == "many" ? 64 : 1;
        bool reclaimRejected = false;
        for (int i = 0; i < count; ++i) loop.spawn([&] {
            Probe probe(live);
            try { loop.collect(); }
            catch (const std::logic_error&) { reclaimRejected = true; }
            loop.yield();
            if (mode == "throw") throw std::runtime_error("business failure");
        });
        for (int i = 0; i < count; ++i) course::require(loop.step(), "first slice missing");
        course::require(live == count && loop.collect() == 0, "suspension must preserve objects/stacks");
        std::cout << "paused: objects=" << live << " stacks=" << loop.stacks() << '\n';
        loop.run();
        course::require(live == 0 && loop.stacks() == static_cast<std::size_t>(count), "object end != stack unmap");
        course::require(reclaimRejected && !loop.onFiber(), "reclaim must be on scheduler");
        std::cout << "done: objects=" << live << " stacks=" << loop.stacks() << '\n';
        course::require(loop.collect() == static_cast<std::size_t>(count) && loop.collect() == 0, "collect exactly once");
        course::require(loop.stacks() == 0 && !loop.step(), "dead context must not resume");
        course::require(loop.completions.size() == static_cast<std::size_t>(count), "completion record lost");
        if (mode == "throw") {
            course::require(bool(loop.completions[0].error), "exception lost");
            try { std::rethrow_exception(loop.completions[0].error); }
            catch (const std::runtime_error& e) { course::require(std::string(e.what()) == "business failure", "wrong error"); }
        } else loop.checkSuccess();
        std::cout << "collected: objects=" << live << " stacks=" << loop.stacks()
                  << " records=" << loop.completions.size() << '\n';
        std::cout << "[PASS] ch06 " << mode << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
