#include "support/fiber_lab.h"
#include <iostream>
#include <string>
int main(int argc, char** argv) {
    try {
        const std::string mode = argc == 2 ? argv[1] : "round-robin";
        course::require(argc <= 2, "one mode argument allowed");
        course::Loop loop;
        std::vector<std::string> log;
        if (mode == "round-robin") {
            for (char name : {'A', 'B', 'C'}) loop.spawn([&, name] {
                log.push_back(std::string(1, name) + "1");
                loop.yield();
                log.push_back(std::string(1, name) + "2");
            });
            loop.run();
            course::require(log == std::vector<std::string>{"A1", "B1", "C1", "A2", "B2", "C2"}, "FIFO trace");
        } else if (mode == "no-yield") {
            loop.spawn([&] { for (int n = 1; n <= 3; ++n) log.push_back("A" + std::to_string(n)); });
            loop.spawn([&] { log.push_back("B1"); });
            loop.run();
            course::require(log == std::vector<std::string>{"A1", "A2", "A3", "B1"}, "cooperative trace");
        } else if (mode == "empty") {
            course::require(!loop.step(), "empty queue should not resume");
            loop.run();
        } else throw std::invalid_argument("round-robin | no-yield | empty");
        loop.checkSuccess();
        course::require(loop.stacks() == 0 && !loop.step(), "completed tasks must not requeue");
        for (const auto& line : log) std::cout << line << '\n';
        std::cout << "[PASS] ch04 " << mode << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
