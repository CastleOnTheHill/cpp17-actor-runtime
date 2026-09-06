#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
namespace actor_runtime {
class BlockingPool final {
public:
    BlockingPool(std::size_t threads, std::size_t capacity);
    ~BlockingPool();
    void submit(std::function<void()> work);
    void stop();
private:
    void loop();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread> threads_;
    std::size_t capacity_;
    bool stopping_ = false;
};
}
