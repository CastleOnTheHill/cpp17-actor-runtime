#include <any>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
void require(bool ok, const char* why) { if (!ok) throw std::logic_error(why); }
struct Add { int amount; };
struct Message { int type; std::any payload; };
enum class Send { Accepted, Full, Closed };
class MailboxActor {
public:
    explicit MailboxActor(std::size_t capacity) : capacity_(capacity) {}
    Send post(Message message) {
        std::lock_guard<std::mutex> lock(mailboxMutex_);
        if (closed_) return Send::Closed;
        if (mailbox_.size() >= capacity_) return Send::Full;
        mailbox_.push_back(std::move(message));
        return Send::Accepted; // only acceptance, no handler has run here
    }
    void close() { std::lock_guard<std::mutex> lock(mailboxMutex_); closed_ = true; }
    bool dispatchOne() {
        std::lock_guard<std::mutex> token(executionMutex_);
        Message message;
        {
            std::lock_guard<std::mutex> lock(mailboxMutex_);
            if (mailbox_.empty()) return false;
            message = std::move(mailbox_.front());
            mailbox_.pop_front();
        } // producers need not wait for business handling while mailbox lock is free
        ++consumed;
        if (message.type != 1) { ++badType; return true; }
        const auto* add = std::any_cast<Add>(&message.payload);
        if (!add) { ++badPayload; return true; }
        value += add->amount;
        ++applied;
        return true;
    }
    // Read only before workers start or after they all join in this lab.
    int value = 0, consumed = 0, applied = 0, badType = 0, badPayload = 0;
private:
    std::mutex executionMutex_, mailboxMutex_;
    std::deque<Message> mailbox_;
    std::size_t capacity_;
    bool closed_ = false;
};
int main(int argc, char** argv) {
    try {
        const std::string mode = argc == 2 ? argv[1] : "1";
        require(argc <= 2 && (mode == "1" || mode == "4" || mode == "capacity"), "1 | 4 | capacity");
        MailboxActor actor(mode == "capacity" ? 1 : 4);
        Add original{2};
        require(actor.post({1, original}) == Send::Accepted, "first send");
        original.amount = 99; // message owns a copied Add value, not a reference to original
        require(actor.value == 0 && actor.consumed == 0, "post executed business inline");
        if (mode == "capacity") {
            require(actor.post({1, Add{3}}) == Send::Full, "capacity boundary");
            require(actor.dispatchOne() && actor.value == 2, "copied payload");
            require(actor.post({1, Add{3}}) == Send::Accepted, "capacity not released by pop");
        } else {
            require(actor.post({1, std::string("not Add")}) == Send::Accepted, "bad payload can be accepted");
            require(actor.post({9, Add{50}}) == Send::Accepted, "unknown type can be accepted");
            require(actor.post({1, Add{3}}) == Send::Accepted, "last send");
        }
        actor.close(); // lab policy: reject new messages, drain previously accepted ones
        require(actor.post({1, Add{9}}) == Send::Closed, "closed mailbox accepted new message");
        const int workers = mode == "4" ? 4 : 1;
        std::vector<std::thread> threads;
        // All producers have finished: empty now means these worker loops may exit.
        for (int w = 0; w < workers; ++w) threads.emplace_back([&] { while (actor.dispatchOne()) {} });
        for (auto& thread : threads) thread.join();
        require(actor.value == 5 && actor.applied == 2, "business result");
        require(actor.consumed == (mode == "capacity" ? 2 : 4), "consumption count");
        require(actor.badPayload == (mode == "capacity" ? 0 : 1) && actor.badType == (mode == "capacity" ? 0 : 1), "type validation");
        require(!actor.dispatchOne(), "empty dispatch");
        std::cout << "consumed=" << actor.consumed << " applied=" << actor.applied
                  << " badPayload=" << actor.badPayload << " badType=" << actor.badType << " value=" << actor.value << '\n';
        std::cout << "[PASS] ch08 " << mode << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
