#include "rln_bridge.h"
#include "liblogos_rln_module_api.h" // generated from metadata.json#optional_dependencies

#include <cstdio>
#include <memory>

#include <nlohmann/json.hpp>

#include <liblogosdelivery_rln.h> // logosdelivery_rln_response
#include <logos_protocol.h>       // lp_* C ABI

using nlohmann::json;

namespace {

constexpr const char* kTarget = "liblogos_rln_module";
constexpr const char* kOrigin = "delivery_module";

// The RLN module's documented internal worst case for a registry read: 70 s.
// The delivery library's own per-op budget usually expires first; a late
// completion is dropped by logosdelivery_rln_response (non-zero return).
constexpr int kReadMs = 70'000;

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
    // lp_client_destroy waits for a reply callback that is mid-flight to
    // finish, then guarantees no further callback fires. A call still in
    // flight loses its callback and leaks its small Pending — bounded, once,
    // at shutdown.
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
    if (!m_client) {
        return "rln bridge has no lp client";
    }
    // No contact with the RLN module here: at enable() time (createNode) the
    // registry connection is not up yet, so acquiring the remote object would
    // fail. First contact is deferred to the first real call, by which point
    // the registry is connected. Those calls arrive on a library thread, so
    // the acquire is handed off to the lp owner thread rather than blocking
    // the caller.
    m_enabled.store(true, std::memory_order_release);
    return {};
}

bool RlnBridge::isTstrOp(Op op)
{
    return op == Op::GetState;
}

// Mirrors the library's budgets (transport.nim: RlnLocalTimeout 10 s,
// RlnRegistryReadTimeout 80 s). Module-driven lifecycle ops have no library
// clock; they borrow the registry-read budget.
int RlnBridge::budgetMsFor(Op op)
{
    switch (op) {
    case Op::Start:
    case Op::Stop:
    case Op::GetState:
    case Op::Generate:
        return 80'000;
    default:
        return 10'000; // get_epoch_quota, validate_proof
    }
}

const char* RlnBridge::opName(Op op)
{
    switch (op) {
    case Op::Start:
        return "start";
    case Op::Stop:
        return "stop";
    case Op::GetState:
        return "get_membership_state";
    case Op::GetQuota:
        return "get_epoch_quota";
    case Op::Generate:
        return "generate_proof";
    case Op::Validate:
        return "validate_proof";
    }
    return "unknown";
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

std::string RlnBridge::reshapeReply(Op op, bool ok, const char* jsonText)
{
    const std::string body = jsonText ? jsonText : "";
    if (!ok) {
        // lp reports failure via the flag; its error json carries
        // {"code","message","origin"}.
        json err = json::parse(body, nullptr, /*allow_exceptions=*/false);
        const std::string msg =
            err.is_object() && err.contains("message") && err["message"].is_string()
                ? err["message"].get<std::string>()
                : body;
        return transportFail(op, "transient", "rln_bridge_transport",
                             std::string(opName(op)) + ": " + msg);
    }
    // A tstr method's lp result is a JSON string holding the module's compact
    // reply — forward the CONTENT (the library's parsers tolerate one leftover
    // string layer either way).
    json parsed = json::parse(body, nullptr, /*allow_exceptions=*/false);
    std::string content = body;
    if (parsed.is_string()) {
        content = parsed.get<std::string>();
        parsed = json::parse(content, nullptr, /*allow_exceptions=*/false);
    }
    // A dispatch rejection arrives as success=false with NO error text, while
    // every genuine module failure carries a message. Treat the empty refusal
    // as a retryable transport failure, not as the module's answer.
    if (parsed.is_object() && parsed.value("success", true) == false) {
        const auto err = parsed.value("error", json());
        if (err.is_null() || (err.is_string() && err.get<std::string>().empty())) {
            return transportFail(op, "transient", "rln_bridge_transport",
                std::string(opName(op)) + ": dispatch rejected (refusal with no message)");
        }
    }
    return content; // the module's reply, verbatim
}

void RlnBridge::onLpReply(int ok, const char* jsonText, void* userData)
{
    std::unique_ptr<Pending> call(static_cast<Pending*>(userData));
    if (!call) {
        return;
    }
    const std::string out = reshapeReply(call->op, ok != 0, jsonText);
    // Non-zero: the library already timed out this reqId — nothing to do.
    (void)logosdelivery_rln_response(call->reqId, out.c_str());
}

void RlnBridge::sendAsync(Op op, uint64_t reqId, const std::string& method,
                          const std::string& argsJson, int timeoutMs)
{
    if (!m_client) {
        const std::string out = transportFail(op, "transient", "rln_bridge_transport",
            method + ": lp client not initialized");
        (void)logosdelivery_rln_response(reqId, out.c_str());
        return;
    }
    auto* call = new Pending{reqId, op};
    const int rc = lp_invoke_async(m_client, method.c_str(), argsJson.c_str(),
                                   timeoutMs, &onLpReply, call);
    if (rc != LP_OK) {
        delete call; // callback will never fire
        const std::string out = transportFail(op, "transient", "rln_bridge_transport",
            method + ": lp_invoke_async rc=" + std::to_string(rc));
        (void)logosdelivery_rln_response(reqId, out.c_str());
    }
}

// --- op entry points ---------------------------------------------------------

std::string RlnBridge::lifecycleResult(Op op, const StdLogosResult& r,
                                       const logos::CallError& err)
{
    // Transport failure (lp error, protocol timeout, module not loaded): the
    // decoded result is meaningless — branch on CallError only.
    if (!err.ok()) {
        return transportFail(op, "transient", "rln_bridge_transport",
            std::string(opName(op)) + ": " + err.code + ": " + err.message);
    }
    // A dispatch rejection arrives as success=false with NO error text, while
    // every genuine module failure carries a message. Treat the empty refusal
    // as a retryable transport failure, not as the module's answer.
    if (!r.success && r.error.empty()) {
        return transportFail(op, "transient", "rln_bridge_transport",
            std::string(opName(op)) + ": dispatch rejected (refusal with no message)");
    }
    // This bridge reports lifecycle success as empty text; anything else is the
    // module's error envelope.
    if (r.success) {
        return {};
    }
    return resultEnvelope(r);
}

std::string RlnBridge::startBackend(std::string configJson)
{
    if (!m_enabled.load(std::memory_order_acquire)) {
        return "rln bridge is not enabled";
    }
    logos::CallError err;
    const StdLogosResult r = m_typed->start(configJson, &err);
    return lifecycleResult(Op::Start, r, err);
}

std::string RlnBridge::stopBackend()
{
    if (!m_enabled.load(std::memory_order_acquire)) {
        return "rln bridge is not enabled";
    }
    logos::CallError err;
    const StdLogosResult r = m_typed->stop(&err);
    return lifecycleResult(Op::Stop, r, err);
}

void RlnBridge::getMembershipState(uint64_t reqId, std::string registryId,
                                   std::string rlnIdentifier)
{
    const json args = json::array({registryId, rlnIdentifier});
    sendAsync(Op::GetState, reqId, "get_membership_state", args.dump(), kReadMs);
}

void RlnBridge::getEpochQuota(uint64_t reqId, std::string registryId,
                              std::string rlnIdentifier, uint64_t timestamp)
{
    // module wants the timestamp as a STRING
    const json args = json::array({registryId, rlnIdentifier,
                                   std::to_string(timestamp)});
    sendAsync(Op::GetQuota, reqId, "get_epoch_quota", args.dump(),
              budgetMsFor(Op::GetQuota));
}

void RlnBridge::generateProof(uint64_t reqId, std::string registryId,
                              std::string rlnIdentifier, std::string signalHex,
                              uint64_t timestamp)
{
    // module wants the timestamp as a STRING
    const json args = json::array({registryId, rlnIdentifier, signalHex,
                                   std::to_string(timestamp)});
    sendAsync(Op::Generate, reqId, "generate_proof", args.dump(), kReadMs);
}

void RlnBridge::validateProof(uint64_t reqId, std::string registryId,
                              std::string rlnIdentifier, std::string signalHex,
                              uint64_t timestamp, std::string proofJson)
{
    // module wants the timestamp as a STRING
    const json args = json::array({registryId, rlnIdentifier, signalHex,
                                   std::to_string(timestamp), proofJson});
    sendAsync(Op::Validate, reqId, "validate_proof", args.dump(),
              budgetMsFor(Op::Validate));
}

