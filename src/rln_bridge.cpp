#include "rln_bridge.h"
#include "liblogos_rln_module_api.h" // generated from metadata.json#dependencies

#include <cstdio>
#include <memory>
#include <semaphore>

#include <nlohmann/json.hpp>

#include <liblogosdelivery_rln.h> // logosdelivery_rln_response
#include <logos_protocol.h>       // lp_* C ABI

using nlohmann::json;

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
    box.ok = ok != 0;
    if (jsonText) {
        box.json = jsonText;
    }
    box.sem.release();
}

// The module's result envelope, rebuilt from the typed client's decode in the
// same (alphabetical) key order the protocol layer serializes.
std::string resultEnvelope(const StdLogosResult& r)
{
    if (r.success) {
        return json{{"error", nullptr}, {"success", true}, {"value", r.value}}.dump();
    }
    return json{{"error", r.error}, {"success", false}, {"value", nullptr}}.dump();
}

} // namespace

RlnBridge::RlnBridge() = default;

RlnBridge::~RlnBridge()
{
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
        lp_client_destroy(m_client);
    }
}

void RlnBridge::init(LiblogosRlnModule* typed)
{
    m_typed = typed;
    if (m_client) {
        return;
    }
    m_client = lp_client_create(kTarget, kOrigin, nullptr, nullptr);
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
    if (isTstrOp(op)) {
        return json{{"error", errorObj}}.dump();
    }
    // result envelope: its error arm is a JSON-ENCODED object.
    return json{{"success", false}, {"error", errorObj.dump()}}.dump();
}

void RlnBridge::enqueue(Job job)
{
    Lane& lane = isSlowOp(job.op) ? m_slow : m_fast;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        lane.queue.push_back(std::move(job));
    }
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
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_lock);
            lane->cv.wait(lock, [&] { return m_stopping || !lane->queue.empty(); });
            if (m_stopping) {
                return;
            }
            job = std::move(lane->queue.front());
            lane->queue.pop_front();
        }
        std::string out;
        try {
            out = serveOp(job);
        } catch (const std::exception& e) {
            out = transportFail(job.op, "permanent", "bridge_exception",
                std::string("rln bridge exception: ") + e.what());
        }
        // Non-zero: the library already timed out this reqId — nothing to do.
        (void)logosdelivery_rln_response(job.reqId, out.c_str());
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
    const int rc = lp_invoke_async(m_client, method.c_str(), argsJson.c_str(),
                                   timeoutMs, &replyTrampoline, handoff);
    if (rc != LP_OK) {
        delete handoff; // callback will never fire
        errMsg = method + ": lp_invoke_async rc=" + std::to_string(rc);
        return false;
    }
    // The protocol enforces timeoutMs; the margin only guards a callback that
    // never fires.
    const auto wait = std::chrono::milliseconds(timeoutMs) + std::chrono::seconds(10);
    if (!box->sem.try_acquire_for(wait)) {
        errMsg = method + ": no lp completion within " + std::to_string(timeoutMs) +
            "ms (+10s)";
        return false;
    }
    if (!box->ok) {
        json err = json::parse(box->json, nullptr, /*allow_exceptions=*/false);
        errMsg = method + ": " +
            (err.is_object() && err.contains("message") && err["message"].is_string()
                    ? err["message"].get<std::string>()
                    : box->json);
        return false;
    }
    // A tstr method's lp result is a JSON string holding the module's compact
    // reply — forward the CONTENT (the library's parsers tolerate one leftover
    // string layer either way).
    json parsed = json::parse(box->json, nullptr, /*allow_exceptions=*/false);
    out = parsed.is_string() ? parsed.get<std::string>() : box->json;
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
    if (!invokeRaw(method, args.dump(), timeoutMs, out, errMsg)) {
        return transportFail(job.op, "transient", "rln_bridge_transport", errMsg);
    }
    return out; // the module's reply, verbatim
}

std::string RlnBridge::serveFast(const Job& job)
{
    const std::string ts = std::to_string(job.timestamp); // module wants a STRING

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
        return transportFail(job.op, "transient", "rln_bridge_transport",
            std::string(method) + ": " + err.code + ": " + err.message);
    }
    // A dispatch rejection arrives as success=false with NO error text, while
    // every genuine module failure carries a message. Treat the empty refusal
    // as a retryable transport failure, not as the module's answer.
    if (!r.success && r.error.empty()) {
        return transportFail(job.op, "transient", "rln_bridge_transport",
            std::string(method) + ": dispatch rejected (refusal with no message)");
    }
    return resultEnvelope(r);
}
