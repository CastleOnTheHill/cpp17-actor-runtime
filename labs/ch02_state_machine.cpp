// 第二章：普通 C++17 状态机模拟可暂停任务，不使用 ucontext，也不是 C++20 协程。
// next() 每次都正常返回；恢复所需的阶段和值放在长期存在的对象里。
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

enum class Step { Suspended, Done };
class Task {
public:
    explicit Task(int initial) : value_(initial) {}
    Step next(std::vector<std::string>& log) {
        if (phase_ == Phase::Start) {
            log.push_back("A before=" + std::to_string(value_));
            phase_ = Phase::AfterPause;
            return Step::Suspended;
        }
        if (phase_ == Phase::AfterPause) {
            ++value_;
            log.push_back("A after=" + std::to_string(value_));
            phase_ = Phase::Finished;
        }
        return Step::Done; // 重复 next 不会重复执行已结束的业务。
    }
private:
    enum class Phase { Start, AfterPause, Finished };
    Phase phase_ = Phase::Start;
    int value_; // 持久业务状态；不是 next() 的自动局部变量。
};
int main(int argc, char** argv) {
    try {
        if (argc > 2) throw std::invalid_argument("expected optional integer");
        int initial = argc == 2 ? std::stoi(argv[1]) : 41;
        if (initial < -1000 || initial > 1000) throw std::out_of_range("use -1000..1000");
        Task task(initial);
        std::vector<std::string> log;
        // 这三步就是一个固定顺序的教学调度器：A 一步 → B → A 下一步。
        if (task.next(log) != Step::Suspended) throw std::logic_error("first step must pause");
        log.push_back("B ran");
        if (task.next(log) != Step::Done) throw std::logic_error("second step must finish");
        if (task.next(log) != Step::Done) throw std::logic_error("finished task changed state");
        const std::vector<std::string> expected{
            "A before=" + std::to_string(initial), "B ran", "A after=" + std::to_string(initial + 1)};
        if (log != expected) throw std::logic_error("wrong sequence or duplicate execution");
        for (const auto& line : log) std::cout << line << '\n';
        std::cout << "[PASS] ch02\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
