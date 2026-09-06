#include "actor_runtime/c_api.h"
#include "internal.h"
#include <cstring>

using namespace actor_runtime;
struct ar_system { std::unique_ptr<ActorSystem> owned; ActorSystem* value = nullptr; };
struct ClientState {
    std::mutex mutex, calls;
    std::condition_variable ready;
    MessageId expected = 0;
    TypeId expectedType = 0;
    bool complete = false, closed = false;
    Bytes bytes;
};
struct ar_client { ActorSystem* system; ActorId id; std::shared_ptr<ClientState> state; };
namespace {
thread_local std::string lastError;
int guarded(const std::function<int()>& operation) noexcept {
    try { lastError.clear(); return operation(); }
    catch (const std::invalid_argument& e) { lastError = e.what(); return AR_INVALID; }
    catch (const std::length_error& e) { lastError = e.what(); return AR_INVALID; }
    catch (const std::exception& e) { lastError = e.what(); return AR_ERROR; }
    catch (...) { lastError = "unknown C++ exception"; return AR_ERROR; }
}
void valid(bool condition) { if (!condition) throw std::invalid_argument("invalid C API argument or actor-thread blocking call"); }
class HandlerActor final : public Actor {
public:
    HandlerActor(ar_system* s, ar_handler handler, void* user, ar_cleanup cleanup)
        : Actor(*s->value), handler_(handler), user_(user), cleanup_(cleanup) { view_.value = s->value; }
    ~HandlerActor() override { if (cleanup_) cleanup_(user_); }
private:
    void onMessage(MessagePtr message) override {
        const auto& bytes = std::any_cast<const Bytes&>(message->payload);
        ar_message view{message->typeId, message->id, message->correlationId, bytes.data(), bytes.size(), message.get()};
        handler_(user_, &view_, &view);
    }
    ar_system view_;
    ar_handler handler_;
    void* user_;
    ar_cleanup cleanup_;
};
class ClientActor final : public Actor {
public:
    ClientActor(ActorSystem& s, std::shared_ptr<ClientState> state) : Actor(s), state_(std::move(state)) {}
    ~ClientActor() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closed = true; state_->ready.notify_all();
    }
private:
    void onMessage(MessagePtr message) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (message->correlationId != state_->expected || message->typeId != state_->expectedType || state_->complete) return;
        state_->bytes = std::any_cast<const Bytes&>(message->payload);
        state_->complete = true; state_->ready.notify_all();
    }
    std::shared_ptr<ClientState> state_;
};
}

extern "C" {
const char* ar_last_error(void) { return lastError.c_str(); }
int ar_system_create(const char* name, size_t workers, ar_system** result) {
    return guarded([&] {
        valid(result); *result = nullptr;
        auto wrapper = std::make_unique<ar_system>();
        ActorSystem::Config config; config.workerCount = workers; config.processName = name ? name : "";
        wrapper->owned = std::make_unique<ActorSystem>(config); wrapper->value = wrapper->owned.get();
        *result = wrapper.release(); return AR_OK;
    });
}
int ar_system_borrow(void* cpp, ar_system** result) {
    return guarded([&] {
        valid(cpp && result); *result = nullptr;
        auto wrapper = std::make_unique<ar_system>(); wrapper->value = static_cast<ActorSystem*>(cpp);
        *result = wrapper.release(); return AR_OK;
    });
}
int ar_system_destroy(ar_system* system) {
    return guarded([&] { valid(system && !currentWorker); delete system; return AR_OK; });
}
int ar_actor_register(ar_system* system, const char* name, ar_handler handler, void* user, ar_cleanup cleanup, uint64_t* id) {
    return guarded([&] {
        valid(system && name && handler && id);
        *id = system->value->registerActor(name, std::make_unique<HandlerActor>(system, handler, user, cleanup)).id();
        return AR_OK;
    });
}
int ar_actor_stop(ar_system* system, uint64_t id, uint32_t timeout) {
    return guarded([&] {
        valid(system && !currentWorker);
        auto stopped = system->value->stopActor(id);
        return stopped.wait_for(std::chrono::milliseconds(timeout)) == std::future_status::ready ? AR_OK : AR_TIMEOUT;
    });
}
int ar_reply(ar_system* system, const ar_message* request, uint64_t type, const void* data, size_t size) {
    return guarded([&] {
        valid(system && request && request->internal_request && currentActor);
        system->value->replyBytes(*static_cast<const Message*>(request->internal_request), type, data, size);
        return AR_OK;
    });
}
int ar_client_create(ar_system* system, const char* name, ar_client** result) {
    return guarded([&] {
        valid(system && name && result); *result = nullptr;
        auto client = std::make_unique<ar_client>(); client->system = system->value;
        client->state = std::make_shared<ClientState>();
        client->id = client->system->registerActor(name, std::make_unique<ClientActor>(*client->system, client->state)).id();
        *result = client.release(); return AR_OK;
    });
}
int ar_client_destroy(ar_client* client) {
    return guarded([&] {
        valid(client && !currentWorker);
        auto state = client->state;
        bool closed;
        { std::lock_guard<std::mutex> lock(state->mutex); closed = state->closed; }
        if (closed) { delete client; return AR_OK; }
        auto stopped = client->system->stopActor(client->id);
        stopped.wait();
        delete client; return AR_OK;
    });
}
int ar_client_request(ar_client* client, const char* process, const char* actor,
                      uint64_t type, const void* data, size_t size, uint64_t replyType,
                      uint32_t timeout, void* out, size_t capacity, size_t* outSize) {
    return guarded([&] {
        valid(client && actor && outSize && (!capacity || out) && !currentWorker);
        *outSize = 0;
        auto state = client->state;
        std::lock_guard<std::mutex> callLock(state->calls);
        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->closed) return AR_CLOSED;
        state->complete = false; state->bytes.clear(); state->expectedType = replyType;
        state->expected = client->system->sendBytes({process ? process : "", actor}, type, data, size, 0, client->id);
        if (!state->ready.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return state->complete || state->closed; })) {
            state->expected = 0; return AR_TIMEOUT;
        }
        if (state->closed) return AR_CLOSED;
        *outSize = state->bytes.size();
        if (state->bytes.size() > capacity) return AR_BUFFER_SMALL;
        if (!state->bytes.empty()) std::memcpy(out, state->bytes.data(), state->bytes.size());
        return AR_OK;
    });
}
}
