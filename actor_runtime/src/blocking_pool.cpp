#include "blocking_pool.h"
#include <stdexcept>
namespace actor_runtime {
BlockingPool::BlockingPool(std::size_t threads, std::size_t capacity) : capacity_(capacity) {
    if (!threads || !capacity) throw std::invalid_argument("blocking pool sizes must be nonzero");
    try {
        for (std::size_t i = 0; i < threads; ++i) threads_.emplace_back([this] { loop(); });
    } catch (...) { stop(); throw; }
}
BlockingPool::~BlockingPool() { stop(); }
void BlockingPool::submit(std::function<void()> work) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) throw std::runtime_error("blocking pool is stopping");
    if (queue_.size() >= capacity_) throw std::runtime_error("blocking pool queue is full");
    queue_.push_back(std::move(work));
    ready_.notify_one();
}
void BlockingPool::loop() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;
            work = std::move(queue_.front()); queue_.pop_front();
        }
        work();
    }
}
void BlockingPool::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    ready_.notify_all();
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
}
}
