// 学习者编辑区：先自己实现，再对照 ../ch02_state_machine.cpp。
// 此练习默认不参与构建/CTest；未完成时故意失败。
#include <iostream>
#include <stdexcept>
#include <vector>
enum class Step { Suspended, Done };
class Task {
public:
    explicit Task(int initial) : value_(initial) {}
    Step next(std::vector<int>& observed) {
        (void)observed;
        // TODO: 第 1 次记录原值并暂停；第 2 次加 1、记录并完成。
        // TODO: 第 3 次以及以后返回 Done，且不能再修改 observed。
        // 提示：区分“本次的返回值”与“下次要做什么的持久状态”。
        throw std::logic_error("TODO: implement Task::next");
    }
private:
    enum class Phase { Start, AfterPause, Finished };
    Phase phase_ = Phase::Start;
    int value_;
};
void check(bool ok) { if (!ok) throw std::logic_error("exercise assertion failed"); }
int main() {
    try {
        for (int initial : {41, 10, -3}) {
            Task task(initial);
            std::vector<int> observed;
            check(task.next(observed) == Step::Suspended);
            check(observed == std::vector<int>{initial});
            check(task.next(observed) == Step::Done);
            check(observed == std::vector<int>({initial, initial + 1}));
            for (int repeat = 0; repeat < 3; ++repeat) check(task.next(observed) == Step::Done);
            check(observed == std::vector<int>({initial, initial + 1}));
        }
        std::cout << "[PASS] builder exercise\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
