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
// Request ops (get_membership_state, get_epoch_quota, generate_proof,
// validate_proof) fire a single lp call and return; the reply reaches the
// library from lp's completion callback. Lifecycle (start, stop) is a
// synchronous typed-client call on the caller's thread — the module wants the
// answer in-call. budgetMsFor carries the delivery library's per-op deadline,
// passed through to each lp call.
//
// Threading: init() is the second-phase constructor. It cannot run during
// construction, because the module context is not ready yet. Instead it runs
// lazily on the first enable call — enable is a module method, and methods
// are only dispatched after the context is ready. The thread that runs
// init() becomes the lp client's owner. The request entry points are safe
// from any thread (the delivery library fires its callbacks on foreign
// threads).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

struct lp_client;
struct StdLogosResult;
namespace logos { struct CallError; }
class LiblogosRlnModule; // generated from metadata.json#optional_dependencies

class RlnBridge {
public:
    RlnBridge();
    ~RlnBridge();
    RlnBridge(const RlnBridge&) = delete;
    RlnBridge& operator=(const RlnBridge&) = delete;

    // Second-phase constructor: stores the typed client and creates the lp
    // client. Safe to call more than once. `typed` is borrowed from
    // modules().liblogos_rln_module, which outlives this object.
    void init(LiblogosRlnModule* typed);

    // Enable answering of RLN requests in-process
    // Returns an error string or empty on success
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

    // Per-call context for the async lp reply path. Owned by onLpReply, or by
    // the sender when lp_invoke_async never accepts the call. Holds no pointer
    // back to the bridge: a reply landing during shutdown must not touch
    // bridge state, and lp_client_destroy waits for running callbacks — a
    // callback taking a bridge lock could deadlock against the destructor.
    struct Pending {
        uint64_t reqId = 0;
        Op op = Op::Start;
    };
    // lp completion callback: reshape the reply, respond, delete the Pending.
    static void onLpReply(int ok, const char* jsonText, void* userData);
    // Fires one lp call and returns; onLpReply answers the reqId when the
    // reply lands. Failure to even send is answered immediately with a
    // transport failure, from the calling thread.
    void sendAsync(Op op, uint64_t reqId, const std::string& method,
                   const std::string& argsJson, int timeoutMs);
    // Single reshape point for module replies: transport-failure shaping,
    // tstr string unwrap, dispatch-refusal detection.
    static std::string reshapeReply(Op op, bool ok, const char* jsonText);

    static bool isTstrOp(Op op);
    // The delivery library's per-op response deadline, passed through to lp.
    static int budgetMsFor(Op op);
    static const char* opName(Op op);
    // The only reply this bridge ever fabricates: a transport failure in the
    // error shape of the op's own method family (docs/pages/rln.md).
    static std::string transportFail(Op op, const std::string& cls,
                                     const std::string& kind, const std::string& msg);

    // Reduces a typed-client lifecycle result to this bridge's convention:
    // empty on success, error text otherwise.
    static std::string lifecycleResult(Op op, const StdLogosResult& r,
                                       const logos::CallError& err);

    std::atomic<bool> m_enabled{false};

    lp_client* m_client = nullptr; // created in init() (context thread)
    LiblogosRlnModule* m_typed = nullptr; // set in init()
};
