#include "actor_runtime/actor_system.h"
#include "actor_runtime/actor.h"
#include <future>
#include <iostream>
#include <unistd.h>
using namespace actor_runtime;
using namespace std::chrono_literals;
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
class Capture final : public Actor {
public:
    Capture(ActorSystem& s, std::promise<MessagePtr>& received) : Actor(s), received_(received) {}
private:
    void onMessage(MessagePtr message) override { received_.set_value(message); }
    std::promise<MessagePtr>& received_;
};
int main() {
    try {
        const auto prefix = "ipc_life_" + std::to_string(::getpid());
        ActorSystem::Config config; config.workerCount = 2; config.diagnosticLogging = true;
        config.processName = prefix + "a"; ActorSystem a(config);
        bool collision = false;
        try { ActorSystem duplicate(config); } catch (const std::system_error&) { collision = true; }
        check(collision, "duplicate process bind accepted");
        config.processName = prefix + "b"; ActorSystem b(config);
        std::promise<MessagePtr> oldInbox, newInbox, serverInbox;
        auto request = serverInbox.get_future(); auto newMessage = newInbox.get_future();
        auto old = a.registerActor("same", std::make_unique<Capture>(a, oldInbox));
        b.registerActor("same", std::make_unique<Capture>(b, serverInbox));
        int input = 17;
        a.sendBytes({config.processName, "same"}, 1, &input, sizeof(input), 0, old.id());
        check(request.wait_for(2s) == std::future_status::ready, "remote request missing");
        auto original = request.get();
        auto stopped = a.stopActor(old.id());
        check(stopped.wait_for(2s) == std::future_status::ready, "stop old incarnation");
        auto replacement = a.registerActor("same", std::make_unique<Capture>(a, newInbox));
        check(replacement.id() != old.id(), "instance id reused");
        b.replyBytes(*original, 2, &input, sizeof(input)); // 应答携带旧 ActorId，不能落入新同名 Actor。
        b.sendBytes({prefix + "a", "same"}, 3, &input, sizeof(input));
        check(newMessage.wait_for(2s) == std::future_status::ready, "new actor not reachable");
        check(newMessage.get()->typeId == 3, "late response hit replacement actor");
        bool badName = false;
        try { a.registerActor("../bad", std::make_unique<Capture>(a, oldInbox)); }
        catch (const std::invalid_argument&) { badName = true; }
        check(badName, "unsafe name accepted");
        bool tooLarge = false;
        try { a.sendBytes({prefix + "b", "same"}, 1, &input, 65536); }
        catch (const std::length_error&) { tooLarge = true; }
        check(tooLarge, "oversized packet accepted");
        a.shutdown(); b.shutdown();
        { ActorSystem rebound(config); } // 退出后 abstract socket 自动释放，无残留 socket 文件。
        std::cout << "[PASS] process collision, stale reply isolation, dynamic rebinding, datagram bounds\n";
    } catch (const std::exception& e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
