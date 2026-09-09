#pragma once

// In-process RLN responder: serves the delivery library's rln* callbacks by
// calling the co-loaded liblogos_rln_module and feeding each reply into
// logosdelivery_rln_response. createNode enables it whenever the node config
// runs lez RLN ("rln-lez"); the rln*Request events keep emitting
// either way, for observability (docs/rln.md). It also carries the RLN
// module's lifecycle: start()/stop() are module-driven synchronous calls,
// not answers to library requests.
//
// Two worker lanes, so a slow registry operation never delays proof
// validation on the message hot path:
//   slow lane — register_membership, get_membership_state, generate_proof:
//     raw lp calls with explicit timeouts, because the generated typed client
//     has no per-call timeout and these ops can legitimately take minutes.
//   fast lane — get_epoch_quota, validate_proof: the generated typed client.
//     These answer in milliseconds; the delivery library's own 10 s budget
//     for them expires before the client's default would.
//
// Lanes are bounded and deadline-aware: a full lane sheds new work with an
// immediate transient failure, and a job whose
// library budget ran out while queued is answered without serving — so a
// burst degrades into fast failures instead of a queue of expired jobs
// blocking the still-awaited ones.
//
// Threading: init() is the second-phase constructor. It cannot run during
// construction, because the module context is not ready yet. Instead it runs
// lazily on the first enable call — enable is a module method, and methods
// are only dispatched after the context is ready. The thread that runs
// init() becomes the lp client's owner. The op entry points only copy
// arguments and enqueue — safe from any thread (the delivery library fires
// its callbacks on foreign threads). start()/stop() are the exception: they
// call the typed client on the caller's thread and block until it answers.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct lp_client;
class LiblogosRlnModule; // generated from metadata.json#dependencies

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

    // Module-driven lifecycle, synchronous on the caller's thread — the only
    // way the RLN module is started or stopped. Empty string = success.
    std::string start(const std::string& configJson);
    std::string stop();

    // Op entry points (any thread; copy + enqueue, return immediately).
    void registerMembership(uint64_t reqId, std::string registryId,
                            std::string rlnIdentifier, std::string optionsJson);
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
    enum class Op { Register, GetState, GetQuota, Generate, Validate };

    struct Job {
        uint64_t reqId = 0;
        Op op = Op::Register;
        std::string registryId;
        std::string rlnIdentifier;
        std::string signalHex;
        std::string optionsJson;
        std::string proofJson;
        uint64_t timestamp = 0;
        std::chrono::steady_clock::time_point enqueuedAt;
    };

    struct Lane {
        std::deque<Job> queue;
        std::condition_variable cv;
        std::thread worker;
    };

    static bool isSlowOp(Op op);
    static bool isTstrOp(Op op);
    // The delivery library's per-op response budget, running since its
    // callback fired — queue wait spends the same clock the serve does.
    static int budgetMsFor(Op op);
    static const char* opName(Op op);
    // The only reply this bridge ever fabricates: a transport failure in the
    // error shape of the op's own method family (docs/rln.md).
    static std::string transportFail(Op op, const std::string& cls,
                                     const std::string& kind, const std::string& msg);

    void enqueue(Job job);
    void laneLoop(Lane* lane);
    std::string serveOp(const Job& job);
    std::string serveFast(const Job& job);
    // One raw lp round-trip; out = the module's reply text. false = transport
    // failure.
    bool invokeRaw(const std::string& method, const std::string& argsJson,
                   int timeoutMs, std::string& out, std::string& errMsg);

    std::mutex m_lock;
    Lane m_slow;
    Lane m_fast;
    bool m_stopping = false;
    bool m_lanesRunning = false;
    std::atomic<bool> m_enabled{false};

    lp_client* m_client = nullptr; // created in init() (context thread)
    LiblogosRlnModule* m_typed = nullptr; // set in init()
};
