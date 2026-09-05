// 第三章：同一个 OS 线程，两个上下文，一条独立有栈协程。
// 创建不等于执行；让出不等于返回；完成之后禁止再次恢复。
// Demo 不可移动，保证 uc_link/context 的地址稳定；栈在 scheduler 上析构。
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

struct Stack {
    void* mapping;
    std::size_t bytes;
    void* base;
    std::size_t usable = 128 * 1024;
    Stack() {
        const long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) throw std::runtime_error("invalid page size");
        auto p = static_cast<std::size_t>(page);
        usable = (usable + p - 1) / p * p;
        bytes = usable + 2 * p;
        mapping = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "mmap");
        base = static_cast<char*>(mapping) + p;
        if (mprotect(base, usable, PROT_READ | PROT_WRITE) != 0) {
            int error = errno;
            munmap(mapping, bytes);
            throw std::system_error(error, std::generic_category(), "mprotect");
        }
    }
    ~Stack() { munmap(mapping, bytes); }
    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;
};

class Demo {
public:
    explicit Demo(bool fail) : fail_(fail) {
        if (getcontext(&fiber_) != 0) throw std::system_error(errno, std::generic_category(), "getcontext");
        fiber_.uc_stack.ss_sp = stack_.base;
        fiber_.uc_stack.ss_size = stack_.usable;
        fiber_.uc_stack.ss_flags = 0;
        fiber_.uc_link = &scheduler_;
        makecontext(&fiber_, &Demo::entry, 0); // 零参数入口，不把 C++ 指针塞进可变参数。
    }
    Demo(const Demo&) = delete;
    Demo& operator=(const Demo&) = delete;
    bool resume() {
        if (done_) return false;
        active_ = this;
        if (swapcontext(&scheduler_, &fiber_) != 0) std::terminate(); // 教学实验：切换失败立即终止。
        active_ = nullptr;
        return true;
    }
    void yield() {
        if (swapcontext(&fiber_, &scheduler_) != 0) std::terminate();
        // 第二次 resume 之后，从这条 swapcontext 后面继续。
    }
    bool resourceAlive = false;
    bool sameAddress = false;
    std::exception_ptr error;
    std::vector<std::string> log;
private:
    struct Guard {
        Demo& demo;
        explicit Guard(Demo& d) : demo(d) { demo.resourceAlive = true; demo.log.push_back("resource open"); }
        ~Guard() { demo.resourceAlive = false; demo.log.push_back("resource closed"); }
    };
    void inner() {
        Guard guard(*this);
        int value = 41;
        int* address = &value;
        log.push_back("A before=41");
        yield(); // inner() 和业务入口都没有返回，guard 与 value 仍然存活。
        sameAddress = address == &value;
        ++value;
        log.push_back("A after=" + std::to_string(value));
        if (fail_) throw std::runtime_error("business failure");
    }
    void body() { inner(); }
    static void entry() {
        Demo& demo = *active_;
        try { demo.body(); } // 异常只在 fiber 栈内展开，保存后由 scheduler 读取。
        catch (...) { demo.error = std::current_exception(); }
        demo.done_ = true;
        // 入口返回后由 uc_link 回到 scheduler，不能在此 munmap 当前栈。
    }
    inline static thread_local Demo* active_ = nullptr;
    Stack stack_;
    ucontext_t scheduler_{};
    ucontext_t fiber_{};
    bool fail_;
    bool done_ = false;
};

int main(int argc, char** argv) {
    try {
        const bool fail = argc == 2 && std::string(argv[1]) == "--throw";
        if (argc > 2 || (argc == 2 && !fail)) throw std::invalid_argument("expected --throw or no args");
        Demo demo(fail);
        if (!demo.log.empty()) throw std::logic_error("construction executed body");
        if (!demo.resume() || !demo.resourceAlive) throw std::logic_error("suspension destroyed resource");
        demo.log.push_back("B ran on scheduler");
        if (!demo.resume() || demo.resourceAlive || !demo.sameAddress) throw std::logic_error("resume/lifetime mismatch");
        if (demo.resume()) throw std::logic_error("finished coroutine resumed again");
        const std::vector<std::string> expected{
            "resource open", "A before=41", "B ran on scheduler", "A after=42", "resource closed"};
        if (demo.log != expected) throw std::logic_error("wrong execution trace");
        for (const auto& line : demo.log) std::cout << line << '\n';
        if (fail) {
            if (!demo.error) throw std::logic_error("lost exception");
            try { std::rethrow_exception(demo.error); }
            catch (const std::runtime_error&) { std::cout << "scheduler caught business failure\n"; }
        } else if (demo.error) { std::rethrow_exception(demo.error); }
        std::cout << "[PASS] ch03\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
