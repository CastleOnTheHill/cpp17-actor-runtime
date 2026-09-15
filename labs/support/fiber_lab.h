#pragma once
// Chapters 4-6: single-thread, non-reentrant, cooperative teaching runtime.
// This is not actor_runtime. Time is a manually advanced integer, NOT wall time.
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace course {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::logic_error(message);
}
class Stack {
public:
    Stack() {
        const long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) throw std::runtime_error("invalid page size");
        const auto p = static_cast<std::size_t>(page);
        usableSize = (128 * 1024 + p - 1) / p * p;
        size_ = usableSize + 2 * p;
        mapping_ = mmap(nullptr, size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping_ == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "mmap");
        usable = static_cast<char*>(mapping_) + p;
        if (mprotect(usable, usableSize, PROT_READ | PROT_WRITE) != 0) {
            const int error = errno;
            munmap(mapping_, size_);
            throw std::system_error(error, std::generic_category(), "mprotect");
        }
    }
    ~Stack() { munmap(mapping_, size_); }
    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;
    void* usable = nullptr;
    std::size_t usableSize = 0;
private:
    void* mapping_ = nullptr;
    std::size_t size_ = 0;
};

enum class State { Ready, Running, Waiting, Done };
class Loop {
    struct Fiber {
        Stack stack;
        ucontext_t context{};
        std::function<void()> body;
        State state = State::Ready;
        bool started = false;
        int deadline = 0;
        std::exception_ptr error;
    };
public:
    struct Completion { std::size_t id; std::exception_ptr error; };
    explicit Loop(bool autoCollect = true) : autoCollect_(autoCollect) {}
    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;
    ~Loop() {
        // Never silently unmap a started but suspended C++ call chain.
        // Labs must drain all started fibers; general cancellation is out of scope.
        for (const auto& f : fibers_)
            if (f && f->started && f->state != State::Done) std::terminate();
    }
    std::size_t spawn(std::function<void()> body) {
        require(active_ == nullptr, "spawn only on scheduler in these labs");
        require(static_cast<bool>(body), "empty body");
        auto fiber = std::make_unique<Fiber>();
        fiber->body = std::move(body);
        if (getcontext(&fiber->context) != 0)
            throw std::system_error(errno, std::generic_category(), "getcontext");
        fiber->context.uc_stack.ss_sp = fiber->stack.usable;
        fiber->context.uc_stack.ss_size = fiber->stack.usableSize;
        fiber->context.uc_stack.ss_flags = 0;
        fiber->context.uc_link = &scheduler_;
        makecontext(&fiber->context, &Loop::entry, 0);
        const auto id = fibers_.size();
        fibers_.push_back(std::move(fiber)); // pointee address stays stable
        ready_.push_back(id);
        return id;
    }
    // Chapter 4 core: FIFO pop -> resume one slice -> inspect returned state.
    bool step() {
        require(active_ == nullptr, "nested/cross-loop run is not supported");
        if (ready_.empty()) return false;
        const auto id = ready_.front();
        ready_.pop_front();
        Fiber& f = *fibers_.at(id);
        require(f.state == State::Ready, "duplicate or invalid ready entry");
        f.state = State::Running;
        f.started = true;
        running_ = &f;
        active_ = this;
        if (swapcontext(&scheduler_, &f.context) != 0) std::terminate();
        active_ = nullptr;
        running_ = nullptr; // now on scheduler; only here may reclaim stack
        if (f.state == State::Ready) ready_.push_back(id);
        else if (f.state == State::Done) {
            completions.push_back({id, f.error});
            finished_.push_back(id);
        } else require(f.state == State::Waiting, "fiber returned without state change");
        if (autoCollect_) collect();
        return true;
    }
    void yield() { suspend(State::Ready); }
    // Chapter 5: waiting fibers never go straight back into ready_.
    void sleepFor(int ticks) {
        require(active_ == this && running_, "sleep outside fiber");
        require(ticks >= 0 && ticks <= std::numeric_limits<int>::max() - now_, "invalid duration");
        if (ticks == 0) { yield(); return; }
        running_->deadline = now_ + ticks;
        suspend(State::Waiting);
    }
    void advanceTo(int target) {
        require(active_ == nullptr, "advance only on scheduler");
        require(target >= now_, "clock cannot go backwards");
        now_ = target;
        // Deterministic tie order = spawn order, not a universal timer policy.
        for (std::size_t id = 0; id < fibers_.size(); ++id) {
            auto& f = fibers_[id];
            if (f && f->state == State::Waiting && f->deadline <= now_) {
                f->state = State::Ready;
                ready_.push_back(id);
            }
        }
    }
    void run() {
        require(active_ == nullptr, "run only on scheduler");
        for (;;) {
            if (step()) continue;
            int earliest = std::numeric_limits<int>::max();
            bool waiting = false;
            for (const auto& f : fibers_) if (f && f->state == State::Waiting) {
                waiting = true;
                earliest = std::min(earliest, f->deadline);
            }
            if (!waiting) break;
            advanceTo(earliest); // fake clock jumps only when ready queue is empty
        }
    }
    // Chapter 6: small completion records survive stack reclamation.
    std::size_t collect() {
        require(active_ == nullptr, "cannot reclaim from a running fiber");
        const auto count = finished_.size();
        while (!finished_.empty()) {
            const auto id = finished_.front();
            finished_.pop_front();
            require(fibers_.at(id)->state == State::Done, "reclaiming unfinished fiber");
            fibers_[id].reset();
        }
        return count;
    }
    std::size_t stacks() const {
        return static_cast<std::size_t>(std::count_if(fibers_.begin(), fibers_.end(),
            [](const auto& f) { return bool(f); }));
    }
    std::size_t readyCount() const { return ready_.size(); }
    int now() const { return now_; }
    bool onFiber() const { return active_ == this; }
    void checkSuccess() const {
        for (const auto& c : completions) if (c.error) std::rethrow_exception(c.error);
    }
    std::vector<Completion> completions;
private:
    void suspend(State state) {
        require(active_ == this && running_, "suspend outside fiber");
        running_->state = state;
        if (swapcontext(&running_->context, &scheduler_) != 0) std::terminate();
    }
    static void entry() {
        Loop& loop = *active_;
        Fiber& fiber = *loop.running_;
        try { fiber.body(); }
        catch (...) { fiber.error = std::current_exception(); }
        fiber.state = State::Done;
        // Return through uc_link. Do not destroy fiber/stack on this stack.
    }
    inline static thread_local Loop* active_ = nullptr;
    ucontext_t scheduler_{};
    std::vector<std::unique_ptr<Fiber>> fibers_;
    std::deque<std::size_t> ready_, finished_;
    Fiber* running_ = nullptr;
    bool autoCollect_;
    int now_ = 0;
};
} // namespace course
