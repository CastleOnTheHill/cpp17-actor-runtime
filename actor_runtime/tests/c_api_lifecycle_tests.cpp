#include "actor_runtime/c_api.h"
#include "actor_runtime/actor_system.h"
#include <atomic>
#include <cstring>
#include <iostream>

using namespace actor_runtime;
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
struct State { std::atomic<int> cleaned{0}; };
void cleanup(void* p) { ++static_cast<State*>(p)->cleaned; }
void echo(void*, ar_system* system, const ar_message* request) {
    check(ar_reply(system, request, 2, request->data, request->size) == AR_OK, "local C reply failed");
    check(ar_system_destroy(system) == AR_INVALID, "handler allowed to destroy its own runtime");
}
int main() {
    try {
        State state;
        ActorSystem system({1});
        ar_system* borrowed = nullptr;
        check(ar_system_borrow(&system, &borrowed) == AR_OK, "borrow runtime");
        std::uint64_t id = 0;
        check(ar_actor_register(borrowed, "local", echo, &state, cleanup, &id) == AR_OK, "C actor register");
        ar_client* client = nullptr;
        check(ar_client_create(borrowed, "module", &client) == AR_OK, "C client create");
        int input = 41, output = 0; std::size_t written = 0;
        check(ar_client_request(client, "", "local", 1, &input, sizeof(input), 2, 1000, &output, sizeof(output), &written) == AR_OK,
              "in-process C module request");
        check(output == input && written == sizeof(input), "C byte copy");
        check(ar_actor_stop(borrowed, id, 1000) == AR_OK && state.cleaned == 1, "C userdata cleanup before stop acknowledgement");
        check(ar_actor_stop(borrowed, id, 1000) == AR_INVALID, "stop removed actor error");
        check(ar_system_destroy(borrowed) == AR_OK, "borrow wrapper release");
        system.shutdown();
        check(ar_client_request(client, "", "local", 1, &input, sizeof(input), 2, 10, &output, sizeof(output), &written) == AR_CLOSED,
              "C client not notified of system shutdown");
        check(ar_client_destroy(client) == AR_OK, "C client cleanup after system shutdown");
        check(ar_system_create("bad/name", 1, &borrowed) == AR_INVALID && borrowed == nullptr, "bad process name accepted");
        check(ar_system_create(nullptr, 1, nullptr) == AR_INVALID, "null output accepted");
        check(ar_system_borrow(nullptr, &borrowed) == AR_INVALID, "null C++ system accepted");
        check(ar_client_destroy(nullptr) == AR_INVALID && std::strlen(ar_last_error()) > 0, "C error text absent");
        std::cout << "[PASS] C borrowed runtime, local request, cleanup, post-shutdown client, invalid inputs\n";
    } catch (const std::exception& e) { std::cerr << "[FAIL] " << e.what() << '\n'; return 1; }
}
