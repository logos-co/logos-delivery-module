#include "rln_bridge.h"
#include "liblogos_rln_module_api.h" // generated from metadata.json#dependencies

#include <cstdio>
#include <memory>
#include <semaphore>
#include <sstream> // TEMP: debug tracing
#include <thread>  // TEMP: debug tracing (std::this_thread::get_id)

#include <nlohmann/json.hpp>

#include <liblogosdelivery_rln.h> // logosdelivery_rln_response
#include <logos_protocol.h>       // lp_* C ABI

using nlohmann::json;

// ===== TEMPORARY debug tracing — remove before merge =========================
#define RLN_BRIDGE_TRACE 1
#if RLN_BRIDGE_TRACE
namespace {
// Set once per worker thread in laneLoop. Anything else — the delivery
// library's callback threads, the lp client's owner thread — prints its raw
// std::thread::id, so a TRAMPOLINE line is visibly on a different thread from
// the worker blocked waiting for it.
thread_local std::string t_who;

const std::string& who()
{
    if (t_who.empty()) {
        std::ostringstream os;
        os << "thread-" << std::this_thread::get_id();
        t_who = os.str();
    }
    return t_who;
}

// Payloads here are proofs and configs; keep one event to one line.
std::string brief(const std::string& s, size_t n = 160)
{
    return s.size() <= n
        ? s
        : s.substr(0, n) + "...<+" + std::to_string(s.size() - n) + " more>";
}
} // namespace
#define RLN_TRACE(fmt, ...)                                                    \
    fprintf(stderr, "[rln_bridge][%-20s] " fmt "\n",                           \
            who().c_str() __VA_OPT__(, ) __VA_ARGS__)
#else
#define RLN_TRACE(...)                                                         \
    do {                                                                       \
    } while (0)
#endif
// =============================================================================

namespace {

constexpr const char* kTarget = "liblogos_rln_module";
constexpr const char* kOrigin = "delivery_module";

// The RLN module's documented internal worst cases: registry reads up to 70 s,
// a register submission up to 190 s. The delivery library's own per-op budget
// usually expires first; a late completion is dropped by
// logosdelivery_rln_response (non-zero return).
constexpr int kReadMs = 70'000;
constexpr int kRegisterMs = 190'000;

// One in-flight raw lp call: the trampoline fills the box and releases the
// semaphore from the lp client's owner thread. shared_ptr keeps the box alive
// for a reply that lands after the wait already timed out.
struct ReplyBox {
    std::binary_semaphore sem{0};
    bool ok = false;
    std::string json;
};

void replyTrampoline(int ok, const char* jsonText, void* userData)
{
    std::unique_ptr<std::shared_ptr<ReplyBox>> boxPtr(
        static_cast<std::shared_ptr<ReplyBox>*>(userData));
    auto& box = **boxPtr;
    RLN_TRACE("  raw[%p] TRAMPOLINE fires (lp owner thread), handoff=%p ok=%d payload=%s",
              (void*)&box, userData, ok,
              jsonText ? brief(jsonText).c_str() : "<null>");
    box.ok = ok != 0;
    if (jsonText) {
        box.json = jsonText;
    }
    RLN_TRACE("  raw[%p] TRAMPOLINE releasing semaphore (waiter may already have timed out)",
              (void*)&box);
    box.sem.release();
}

// The module's result envelope, rebuilt from the typed client's decode in the
// same (alphabetical) key order the protocol layer serializes.
std::string resultEnvelope(const StdLogosResult& r)
{
    const std::string out =
        r.success
        ? json{{"error", nullptr}, {"success", true}, {"value", r.value}}.dump()
        : json{{"error", r.error}, {"success", false}, {"value", nullptr}}.dump();
    RLN_TRACE("  xform StdLogosResult{success=%d value=%s error=%s} -> envelope %s",
              (int)r.success, brief(r.value.dump()).c_str(), brief(r.error).c_str(),
              brief(out).c_str());
    return out;
}

} // namespace

// TEMP: debug tracing
const char* RlnBridge::opName(Op op)
{
    switch (op) {
    case Op::Start:    return "Start";
    case Op::Stop:     return "Stop";
    case Op::Register: return "Register";
    case Op::GetState: return "GetState";
    case Op::GetQuota: return "GetQuota";
    case Op::Generate: return "Generate";
    case Op::Validate: return "Validate";
    }
    return "?";
}

RlnBridge::RlnBridge(LiblogosRlnModule* typed) : m_typed(typed) {}

RlnBridge::~RlnBridge()
{
    RLN_TRACE("dtor: signalling stop, waking both lanes");
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_stopping = true;
    }
    m_slow.cv.notify_all();
    m_fast.cv.notify_all();
    if (m_slow.worker.joinable()) {
        m_slow.worker.join();
    }
    if (m_fast.worker.joinable()) {
        m_fast.worker.join();
    }
    if (m_client) {
        RLN_TRACE("dtor: both workers joined, destroying lp client");
        lp_client_destroy(m_client);
    }
}

void RlnBridge::init()
{
    if (m_client) {
        return;
    }
    m_client = lp_client_create(kTarget, kOrigin, nullptr, nullptr);
    RLN_TRACE("init: lp_client_create(target=%s origin=%s) -> %p"
              "  [THIS THREAD becomes the lp owner]",
              kTarget, kOrigin, (void*)m_client);
    if (!m_client) {
        fprintf(stderr, "delivery_module: rln bridge lp_client_create failed for %s\n",
                kTarget);
    }
}

std::string RlnBridge::enable()
{
    if (!m_typed) {
        return "rln bridge has no typed client";
    }
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_lanesRunning) {
        RLN_TRACE("enable: spawning slow + fast worker threads");
        m_slow.worker = std::thread(&RlnBridge::laneLoop, this, &m_slow);
        m_fast.worker = std::thread(&RlnBridge::laneLoop, this, &m_fast);
        m_lanesRunning = true;
    }
    m_enabled.store(true, std::memory_order_release);
    return {};
}

bool RlnBridge::isSlowOp(Op op)
{
    return op == Op::Register || op == Op::GetState || op == Op::Generate;
}

bool RlnBridge::isTstrOp(Op op)
{
    return op == Op::Register || op == Op::GetState;
}

std::string RlnBridge::transportFail(Op op, const std::string& cls,
                                     const std::string& kind, const std::string& msg)
{
    const json errorObj{{"class", cls}, {"kind", kind}, {"message", msg}};
    // result envelope: its error arm is a JSON-ENCODED object.
    const std::string out =
        isTstrOp(op) ? json{{"error", errorObj}}.dump()
                     : json{{"success", false}, {"error", errorObj.dump()}}.dump();
    RLN_TRACE("  FABRICATE op=%s shape=%s -> %s", opName(op),
              isTstrOp(op) ? "tstr" : "envelope", brief(out).c_str());
    return out;
}

void RlnBridge::enqueue(Job job)
{
    const Op op = job.op; // TEMP: read before the move
    const uint64_t reqId = job.reqId;
    Lane& lane = isSlowOp(job.op) ? m_slow : m_fast;
    const char* laneName = (&lane == &m_slow) ? "slow" : "fast";
    size_t depth = 0;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        lane.queue.push_back(std::move(job));
        depth = lane.queue.size();
    }
    // Traced outside the lock: never hold m_lock across stderr I/O.
    RLN_TRACE("ENQUEUE  op=%-8s reqId=%llu -> %s lane (depth now %zu)"
              "  [caller thread returns immediately]",
              opName(op), (unsigned long long)reqId, laneName, depth);
    (void)op;
    (void)reqId;
    (void)laneName;
    (void)depth;
    lane.cv.notify_one();
}

// --- op entry points ---------------------------------------------------------

void RlnBridge::start(uint64_t reqId, std::string configJson)
{
    Job j;
    j.reqId = reqId;
    j.op = Op::Start;
    j.configJson = std::move(configJson);
    enqueue(std::move(j));
}

void RlnBridge::stop(uint64_t reqId)
{
    Job j;
    j.reqId = reqId;
    j.op = Op::Stop;
    enqueue(std::move(j));
}

void RlnBridge::registerMembership(uint64_t reqId, std::string registryId,
                                   std::string rlnIdentifier, std::string optionsJson)
{
    Job j;
    j.reqId = reqId;
    j.op = Op::Register;
    j.registryId = std::move(registryId);
    j.rlnIdentifier = std::move(rlnIdentifier);
    j.optionsJson = std::move(optionsJson);
    enqueue(std::move(j));
}

void RlnBridge::getMembershipState(uint64_t reqId, std::string registryId,
                                   std::string rlnIdentifier)
{
    Job j;
    j.reqId = reqId;
    j.op = Op::GetState;
    j.registryId = std::move(registryId);
    j.rlnIdentifier = std::move(rlnIdentifier);
    enqueue(std::move(j));
}

void RlnBridge::getEpochQuota(uint64_t reqId, std::string registryId,
                              std::string rlnIdentifier, uint64_t timestamp)
{
    Job j;
    j.reqId = reqId;
    j.op = Op::GetQuota;
    j.registryId = std::move(registryId);
    j.rlnIdentifier = std::move(rlnIdentifier);
    j.timestamp = timestamp;
    enqueue(std::move(j));
}

void RlnBridge::generateProof(uint64_t reqId, std::string registryId,
                              std::string rlnIdentifier, std::string signalHex,
                              uint64_t timestamp)
{
    Job j;
    j.reqId = reqId;
    j.op = Op::Generate;
    j.registryId = std::move(registryId);
    j.rlnIdentifier = std::move(rlnIdentifier);
    j.signalHex = std::move(signalHex);
    j.timestamp = timestamp;
    enqueue(std::move(j));
}

void RlnBridge::validateProof(uint64_t reqId, std::string registryId,
                              std::string rlnIdentifier, std::string signalHex,
                              uint64_t timestamp, std::string proofJson)
{
    Job j;
    j.reqId = reqId;
    j.op = Op::Validate;
    j.registryId = std::move(registryId);
    j.rlnIdentifier = std::move(rlnIdentifier);
    j.signalHex = std::move(signalHex);
    j.timestamp = timestamp;
    j.proofJson = std::move(proofJson);
    enqueue(std::move(j));
}

// --- serving -----------------------------------------------------------------

void RlnBridge::laneLoop(Lane* lane)
{
    const char* laneName = (lane == &m_slow) ? "slow" : "fast";
#if RLN_BRIDGE_TRACE
    t_who = std::string(laneName) + "-worker";
#endif
    RLN_TRACE("WORKER up, serving the %s lane", laneName);
    for (;;) {
        Job job;
        size_t remaining = 0;
        {
            std::unique_lock<std::mutex> lock(m_lock);
            lane->cv.wait(lock, [&] { return m_stopping || !lane->queue.empty(); });
            if (m_stopping) {
                RLN_TRACE("WORKER exiting (stopping)");
                return;
            }
            job = std::move(lane->queue.front());
            lane->queue.pop_front();
            remaining = lane->queue.size();
        }
        RLN_TRACE("DEQUEUE  op=%-8s reqId=%llu (%zu left in %s lane)",
                  opName(job.op), (unsigned long long)job.reqId, remaining, laneName);
        (void)remaining;
        (void)laneName;
        std::string out;
        try {
            out = serveOp(job);
        } catch (const std::exception& e) {
            RLN_TRACE("  EXCEPTION escaped serveOp: %s", e.what());
            out = transportFail(job.op, "permanent", "bridge_exception",
                std::string("rln bridge exception: ") + e.what());
        }
        // Non-zero: the library already timed out this reqId — nothing to do.
        const int rc = logosdelivery_rln_response(job.reqId, out.c_str());
        RLN_TRACE("RESPOND  op=%-8s reqId=%llu rc=%d%s reply=%s",
                  opName(job.op), (unsigned long long)job.reqId, rc,
                  rc ? "  <-- DROPPED, library already timed out" : "",
                  brief(out).c_str());
        (void)rc;
    }
}

bool RlnBridge::invokeRaw(const std::string& method, const std::string& argsJson,
                          int timeoutMs, std::string& out, std::string& errMsg)
{
    if (!m_client) {
        errMsg = method + ": lp client not initialized";
        return false;
    }
    auto box = std::make_shared<ReplyBox>();
    auto* handoff = new std::shared_ptr<ReplyBox>(box);
    RLN_TRACE("  raw[%p] %s: box created, handoff=%p args=%s timeout=%dms",
              (void*)box.get(), method.c_str(), (void*)handoff,
              brief(argsJson).c_str(), timeoutMs);
    const int rc = lp_invoke_async(m_client, method.c_str(), argsJson.c_str(),
                                   timeoutMs, &replyTrampoline, handoff);
    RLN_TRACE("  raw[%p] %s: lp_invoke_async rc=%d (DISPATCHED only, not completed)",
              (void*)box.get(), method.c_str(), rc);
    if (rc != LP_OK) {
        RLN_TRACE("  raw[%p] %s: dispatch failed -> deleting handoff, trampoline will NEVER fire",
                  (void*)box.get(), method.c_str());
        delete handoff; // callback will never fire
        errMsg = method + ": lp_invoke_async rc=" + std::to_string(rc);
        return false;
    }
    // The protocol enforces timeoutMs; the margin only guards a callback that
    // never fires.
    const auto wait = std::chrono::milliseconds(timeoutMs) + std::chrono::seconds(10);
    RLN_TRACE("  raw[%p] %s: BLOCKING on semaphore up to %dms+10s (box use_count=%ld)",
              (void*)box.get(), method.c_str(), timeoutMs, (long)box.use_count());
    if (!box->sem.try_acquire_for(wait)) {
        RLN_TRACE("  raw[%p] %s: WOKE BY TIMEOUT - no trampoline. Worker drops its ref; "
                  "the handoff keeps the box alive for a late reply (use_count=%ld)",
                  (void*)box.get(), method.c_str(), (long)box.use_count());
        errMsg = method + ": no lp completion within " + std::to_string(timeoutMs) +
            "ms (+10s)";
        return false;
    }
    RLN_TRACE("  raw[%p] %s: WOKE BY TRAMPOLINE. ok=%d raw=%s",
              (void*)box.get(), method.c_str(), (int)box->ok, brief(box->json).c_str());
    if (!box->ok) {
        json err = json::parse(box->json, nullptr, /*allow_exceptions=*/false);
        errMsg = method + ": " +
            (err.is_object() && err.contains("message") && err["message"].is_string()
                    ? err["message"].get<std::string>()
                    : box->json);
        RLN_TRACE("  raw[%p] %s: lp reported failure -> errMsg=%s",
                  (void*)box.get(), method.c_str(), errMsg.c_str());
        return false;
    }
    // A tstr method's lp result is a JSON string holding the module's compact
    // reply — forward the CONTENT (the library's parsers tolerate one leftover
    // string layer either way).
    json parsed = json::parse(box->json, nullptr, /*allow_exceptions=*/false);
    const bool unwrapped = parsed.is_string();
    out = unwrapped ? parsed.get<std::string>() : box->json;
    RLN_TRACE("  raw[%p] %s: xform %s -> out=%s", (void*)box.get(), method.c_str(),
              unwrapped ? "reply WAS a JSON string layer, unwrapped one level"
                        : "reply forwarded verbatim (no string layer)",
              brief(out).c_str());
    (void)unwrapped;
    return true;
}

std::string RlnBridge::serveOp(const Job& job)
{
    if (!isSlowOp(job.op)) {
        return serveFast(job);
    }
    const std::string ts = std::to_string(job.timestamp); // module wants a STRING

    std::string method;
    json args = json::array();
    int timeoutMs = kReadMs;
    switch (job.op) {
    case Op::Register:
        method = "register_membership";
        args = json::array({job.registryId, job.rlnIdentifier, job.optionsJson});
        timeoutMs = kRegisterMs;
        break;
    case Op::GetState:
        method = "get_membership_state";
        args = json::array({job.registryId, job.rlnIdentifier});
        break;
    case Op::Generate:
        method = "generate_proof";
        args = json::array({job.registryId, job.rlnIdentifier, job.signalHex, ts});
        break;
    default:
        break; // fast ops returned above
    }

    std::string out;
    std::string errMsg;
    RLN_TRACE("SERVE-SLOW op=%s -> raw lp method=%s args=%s timeout=%dms",
              opName(job.op), method.c_str(), brief(args.dump()).c_str(), timeoutMs);
    if (!invokeRaw(method, args.dump(), timeoutMs, out, errMsg)) {
        return transportFail(job.op, "transient", "rln_bridge_transport", errMsg);
    }
    return out; // the module's reply, verbatim
}

std::string RlnBridge::serveFast(const Job& job)
{
    const std::string ts = std::to_string(job.timestamp); // module wants a STRING

    RLN_TRACE("SERVE-FAST op=%s -> typed client (protocol default 20s timeout)",
              opName(job.op));
    logos::CallError err;
    StdLogosResult r;
    const char* method = "";
    switch (job.op) {
    case Op::Start:
        method = "start";
        r = m_typed->start(job.configJson, &err);
        break;
    case Op::Stop:
        method = "stop";
        r = m_typed->stop(&err);
        break;
    case Op::GetQuota:
        method = "get_epoch_quota";
        r = m_typed->get_epoch_quota(job.registryId, job.rlnIdentifier, ts, &err);
        break;
    case Op::Validate:
        method = "validate_proof";
        r = m_typed->validate_proof(job.registryId, job.rlnIdentifier, job.signalHex,
                                    ts, job.proofJson, &err);
        break;
    default:
        return transportFail(job.op, "permanent", "bridge_exception",
            "slow op routed to the typed fast path");
    }
    // Transport failure (lp error, protocol timeout, module not loaded): the
    // decoded result is meaningless — branch on CallError only.
    if (!err.ok()) {
        RLN_TRACE("  typed %s: TRANSPORT error code=%s message=%s (result is meaningless)",
                  method, err.code.c_str(), err.message.c_str());
        return transportFail(job.op, "transient", "rln_bridge_transport",
            std::string(method) + ": " + err.code + ": " + err.message);
    }
    RLN_TRACE("  typed %s: returned success=%d error=%s", method, (int)r.success,
              brief(r.error).c_str());
    // A dispatch rejection arrives as success=false with NO error text, while
    // every genuine module failure carries a message. Treat the empty refusal
    // as a retryable transport failure, not as the module's answer.
    if (!r.success && r.error.empty()) {
        RLN_TRACE("  typed %s: empty refusal -> reclassified as TRANSIENT transport failure",
                  method);
        return transportFail(job.op, "transient", "rln_bridge_transport",
            std::string(method) + ": dispatch rejected (refusal with no message)");
    }
    return resultEnvelope(r);
}
