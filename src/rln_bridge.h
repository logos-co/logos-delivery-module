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
// Two worker lanes, so a slow registry operation never delays proof
// validation on the message hot path:
//   slow lane — get_membership_state, generate_proof:
//     raw lp calls with explicit timeouts, because the generated typed client
//     has no per-call timeout and these ops can legitimately take minutes.
//   fast lane — start, stop, get_epoch_quota, validate_proof: the generated
//     typed client. These answer in milliseconds; the delivery library's own
//     10 s budget for them expires before the client's default would.
//
// Lanes are bounded and deadline-aware: a full lane sheds new work with an
// immediate transient failure (start/stop always accepted), and a job whose
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
// its callbacks on foreign threads).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
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

    // Backend lifecycle, driven by this module — the delivery library neither
    // starts nor stops the RLN module. Both run on the caller's thread and
    // return the module's reply text (empty on success, error text otherwise).
    std::string startBackend(std::string configJson);
    std::string stopBackend();

    // Op entry points (any thread; copy + enqueue, return immediately). The
    // registry and identifier are this module's own configuration: they do not
    // come from the delivery library, which is agnostic of them.
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

    struct Job {
        uint64_t reqId = 0;
        Op op = Op::Start;
        std::string configJson;
        std::string registryId;
        std::string rlnIdentifier;
        std::string signalHex;
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
    // error shape of the op's own method family (docs/pages/rln.md).
    static std::string transportFail(Op op, const std::string& cls,
                                     const std::string& kind, const std::string& msg);

    void enqueue(Job job);
    // Runs a lifecycle op inline on the calling thread (see the .cpp).
    std::string runLifecycle(Op op, std::string configJson);
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
