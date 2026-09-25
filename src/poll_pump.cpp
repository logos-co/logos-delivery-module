#include "poll_pump.h"

#include <cstdio>
#include <poll.h>
#include <vector>

#if __has_include(<QSocketNotifier>)
#include <QSocketNotifier>
#include <QObject>
#define POLL_PUMP_HAS_QT 1
#endif

using nlohmann::json;

namespace {

// Time slice of one blocking poll while waiting for a reply: long enough to
// stay idle cheaply, short enough that the deadline is honoured.
constexpr int32_t kSliceMs = 50;

std::vector<uint8_t> encode(const json& request)
{
    return json::to_cbor(request.is_null() ? json::object() : request);
}

// A reply's payload: CBOR on RET_OK, UTF-8 text otherwise.
PollPump::Reply decodeReply(const NimFfiMsg* m)
{
    PollPump::Reply r;
    r.ret = m->ret_code;
    if (m->ret_code == RET_OK) {
        if (m->len > 0) {
            r.value = json::from_cbor(std::vector<uint8_t>(m->payload, m->payload + m->len),
                                      /*strict=*/true, /*allow_exceptions=*/false);
        }
    } else {
        r.error.assign(reinterpret_cast<const char*>(m->payload), m->len);
    }
    return r;
}

// An event's payload is whatever the library enqueued: liblogosdelivery emits
// its events as JSON text, so that is tried first; a CBOR value second.
json decodeEvent(const NimFfiMsg* m)
{
    if (m->len == 0) return json();
    const auto* text = reinterpret_cast<const char*>(m->payload);
    json j = json::parse(text, text + m->len, nullptr, /*allow_exceptions=*/false);
    if (!j.is_discarded()) return j;
    return json::from_cbor(std::vector<uint8_t>(m->payload, m->payload + m->len), true, false);
}

json decodeArgs(const NimFfiMsg* m)
{
    if (m->len == 0) return json::object();
    return json::from_cbor(std::vector<uint8_t>(m->payload, m->payload + m->len), true, false);
}

} // namespace

PollPump::PollPump() = default;

PollPump::~PollPump()
{
    destroy();
}

std::string PollPump::create(const std::string& configJson, bool rlnPlugin, std::chrono::milliseconds timeout)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    if (m_ctx) return "context already initialized";
    const auto req = encode(json{{"configJson", configJson}, {"rlnPlugin", rlnPlugin}});
    void* ctx = nullptr;
    uint64_t id = 0;
    const int rc = logosdelivery_create_node(req.data(), req.size(), &ctx, &id);
    if (rc != RET_OK || !ctx) {
        return "logosdelivery_create_node rc=" + std::to_string(rc);
    }
    m_ctx = ctx;
    Reply ready = waitFor(id, timeout);
    if (ready.ret != RET_OK) {
        std::string why = ready.error.empty() ? "rc=" + std::to_string(ready.ret) : ready.error;
        logosdelivery_destroy(m_ctx);
        m_ctx = nullptr;
        return "node creation failed: " + why;
    }
    watch();
    return {};
}

void PollPump::destroy()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    m_notifier.reset();
    if (m_ctx) {
        logosdelivery_destroy(m_ctx);
        m_ctx = nullptr;
    }
    m_settled.clear();
    m_done.clear();
}

void PollPump::watch()
{
#ifdef POLL_PUMP_HAS_QT
    const int fd = logosdelivery_poll_fd(m_ctx);
    if (fd < 0) return;
    auto notifier = std::make_shared<QSocketNotifier>(fd, QSocketNotifier::Read);
    QObject::connect(notifier.get(), &QSocketNotifier::activated, [this](int) { drain(); });
    m_notifier = notifier;
#endif
}

int PollPump::submit(Method method, const json& request, Done done)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    if (!m_ctx) return RET_INVALID_CTX;
    const auto req = encode(request);
    uint64_t id = 0;
    const int rc = method(m_ctx, req.data(), req.size(), &id);
    if (rc == RET_OK && done) {
        m_done[id] = std::move(done);
    }
    drain(); // whatever is already queued, including a reply that came at once
    return rc;
}

PollPump::Reply PollPump::call(const char* name, Method method, const json& request,
                              std::chrono::milliseconds timeout)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    Reply r;
    if (!m_ctx) {
        r.error = std::string(name) + ": context not initialized";
        return r;
    }
    const auto req = encode(request);
    uint64_t id = 0;
    const int rc = method(m_ctx, req.data(), req.size(), &id);
    if (rc != RET_OK) {
        // no reply will come for this call; nim-ffi says why
        r.ret = rc;
        r.error = std::string(name) + ": not accepted, rc=" + std::to_string(rc);
        return r;
    }
    r = waitFor(id, timeout);
    if (r.ret == RET_TIMEOUT && r.error.empty()) {
        r.error = std::string(name) + ": no reply within " + std::to_string(timeout.count()) + " ms";
    }
    return r;
}

PollPump::Reply PollPump::waitFor(uint64_t id, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (auto it = m_settled.find(id); it != m_settled.end()) {
            Reply r = std::move(it->second);
            m_settled.erase(it);
            return r;
        }
        if (!m_ctx) {
            Reply r;
            r.ret = RET_CLOSED;
            r.error = "context closed while waiting";
            return r;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            Reply r;
            r.ret = RET_TIMEOUT;
            return r;
        }
        const NimFfiMsg* m = nullptr;
        const int rc = logosdelivery_poll(m_ctx, static_cast<int32_t>(std::min<int64_t>(left.count(), kSliceMs)), &m);
        if (rc == RET_OK && m) {
            dispatch(m);
        } else if (rc != RET_TIMEOUT && rc != RET_OK) {
            Reply r;
            r.ret = rc;
            r.error = "logosdelivery_poll rc=" + std::to_string(rc);
            return r;
        }
    }
}

void PollPump::drain()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    if (!m_ctx) return;
    const NimFfiMsg* m = nullptr;
    while (logosdelivery_poll(m_ctx, 0, &m) == RET_OK && m) {
        dispatch(m);
        m = nullptr;
    }
}

void PollPump::dispatch(const NimFfiMsg* m)
{
    switch (m->kind) {
    case NIMFFI_MSG_REPLY:
        if (auto it = m_done.find(m->id); it != m_done.end()) {
            Done done = std::move(it->second);
            m_done.erase(it);
            done(decodeReply(m));
        } else {
            m_settled[m->id] = decodeReply(m);
        }
        break;
    case NIMFFI_MSG_EVENT:
        if (m_onEvent) m_onEvent(m->name_id, decodeEvent(m));
        break;
    case NIMFFI_MSG_REVERSE_CALL:
        if (m_onReverse) {
            m_onReverse(m->id, m->name_id, decodeArgs(m));
        } else {
            reverseReply(m->id, RET_ERR, "no handler for reverse calls");
        }
        break;
    case NIMFFI_MSG_STALE_WARN:
    case NIMFFI_MSG_NOT_RESPONDING:
    case NIMFFI_MSG_RESPONDING:
        break; // progress ticks on long calls; the deadline decides
    case NIMFFI_MSG_CLOSED:
        fprintf(stderr, "delivery_module: liblogosdelivery context closed\n");
        m_ctx = nullptr; // nothing more will come; waiters see RET_CLOSED
        break;
    default:
        break;
    }
}

void PollPump::reverseReply(uint64_t callId, int ret, const json& payload)
{
    void* ctx = m_ctx; // read without the lock: replies come from other threads
    if (!ctx) return;
    if (ret == RET_OK) {
        const auto bytes = json::to_cbor(payload);
        logosdelivery_reverse_reply(ctx, callId, ret, bytes.data(), bytes.size());
    } else {
        const std::string text = payload.is_string() ? payload.get<std::string>() : payload.dump();
        logosdelivery_reverse_reply(ctx, callId, ret,
                                    reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }
}
