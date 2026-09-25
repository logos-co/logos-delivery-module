#pragma once
// The one place this module talks to liblogosdelivery's poll ABI.
//
// nim-ffi's poll model hands the host every message -- method replies, events,
// and the library's own questions (REVERSE_CALL) -- through one queue per
// context, and a file descriptor that is readable while the queue is not
// empty. This pump owns that queue for the delivery context:
//
//   - A method call submits the CBOR request and then pumps the queue on the
//     calling thread until the call's reply arrives. Everything else that comes
//     out meanwhile (events, other replies, reverse calls) is dispatched as it
//     is seen, so a library that must ask the host something before it can
//     answer is served from inside the wait. That is the same blocking the old
//     semaphore did, with no thread and no deadlock.
//   - Between calls a QSocketNotifier on the poll fd drains the queue from the
//     Qt event loop, so events and reverse calls do not wait for the next call.
//
// Nothing here owns a thread. `reverseReply` may be called from any thread
// (nim-ffi guarantees that side); everything else runs on the thread that
// dispatches module methods, which is where the notifier lives too.
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <nlohmann/json.hpp>

extern "C" {
#include <liblogosdelivery_poll.h>
}

class PollPump {
public:
    // A method's outcome. `ret` is nim-ffi's code; on RET_OK `value` is the
    // decoded reply, otherwise `error` is the library's text.
    struct Reply {
        int ret = RET_ERR;
        nlohmann::json value;
        std::string error;
    };
    using Method = int (*)(void*, const uint8_t*, size_t, uint64_t*);
    // An event: the library's wire name id and its JSON payload.
    using EventHandler = std::function<void(uint64_t nameId, const nlohmann::json& payload)>;
    // A question from the library: answer it, from any thread, with reverseReply.
    using ReverseHandler = std::function<void(uint64_t callId, uint64_t nameId, const nlohmann::json& args)>;

    PollPump();
    ~PollPump();
    PollPump(const PollPump&) = delete;
    PollPump& operator=(const PollPump&) = delete;

    void setEventHandler(EventHandler h) { m_onEvent = std::move(h); }
    void setReverseHandler(ReverseHandler h) { m_onReverse = std::move(h); }

    // Creates the node and pumps until the constructor's reply. `rlnPlugin`
    // tells the library this module answers its RLN questions. Empty string
    // on success, otherwise the error text.
    std::string create(const std::string& configJson, bool rlnPlugin, std::chrono::milliseconds timeout);
    void destroy();
    bool alive() const { return m_ctx != nullptr; }
    void* ctx() const { return m_ctx; }

    // Submits `method` and pumps this thread until its reply, or `timeout`.
    Reply call(const char* name, Method method, const nlohmann::json& request,
               std::chrono::milliseconds timeout);
    // Submits without waiting and hands the reply to `done` when the pump
    // sees it; for calls whose outcome this module reports as an event
    // (start_node, stop_node). Returns nim-ffi's code; `done` never runs
    // unless it is RET_OK.
    using Done = std::function<void(const Reply&)>;
    int submit(Method method, const nlohmann::json& request, Done done);

    // Answers a REVERSE_CALL. Thread-safe. `payload` is the reply value.
    void reverseReply(uint64_t callId, int ret, const nlohmann::json& payload);

    // Dispatches every message queued right now. The notifier's slot.
    void drain();

private:
    Reply waitFor(uint64_t id, std::chrono::milliseconds timeout);
    void dispatch(const NimFfiMsg* m);
    void watch();

    void* m_ctx = nullptr;
    std::shared_ptr<void> m_notifier;  // the QSocketNotifier, when there is a Qt loop to live in
    std::recursive_mutex m_lock;     // the queue is pumped from one thread at a time
    std::map<uint64_t, Reply> m_settled;  // replies seen before their waiter asked
    std::map<uint64_t, Done> m_done;      // replies nobody waits for, handed on arrival
    EventHandler m_onEvent;
    ReverseHandler m_onReverse;
};
