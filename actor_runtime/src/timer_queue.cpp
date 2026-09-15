#include "internal.h"
#include <cassert>

namespace actor_runtime {
void TimerQueue::exchange(std::size_t a, std::size_t b) noexcept {
    std::swap(entries_[a], entries_[b]);
    entries_[a].leaf->timerIndex = a;
    entries_[b].leaf->timerIndex = b;
}
void TimerQueue::up(std::size_t index) noexcept {
    while (index) {
        const auto parent = (index - 1) / 2;
        if (!TimerLater{}(entries_[parent], entries_[index])) break;
        exchange(parent, index);
        index = parent;
    }
}
void TimerQueue::down(std::size_t index) noexcept {
    for (;;) {
        auto child = index * 2 + 1;
        if (child >= entries_.size()) return;
        if (child + 1 < entries_.size() && TimerLater{}(entries_[child], entries_[child + 1])) ++child;
        if (!TimerLater{}(entries_[index], entries_[child])) return;
        exchange(index, child);
        index = child;
    }
}
void TimerQueue::push(TimerEntry entry) {
    assert(entry.leaf && entry.leaf->timerIndex == static_cast<std::size_t>(-1));
    entries_.push_back(std::move(entry));
    entries_.back().leaf->timerIndex = entries_.size() - 1;
    up(entries_.size() - 1);
}
void TimerQueue::pop() noexcept { erase(0); }
void TimerQueue::cancel(RuntimeWaitNode& leaf) noexcept {
    if (leaf.timerIndex == static_cast<std::size_t>(-1)) return;
    assert(leaf.timerIndex < entries_.size() && entries_[leaf.timerIndex].leaf == &leaf);
    erase(leaf.timerIndex);
}
void TimerQueue::erase(std::size_t index) noexcept {
    entries_[index].leaf->timerIndex = static_cast<std::size_t>(-1);
    if (index != entries_.size() - 1) {
        entries_[index] = std::move(entries_.back());
        entries_[index].leaf->timerIndex = index;
    }
    entries_.pop_back();
    if (index < entries_.size()) {
        if (index && TimerLater{}(entries_[(index - 1) / 2], entries_[index])) up(index);
        else down(index);
    }
    // Retain a small working set; reclaim burst high-water capacity geometrically.
    // Reclamation is best-effort under allocation failure, cancellation is not.
    if (entries_.capacity() > 256 && entries_.size() < entries_.capacity() / 4) {
        try { std::vector<TimerEntry>(entries_).swap(entries_); }
        catch (const std::bad_alloc&) {}
    }
}
} // namespace actor_runtime
