#pragma once

// In-process RLN responder: serves the delivery library's rln* callbacks by
// calling the co-loaded liblogos_rln_module and feeding each reply into
// logosdelivery_rln_response. createNode enables it whenever the node config
// runs lez RLN ("rln-lez"); the rln*Request events keep emitting
// either way, for observability (docs/pages/rln.md).
//
// The delivery library's RLN plugin is implementation-agnostic: it never names
// a registry or a membership and never starts the backend. This module holds
// that knowledge — it starts and stops liblogos_rln_module itself and adds the
// registry id and rln identifier to every call it forwards.
//
// EVERY call rides the generated typed client. Request ops
// (get_membership_state, get_epoch_quota, generate_proof, validate_proof) use
// its `<name>AsyncResult` twins: one call that returns immediately, with the
// reply arriving on the client's completion callback and carrying the
// CallError that says whether there is a reply at all. Lifecycle (start, stop)
// uses the synchronous twins on the caller's thread — the module wants the
// answer in-call. Both pass an explicit per-call deadline (timeoutMsFor).
//
// There is no raw lp_* call here, and no client of our own: the typed client
// belongs to modules() and already owns an lp client, creates it lazily, does
// the capability flow, maps transport codes to CallError and frees the reply
// strings. Hand-rolling that was worth it only while the generated client had
// no per-call timeout, which is no longer true.
//
// Threading: init() is the second-phase constructor. It cannot run during
// construction, because the module context is not ready yet. Instead it runs
// lazily on the first enable call — enable is a module method, and methods are
// only dispatched after the context is ready. The op entry points are safe
// from any thread (the delivery library fires its callbacks on foreign
// threads).
//
// Completion callbacks run on the typed client's own thread, NOT the caller's
// — except a call lp declines to send, which answers inline before the
// dispatch returns. Either way the callback fires exactly once. Every callback
// captures reqId BY VALUE and calls only static helpers, so it holds no
// pointer back to this object: the typed client outlives the bridge, so a
// reply CAN land after teardown, and it must not touch bridge state when it
// does.

#include <atomic>
#include <functional>
#include <cstdint>
#include <string>

struct StdLogosResult;
namespace logos { struct CallError; }
class LiblogosRlnModule; // generated from metadata.json#optional_dependencies

class RlnBridge {
public:
    RlnBridge();
    ~RlnBridge();
    RlnBridge(const RlnBridge&) = delete;
    RlnBridge& operator=(const RlnBridge&) = delete;

    // Second-phase constructor: stores the typed client. Safe to call more
    // than once. `rlnModule` is borrowed from modules().liblogos_rln_module,
    // which outlives this object.
    void init(LiblogosRlnModule* rlnModule);

    // Enable answering of RLN requests in-process.
    // Returns an error string or empty on success.
    std::string enable();
    bool enabled() const { return m_enabled.load(std::memory_order_acquire); }

    // Backend lifecycle, driven by this module — the delivery library neither
    // starts nor stops the RLN module. Both run on the caller's thread and
    // return the module's reply text (empty on success, error text otherwise).
    std::string startBackend(std::string configJson);
    std::string stopBackend();

    // Op entry points (any thread; return immediately — the reply reaches the
    // library later via logosdelivery_rln_response). The registry and
    // identifier are this module's own configuration: they do not come from
    // the delivery library, which is agnostic of them.
    void getMembershipState(uint64_t reqId, std::string registryId,
                            std::string rlnIdentifier);
    void getEpochQuota(uint64_t reqId, std::string registryId,
                       std::string rlnIdentifier, uint64_t timestamp);
    void generateProof(uint64_t reqId, std::string registryId,
                       std::string rlnIdentifier, std::string signalHex,
                       uint64_t timestamp);
    void validateProof(uint64_t reqId, std::string registryId,
                       std::string rlnIdentifier, std::string signalHex,
                       uint64_t timestamp, std::string proofJson);

private:
    enum class Op { Start, Stop, GetState, GetQuota, Generate, Validate };

    // get_membership_state is the module's one tstr method; the other five
    // answer the result envelope. The two dialects fail differently, which is
    // the only reason the bridge tracks which op it is serving.
    static bool isTstrOp(Op op);
    // The deadline each call carries, mirroring the delivery library's own
    // per-op budget where it has one.
    static int timeoutMsFor(Op op);
    static const char* opName(Op op);

    // The only replies this bridge fabricates: a failure in the error shape of
    // the op's own method family (docs/pages/rln.md).
    static std::string transportFail(Op op, const std::string& cls,
                                     const std::string& kind, const std::string& msg);
    // A call that never produced the module's answer, classified from the
    // CallError alone.
    static std::string callFail(Op op, const logos::CallError& err);
    // The module's answer to a result-dialect call, rebuilt into the library's
    // envelope. Only valid once the call is known to have succeeded.
    static std::string resultReply(Op op, const StdLogosResult& r);
    // The module's answer to a tstr call: its compact reply, forwarded whole.
    static std::string tstrReply(Op op, const std::string& value);

    // The single place a reply reaches the library. Static so a completion
    // callback needs no bridge pointer.
    // How an answer reaches the library: the plugin installs the pump's
    // reverse reply. Called from whatever thread the RLN module's completion
    // arrives on; nim-ffi takes it from any thread.
public:
    using Responder = std::function<void(uint64_t reqId, const std::string& out)>;
    static void setResponder(Responder responder);

private:
    static void respond(uint64_t reqId, const std::string& out);

    // First line of every op entry point: a bridge that is not serving still
    // owes the library an answer for this reqId. Returns true when it answered
    // and the caller must stop.
    bool rejectIfNotEnabled(Op op, uint64_t reqId) const;

    // Reduces a lifecycle call to this bridge's convention: empty on success,
    // error text otherwise.
    static std::string lifecycleResult(Op op, const StdLogosResult& r,
                                       const logos::CallError& err);

    std::atomic<bool> m_enabled{false};
    // Set in init(); owned by modules(). enable() refuses without it, so
    // m_enabled implies a client and the op paths test only m_enabled.
    LiblogosRlnModule* m_rlnModule = nullptr;
};
