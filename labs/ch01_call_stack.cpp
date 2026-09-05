// 第一章：正常返回和异常展开都会离开局部对象的作用域。
// 不讨论编译器是否真的创建物理栈帧；这里观察 C++ 层面的调用和生命周期。
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

std::vector<std::string> events;
void trace(std::string event) { events.push_back(event); std::cout << event << '\n'; }
struct Guard {
    std::string name;
    explicit Guard(std::string value) : name(std::move(value)) { trace("construct " + name); }
    ~Guard() { trace("destroy " + name); }
};

int inner(bool fail) {
    Guard resource("inner");
    int value = 41;
    trace("inner value=" + std::to_string(value));
    if (fail) throw std::runtime_error("simulated failure");
    return value + 1; // 计算返回值，再析构本层 resource，随后调用者继续。
}
int outer(bool fail) {
    Guard resource("outer");
    int result = inner(fail);
    trace("outer received=" + std::to_string(result));
    return result;
}
int main(int argc, char** argv) {
    const bool fail = argc == 2 && std::string(argv[1]) == "--throw";
    if (argc > 2 || (argc == 2 && !fail)) return 2;
    trace("main before");
    try { trace("main received=" + std::to_string(outer(fail))); }
    catch (const std::runtime_error&) { trace("main caught"); }
    trace("main after");
    const std::vector<std::string> expected = fail
        ? std::vector<std::string>{"main before", "construct outer", "construct inner", "inner value=41",
            "destroy inner", "destroy outer", "main caught", "main after"}
        : std::vector<std::string>{"main before", "construct outer", "construct inner", "inner value=41",
            "destroy inner", "outer received=42", "destroy outer", "main received=42", "main after"};
    if (events != expected) { std::cerr << "[FAIL] lifetime/ordering mismatch\n"; return 1; }
    std::cout << "[PASS] ch01\n";
}
