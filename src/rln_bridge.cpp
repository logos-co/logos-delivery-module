#include "rln_bridge.h"
#include "liblogos_rln_module_api.h" // generated from metadata.json#optional_dependencies

#include <nlohmann/json.hpp>

#include <logos_async_result.h>   // logos::AsyncResult
#include <logos_call_error.h>     // logos::CallError

using nlohmann::json;

namespace {

constexpr const char* kTransient = "transient";
constexpr const char* kPermanent = "permanent";
constexpr const char* kTransportKind = "rln_bridge_transport";
constexpr const char* kRefusalKind = "rln_bridge_dispatch";
constexpr const char* kDecodeKind = "rln_bridge_decode";
constexpr const char* kDisabledKind = "rln_bridge_disabled";

// A provider REFUSAL: the module was reached and declined the call itself —
// the method is unknown, the arguments do not fit, dispatch failed. The
// generated client folds that canonical rejection into the CallError, so it
// arrives typed instead of being guessed at from an empty error field.
//
// It is PERMANENT, unlike every other CallError here. A refusal means this
// bridge and the module disagree about the contract, which retrying cannot
// fix; classified transient, the delivery library would retry it forever.
bool isRefusal(const logos::CallError& err)
{
    return err.code == "invalid_args" || err.code == "unknown_method"
        || err.code == "dispatch_failed";
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

// Nothing to tear down: this object owns no client. The typed client belongs
// to modules() and is destroyed after this bridge, so a call still in flight
// here keeps a live callee — and its completion runs a callback that touches
// no bridge state, which is what makes that safe rather than lucky.
RlnBridge::~RlnBridge() = default;

void RlnBridge::init(LiblogosRlnModule* rlnModule)
{
    m_rlnModule = rlnModule;
}

std::string RlnBridge::enable()
{
    if (!m_rlnModule) {
        return "rln-module client is not set for rln bridge";
    }
    // No contact with the RLN module here: at enable() time (createNode) the
    // registry connection is not up yet, so acquiring the remote object would
    // fail. First contact is deferred to the first real call, by which point
    // the registry is connected — the typed client creates its own lp client
    // lazily, on whichever thread calls first, for exactly that reason.
    m_enabled.store(true, std::memory_order_release);
    return {};
}

bool RlnBridge::isTstrOp(Op op)
{
    return op == Op::GetState;
}

// Mirrors the library's budgets (transport.nim: RlnLocalTimeout 10 s,
// RlnRegistryReadTimeout 80 s). Two rules hold this table together:
//
//   - a request op's deadline must stay STRICTLY UNDER the library's, or the
//     library always answers first and the bridge's real reply is discarded as
//     a late completion;
//   - a value <= 0 selects the protocol default, so no arm may fall through to
//     zero by accident.
//
// Lifecycle has no library clock behind it: 20 s is what these calls already
// got from the protocol default, written down so it is greppable rather than
// inherited.
int RlnBridge::timeoutMsFor(Op op)
{
    switch (op) {
    case Op::GetState:
    case Op::Generate:
        return 70'000;
    case Op::GetQuota:
    case Op::Validate:
        return 10'000;
    case Op::Start:
    case Op::Stop:
        return 20'000;
    }
    return 10'000;
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

namespace {
RlnBridge::Responder& responder()
{
    static RlnBridge::Responder r;
    return r;
}
} // namespace

void RlnBridge::setResponder(Responder r)
{
    responder() = std::move(r);
}

void RlnBridge::respond(uint64_t reqId, const std::string& out)
{
    if (responder()) {
        responder()(reqId, out);
    }
}

std::string RlnBridge::callFail(Op op, const logos::CallError& err)
{
    const std::string msg =
        std::string(opName(op)) + ": " + err.code + ": " + err.message;
    if (isRefusal(err)) {
        return transportFail(op, kPermanent, kRefusalKind, msg);
    }
    return transportFail(op, kTransient, kTransportKind, msg);
}

// Only reachable once the call is known to have succeeded. On a FAILED call
// the decoded value is not default — it carries the SDK's own
// "expected a result object, got null" — so reading it without that gate
// reports a decode artifact to the library as if it were the module's error.
std::string RlnBridge::resultReply(Op op, const StdLogosResult& r)
{
    // Residue of the heuristic this bridge used before refusals were typed:
    // success=false with no error text. The client now folds every canonical
    // refusal into the CallError and gives a non-object reply a message of its
    // own, so what is left here is a module that answered nothing usable.
    if (!r.success && r.error.empty()) {
        return transportFail(op, kPermanent, kDecodeKind,
                             std::string(opName(op)) + ": refusal with no message");
    }
    return resultEnvelope(r);
}

// A tstr method's decoded value IS the module's compact reply — the client
// already stripped the transport's string layer. Forward it whole, an in-band
// {"error":{…}} included: this bridge does not model that schema.
std::string RlnBridge::tstrReply(Op op, const std::string& value)
{
    if (value.empty()) {
        // A successful call whose reply was not a string decodes to "", which
        // is not a document the library can parse.
        return transportFail(op, kPermanent, kDecodeKind,
                             std::string(opName(op)) + ": empty reply");
    }
    return value;
}

// Permanent, not transient: enable() runs once at createNode, so a bridge that
// is not serving now never will be for this node. Told that, the library can
// fail the request instead of retrying it until its own budget runs out.
bool RlnBridge::rejectIfNotEnabled(Op op, uint64_t reqId) const
{
    if (m_enabled.load(std::memory_order_acquire)) {
        return false;
    }
    const char* why = m_rlnModule ? "rln bridge is not enabled"
                                  : "rln-module client is not set for rln bridge";
    respond(reqId, transportFail(op, kPermanent, kDisabledKind,
                                 std::string(opName(op)) + ": " + why));
    return true;
}

std::string RlnBridge::lifecycleResult(Op op, const StdLogosResult& r,
                                       const logos::CallError& err)
{
    // The call never produced the module's answer — transport failure,
    // deadline, module not loaded, or a refusal the client folded in. The
    // decoded result is meaningless: branch on CallError only.
    if (!err.ok()) {
        return callFail(op, err);
    }
    // This bridge reports lifecycle success as empty text; anything else is
    // the module's error envelope.
    if (r.success) {
        return {};
    }
    return resultReply(op, r);
}

std::string RlnBridge::startBackend(std::string configJson)
{
    if (!m_enabled.load(std::memory_order_acquire)) {
        return "rln bridge is not enabled";
    }
    logos::CallError err;
    const StdLogosResult r =
        m_rlnModule->start(configJson, &err, timeoutMsFor(Op::Start));
    return lifecycleResult(Op::Start, r, err);
}

std::string RlnBridge::stopBackend()
{
    if (!m_enabled.load(std::memory_order_acquire)) {
        return "rln bridge is not enabled";
    }
    logos::CallError err;
    const StdLogosResult r = m_rlnModule->stop(&err, timeoutMsFor(Op::Stop));
    return lifecycleResult(Op::Stop, r, err);
}

// The op entry points below carry no precondition: every one of them answers
// the reqId it was handed, whether or not this bridge can serve it. A caller
// that checked first would decide the library's fate for it — an unserved
// request would simply go quiet and expire against the library's own budget,
// with nothing said about why.
//
// Every callback below captures EXACTLY [reqId] — never `this`, never [=],
// which in a member function captures this implicitly. The callback can run on
// the client's thread after this object is gone, so it calls only static
// helpers; a capture of this is the one edit that would make that unsafe.
//
// The timestamp crosses as a STRING: the module's contract asks for it that
// way, and a bare number would arrive as the wrong type.

void RlnBridge::getMembershipState(uint64_t reqId, std::string registryId,
                                   std::string rlnIdentifier)
{
    if (rejectIfNotEnabled(Op::GetState, reqId)) {
        return;
    }
    m_rlnModule->get_membership_stateAsyncResult(
        registryId, rlnIdentifier,
        [reqId](logos::AsyncResult<std::string> r) {
            respond(reqId, r.ok() ? tstrReply(Op::GetState, r.value)
                                  : callFail(Op::GetState, r.error));
        },
        timeoutMsFor(Op::GetState));
}

void RlnBridge::getEpochQuota(uint64_t reqId, std::string registryId,
                              std::string rlnIdentifier, uint64_t timestamp)
{
    if (rejectIfNotEnabled(Op::GetQuota, reqId)) {
        return;
    }
    m_rlnModule->get_epoch_quotaAsyncResult(
        registryId, rlnIdentifier, std::to_string(timestamp),
        [reqId](logos::AsyncResult<StdLogosResult> r) {
            respond(reqId, r.ok() ? resultReply(Op::GetQuota, r.value)
                                  : callFail(Op::GetQuota, r.error));
        },
        timeoutMsFor(Op::GetQuota));
}

void RlnBridge::generateProof(uint64_t reqId, std::string registryId,
                              std::string rlnIdentifier, std::string signalHex,
                              uint64_t timestamp)
{
    if (rejectIfNotEnabled(Op::Generate, reqId)) {
        return;
    }
    m_rlnModule->generate_proofAsyncResult(
        registryId, rlnIdentifier, signalHex, std::to_string(timestamp),
        [reqId](logos::AsyncResult<StdLogosResult> r) {
            respond(reqId, r.ok() ? resultReply(Op::Generate, r.value)
                                  : callFail(Op::Generate, r.error));
        },
        timeoutMsFor(Op::Generate));
}

void RlnBridge::validateProof(uint64_t reqId, std::string registryId,
                              std::string rlnIdentifier, std::string signalHex,
                              uint64_t timestamp, std::string proofJson)
{
    if (rejectIfNotEnabled(Op::Validate, reqId)) {
        return;
    }
    m_rlnModule->validate_proofAsyncResult(
        registryId, rlnIdentifier, signalHex, std::to_string(timestamp), proofJson,
        [reqId](logos::AsyncResult<StdLogosResult> r) {
            respond(reqId, r.ok() ? resultReply(Op::Validate, r.value)
                                  : callFail(Op::Validate, r.error));
        },
        timeoutMsFor(Op::Validate));
}
