#include "libp2p_bring_up.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "libp2p_module_api.h"

namespace {

// Deadline for the first libp2p call, which is how an absent module is found.
// The transport's own wait is 20s, as long as the node's budget for `start`,
// which would cancel the verb before the reason arrived.
constexpr int kFirstContactTimeoutMs = 5000;
// How long a caller waits for a bring-up another caller has in flight: under
// the discovery plugin's 15 s call budget, so a `start` that finds the RLN
// path mid bring-up gets an answer rather than a cancelled verb.
constexpr auto kWaitForBringUpInFlight = std::chrono::seconds(10);

bool mentionsTimeout(const std::string& s)
{
    std::string lowered(s);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find("timeout") != std::string::npos;
}

/// Whether a transport error is a deadline expiring rather than a refusal.
/// Matched on text: CallError has no typed code for it.
bool isCallTimeout(const logos::CallError& err)
{
    return mentionsTimeout(err.code) || mentionsTimeout(err.message);
}

} // namespace

const char* const Libp2pBringUp::kUnavailable =
    "libp2p_module is not available (not installed or not loaded)";

Libp2pBringUp::Libp2pBringUp(Libp2pModule* libp2p, std::string config,
                             std::function<void(const std::string&)> log)
    : libp2p_(libp2p)
    , config_(std::move(config))
    , log_(std::move(log))
{
}

bool Libp2pBringUp::ready()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

std::string Libp2pBringUp::ensure()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (inProgress_) {
        settled_.wait_for(lock, kWaitForBringUpInFlight, [this] { return !inProgress_; });
        if (inProgress_) {
            return "libp2p bring-up still in progress";
        }
    }
    if (ready_) {
        return {};
    }
    if (!libp2p_) {
        return "no libp2p_module client";
    }
    inProgress_ = true;
    bool nodeCreated = nodeCreated_;
    bool startIssued = startIssued_;
    lock.unlock();

    const std::string diagnostics = bringUp(nodeCreated, startIssued);

    lock.lock();
    nodeCreated_ = nodeCreated;
    startIssued_ = startIssued;
    ready_ = diagnostics.empty();
    inProgress_ = false;
    settled_.notify_all();
    return diagnostics;
}

std::string Libp2pBringUp::bringUp(bool& nodeCreated_, bool& startIssued_)
{
    std::string diagnostics;
    log_("---- bringing up the libp2p backend ----");

    // No readiness probe: libp2p's `status` is rejected even while the verbs
    // below are served, so it says nothing. The config was validated by
    // whoever supplied it.
    nlohmann::json cfg = nlohmann::json::parse(config_, nullptr, false);
    if (!cfg.is_object()) {
        return "libp2p config is not a JSON object";
    }
    // Every bootstrap peer goes to libp2p. Dialling several can outlast its
    // 10s start deadline; that is a notice below, not a failure.
    size_t bootstrapCount = 0;
    if (const auto nodes = cfg.find("bootstrapNodes");
        nodes != cfg.end() && nodes->is_array()) {
        bootstrapCount = nodes->size();
    }

    if (nodeCreated_) {
        // An earlier attempt built it; only `start` failed. A second
        // createNode would be refused.
        log_("libp2p createNode        ALREADY DONE  bootstrapNodes=" + std::to_string(bootstrapCount));
    } else {
        logos::CallError err;
        const StdLogosResult r = libp2p_->createNode(cfg.dump(), &err, kFirstContactTimeoutMs);
        if (err.code == "object_unavailable") {
            // No module to talk to; the calls below would only time out too.
            log_("libp2p createNode        UNAVAILABLE  " + err.message);
            return kUnavailable;
        }
        log_(std::string("libp2p createNode        ")
             + (!err.ok() ? "TRANSPORT-ERR" : (r.success ? "OK" : "REFUSED"))
             + "  bootstrapNodes=" + std::to_string(bootstrapCount));
        nodeCreated_ = err.ok() && r.success;
        if (!err.ok()) {
            diagnostics += "createNode: " + err.code + ": " + err.message + "; ";
        } else if (!r.success) {
            // libp2p refuses a second createNode once a node exists: this
            // module reloaded, or another consumer brought the node up. A node
            // that answers for itself is that node, created and running; a
            // second `start` on it would crash libp2p_module.
            logos::CallError probe;
            const StdLogosResult info = libp2p_->getNodeInfo("PeerId", &probe);
            if (probe.ok() && info.success) {
                log_("libp2p createNode        EXISTS  the node is already up; taking it as created and started");
                nodeCreated_ = true;
                startIssued_ = true;
            } else {
                // Fatal for discovery: a default libp2p node has no bootstrap peers.
                diagnostics += "createNode: " + (r.error.empty() ? std::string("refused") : r.error) + "; ";
            }
        }
    }

    // libp2p's start() brings up a default node if createNode did not.
    if (startIssued_) {
        // A start that succeeded or is still running must not be issued
        // again: nim-libp2p's double-start guard only trips once a start has
        // finished, so a second one runs a second switch start beside the
        // first -- two accept loops on one listener, and libp2p_module crashes
        // on the next inbound connection.
        log_("libp2p start             ALREADY ISSUED");
    } else {
        logos::CallError err;
        const StdLogosResult r = libp2p_->start(&err);
        // A notice, not a failure: the kademlia bootstrap inside start outlives
        // the call deadline and keeps going. libp2p reports its own 10s deadline
        // in the result ("Failed to start libp2p: timeout"), the transport its
        // deadline as a CallError. If the start really failed, the verbs that
        // follow say so.
        const bool timedOut =
            err.ok() ? (!r.success && mentionsTimeout(r.error)) : isCallTimeout(err);
        if (timedOut) {
            const std::string why = err.ok() ? r.error : err.code + ": " + err.message;
            log_("libp2p start             NOTICE  " + why + " (bring-up continues)");
        } else if (!err.ok()) {
            diagnostics += "start: " + err.code + ": " + err.message + "; ";
        } else if (!r.success) {
            diagnostics += "start: " + (r.error.empty() ? std::string("failed") : r.error) + "; ";
        }
        startIssued_ = timedOut || (err.ok() && r.success);
    }

    // Trace libp2p's identity and listen addresses; nothing else reports them.
    if (diagnostics.empty()) {
        // libp2p's field names are capitalised.
        for (const char* field : {"PeerId", "Multiaddrs"}) {
            logos::CallError err;
            const StdLogosResult r = libp2p_->getNodeInfo(field, &err);
            if (!err.ok() || !r.success) {
                continue;
            }
            const std::string v =
                r.value.is_string() ? r.value.get<std::string>() : r.value.dump();
            log_(std::string("libp2p ") + field + " " + v);
        }
    }

    log_("libp2p backend ready" + (diagnostics.empty() ? std::string() : " with: " + diagnostics));
    return diagnostics;
}
