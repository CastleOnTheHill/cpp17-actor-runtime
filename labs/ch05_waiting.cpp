#include "support/fiber_lab.h"
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    try {
        const std::string mode = argc == 2 ? argv[1] : "deadline";
        course::require(argc <= 2, "one mode argument allowed");
        course::Loop loop;
        std::vector<std::string> log;
        if (mode == "deadline") {
            loop.spawn([&] { log.push_back("A waits@0"); loop.sleepFor(5); log.push_back("A resumes@" + std::to_string(loop.now())); });
            loop.spawn([&] { log.push_back("B runs@" + std::to_string(loop.now())); });
            course::require(loop.step() && loop.step(), "A and B should execute");
            course::require(!loop.step() && loop.completions.size() == 1 && loop.stacks() == 1, "empty ready != complete");
            loop.advanceTo(4);
            course::require(!loop.step(), "early wakeup");
            loop.advanceTo(5);
            loop.advanceTo(5); // repeated timer polling must not double-enqueue
            course::require(loop.readyCount() == 1, "duplicate wakeup");
            loop.run();
            course::require(log == std::vector<std::string>{"A waits@0", "B runs@0", "A resumes@5"}, "deadline trace");
        } else if (mode == "same-deadline") {
            for (char name : {'A', 'B'}) loop.spawn([&, name] { loop.sleepFor(3); log.emplace_back(1, name); });
            loop.run();
            course::require(log == std::vector<std::string>{"A", "B"} && loop.now() == 3, "tie policy");
        } else if (mode == "zero") {
            loop.spawn([&] { log.push_back("A1"); loop.sleepFor(0); log.push_back("A2"); });
            loop.spawn([&] { log.push_back("B"); });
            loop.run();
            course::require(log == std::vector<std::string>{"A1", "B", "A2"} && loop.now() == 0, "zero is yield in this lab");
        } else throw std::invalid_argument("deadline | same-deadline | zero");
        loop.checkSuccess();
        course::require(loop.stacks() == 0, "unfinished stack");
        for (const auto& line : log) std::cout << line << '\n';
        std::cout << "[PASS] ch05 " << mode << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
