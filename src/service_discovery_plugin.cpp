#include "service_discovery_plugin.h"

#include <cstdarg>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <thread>

extern "C" {
#include <liblogosdelivery.h>
}

// Generated at build time from metadata.json#optional_dependencies.
#include "libp2p_module_api.h"
#include "base64.h"

namespace {

// Deadline for the first libp2p call, which is how an absent module is found.
// The transport's own wait is 20s, as long as the node's budget for `start`,
// which would cancel the verb before the reason arrived.
constexpr int kFirstContactTimeoutMs = 5000;

// Reaches the caller as the reason start() failed, so it says what to do.
constexpr const char* kLibp2pUnavailable =
    "libp2p_module is not available (not installed or not loaded), but this "
    "node is configured for external service discovery, which it hosts. "
    "Install and load libp2p_module, or configure internal discovery instead "
    "(e.g. discv5, without plugin-kad-discovery); on Windows internal "
    "discovery is the only option";

// The node's reverse-call wire names, in Wire order; also the names its
// get_discovery_requirements lists under requiredHostImpls.
constexpr const char* kWires[] = {
    "sd_start",
    "sd_stop",
    "sd_lookup",
    "sd_random_lookup",
    "sd_start_advertising",
    "sd_stop_advertising",
    "sd_register_interest",
    "sd_unregister_interest",
};

// Opt-in trace of the plugin boundary to the file named by LD_DISCO_TRACE:
// logos-core discards a module's own stdout/stderr, so there is no other view.
FILE* traceFile()
{
    static FILE* f = [] () -> FILE* {
        const char* path = getenv("LD_DISCO_TRACE");
        if (!path || !*path) {
            return nullptr;
        }
        FILE* h = fopen(path, "a");
        if (h) {
            setvbuf(h, nullptr, _IOLBF, 0); // line-buffered, so `tail -f` works
        }
        return h;
    }();
    return f;
}

void trace(const char* fmt, ...)
{
    FILE* f = traceFile();
    if (!f) {
        return;
    }
    char stamp[32] = "";
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    // MinGW declares localtime_r only under _POSIX_C_SOURCE; localtime_s is
    // the same call with the arguments swapped, returning 0 on success.
    if (localtime_s(&tm, &now) == 0) {
#else
    if (localtime_r(&now, &tm)) {
#endif
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    }
    fprintf(f, "[%s] ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
}

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

using Outcome = DeliveryServiceDiscoveryPlugin::Outcome;

Outcome failed(std::string error)
{
    Outcome out;
    out.ok = false;
    out.error = std::move(error);
    return out;
}

/// Turns one typed reply into an Outcome. Transport failures (CallError) and
/// libp2p's own refusals are reported apart.
Outcome settle(const char* method, const StdLogosResult& r, const logos::CallError& err)
{
    if (!err.ok()) {
        const std::string msg =
            std::string(method) + ": " + err.code + ": " + err.message;
        trace("%-22s TRANSPORT-ERR  %s", method, msg.c_str());
        return failed(msg);
    }
    if (!r.success) {
        const std::string msg =
            r.error.empty() ? std::string(method) + " failed" : r.error;
        trace("%-22s REFUSED        %s", method, msg.c_str());
        return failed(msg);
    }
    return {};
}

/// Peer ids of a lookup's records, shortened, for the trace.
std::string tracePeers(const nlohmann::json& records)
{
    std::string peers;
    if (records.is_array()) {
        for (const auto& rec : records) {
            if (!rec.is_object() || !rec.contains("peerId") || !rec["peerId"].is_string()) continue;
            const std::string id = rec["peerId"].get<std::string>();
            if (!peers.empty()) peers += ",";
            peers += id.size() > 12 ? id.substr(id.size() - 8) : id;
        }
    }
    return peers;
}

/// Runs one verb. The executor is the only caller and an escaping exception
/// would take its thread down, so report it as the verb's failure instead.
template <typename Body>
Outcome guarded(const char* verb, Body&& body)
{
    // The message is copied inside the handler: `what()` dies with the
    // exception object when the handler ends.
    std::string what = "unknown exception";
    try {
        return body();
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {
    }
    trace("%-22s EXCEPTION      %s", verb, what.c_str());
    return failed(std::string(verb) + ": " + what);
}

/// Owns the C view of a lookup's records for one typed reply: the generated
/// SdPeerRecords points into the strings and vectors kept here.
class PeerRecordsView
{
public:
    /// Records without a string peer id are skipped, as the node would drop
    /// them anyway; a service's base64 `data` is decoded to its bytes.
    explicit PeerRecordsView(const nlohmann::json& records)
    {
        if (!records.is_array()) {
            return;
        }
        for (const auto& rec : records) {
            if (!rec.is_object() || !rec.contains("peerId") || !rec["peerId"].is_string()) {
                continue;
            }
            Peer p;
            p.peerId = rec["peerId"].get<std::string>();
            if (const auto s = rec.find("seqNo"); s != rec.end() && s->is_number_integer() &&
                                                  s->get<int64_t>() >= 0) {
                p.seqNo = s->get<uint64_t>();
            }
            if (const auto a = rec.find("addrs"); a != rec.end() && a->is_array()) {
                for (const auto& addr : *a) {
                    if (addr.is_string()) p.addrs.push_back(addr.get<std::string>());
                }
            }
            if (const auto svcs = rec.find("services"); svcs != rec.end() && svcs->is_array()) {
                for (const auto& svc : *svcs) {
                    if (!svc.is_object() || !svc.contains("id") || !svc["id"].is_string()) continue;
                    Service s;
                    s.id = svc["id"].get<std::string>();
                    if (svc.contains("data") && svc["data"].is_string()) {
                        s.data = delivery_base64::decode(svc["data"].get<std::string>());
                    }
                    p.services.push_back(std::move(s));
                }
            }
            peers_.push_back(std::move(p));
        }
        // Pointers are taken only now: peers_ no longer moves.
        for (auto& p : peers_) {
            for (const auto& a : p.addrs) p.addrPtrs.push_back(a.c_str());
            for (auto& s : p.services) {
                p.serviceViews.push_back(SdService{
                    s.id.c_str(), NimFfiBytes{s.data.empty() ? nullptr : s.data.data(), s.data.size()}});
            }
            records_.push_back(SdPeerRecord{
                p.peerId.c_str(),
                p.seqNo,
                LogosDeliverySeq_Str{p.addrPtrs.empty() ? nullptr : p.addrPtrs.data(), p.addrPtrs.size()},
                LogosDeliverySeq_SdService{p.serviceViews.empty() ? nullptr : p.serviceViews.data(),
                                           p.serviceViews.size()},
            });
        }
        view_.peers = LogosDeliverySeq_SdPeerRecord{records_.empty() ? nullptr : records_.data(),
                                                    records_.size()};
    }

    const SdPeerRecords* get() const { return &view_; }

private:
    struct Service {
        std::string id;
        std::vector<uint8_t> data;
    };
    struct Peer {
        std::string peerId;
        uint64_t seqNo = 0;
        std::vector<std::string> addrs;
        std::vector<Service> services;
        std::vector<const char*> addrPtrs;
        std::vector<SdService> serviceViews;
    };
    std::vector<Peer> peers_;
    std::vector<SdPeerRecord> records_;
    SdPeerRecords view_{};
};

/// Decodes a key-only reverse call's argument; false (and an error reply
/// already sent) when it does not decode.
template <typename Decode>
bool decodeKey(Decode decode, void* ctx, uint64_t callId, const uint8_t* args, size_t len,
               std::string& key)
{
    const char* out = nullptr;
    char* err = nullptr;
    if (decode(args, len, &out, &err) != 0) {
        logosdelivery_reverse_reply_err(ctx, callId, err ? err : "args decode failed");
        free(err);
        return false;
    }
    key = out ? out : "";
    free(const_cast<char*>(out));
    return true;
}

} // namespace

DeliveryServiceDiscoveryPlugin::DeliveryServiceDiscoveryPlugin(Libp2pModule* libp2p,
                                                               std::string libp2pConfig)
    : libp2p_(libp2p)
    , libp2pConfig_(std::move(libp2pConfig))
    , backendReady_(false)
    , nodeCreated_(false)
    , startIssued_(false)
{
    for (int w = 0; w < WireCount; ++w) {
        bindings_[w] = Binding{this, static_cast<Wire>(w)};
    }
}

DeliveryServiceDiscoveryPlugin::~DeliveryServiceDiscoveryPlugin()
{
    // The owner quiesces first and leaks the object if that times out, so the
    // executor is gone or never ran by now.
    if (executor_.joinable()) {
        quiesce(std::chrono::milliseconds(0));
        if (executor_.joinable()) {
            executor_.detach();
        }
    }
}

std::string DeliveryServiceDiscoveryPlugin::toServiceId(const std::string& key)
{
    constexpr const char* kServicePrefix = "service:";
    constexpr size_t kServicePrefixLen = 8;
    if (key.rfind(kServicePrefix, 0) == 0) {
        return key.substr(kServicePrefixLen);
    }
    return key;
}

// --- reverse-call plumbing ---------------------------------------------------

std::string DeliveryServiceDiscoveryPlugin::registerOn(void* ctx)
{
    if (!executor_.joinable()) {
        executor_ = std::thread([this] { runExecutor(); });
    }
    using SetImpl = int (*)(void*, FFIReverseImpl, void*, FFIReverseRelease);
    const SetImpl setters[WireCount] = {
        logosdelivery_set_sd_start_impl,
        logosdelivery_set_sd_stop_impl,
        logosdelivery_set_sd_lookup_impl,
        logosdelivery_set_sd_random_lookup_impl,
        logosdelivery_set_sd_start_advertising_impl,
        logosdelivery_set_sd_stop_advertising_impl,
        logosdelivery_set_sd_register_interest_impl,
        logosdelivery_set_sd_unregister_interest_impl,
    };
    for (int w = 0; w < WireCount; ++w) {
        // No release: this object owns its bindings and outlives the context.
        const int rc = setters[w](ctx, &DeliveryServiceDiscoveryPlugin::cImpl, &bindings_[w], nullptr);
        if (rc != NIMFFI_REVERSE_ACCEPTED) {
            return std::string("registering ") + kWires[w] + " was refused (" + std::to_string(rc) + ")";
        }
    }
    trace("reverse impls            REGISTERED  %d wires", static_cast<int>(WireCount));
    return {};
}

void DeliveryServiceDiscoveryPlugin::cImpl(void* ctx, uint64_t callId, const uint8_t* args,
                                           size_t argsLen, void* userData)
{
    // On a nim-ffi reverse worker: decode, queue, return. `args` is the
    // library's until this returns, so the call keeps its own copy.
    const Binding* b = static_cast<const Binding*>(userData);
    Call call{b->wire, ctx, callId, {}, 0, {}, {}};
    switch (b->wire) {
    case SdLookup: {
        SdLookupArgs a{};
        char* err = nullptr;
        if (logosdelivery_decode_sd_lookup_args(args, argsLen, &a, &err) != 0) {
            logosdelivery_reverse_reply_err(ctx, callId, err ? err : "args decode failed");
            free(err);
            return;
        }
        call.key = a.key ? a.key : "";
        call.limit = a.limit;
        logosdelivery_free_SdLookupArgs(&a);
        break;
    }
    case SdStartAdvertising: {
        SdStartAdvertisingArgs a{};
        char* err = nullptr;
        if (logosdelivery_decode_sd_start_advertising_args(args, argsLen, &a, &err) != 0) {
            logosdelivery_reverse_reply_err(ctx, callId, err ? err : "args decode failed");
            free(err);
            return;
        }
        call.key = a.key ? a.key : "";
        call.data.assign(a.data.data, a.data.data + a.data.len);
        call.record.assign(a.record.data, a.record.data + a.record.len);
        logosdelivery_free_SdStartAdvertisingArgs(&a);
        break;
    }
    case SdStopAdvertising:
        if (!decodeKey(logosdelivery_decode_sd_stop_advertising_args, ctx, callId, args, argsLen, call.key)) return;
        break;
    case SdRegisterInterest:
        if (!decodeKey(logosdelivery_decode_sd_register_interest_args, ctx, callId, args, argsLen, call.key)) return;
        break;
    case SdUnregisterInterest:
        if (!decodeKey(logosdelivery_decode_sd_unregister_interest_args, ctx, callId, args, argsLen, call.key)) return;
        break;
    default: // sd_start, sd_stop, sd_random_lookup take no arguments
        break;
    }
    if (!b->self->enqueue(std::move(call))) {
        logosdelivery_reverse_reply_err(ctx, callId, "service discovery host is shutting down");
    }
}

bool DeliveryServiceDiscoveryPlugin::enqueue(Call call)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        queue_.push_back(std::move(call));
    }
    wake_.notify_one();
    return true;
}

void DeliveryServiceDiscoveryPlugin::runExecutor()
{
    for (;;) {
        Call call;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                break;
            }
            call = std::move(queue_.front());
            queue_.pop_front();
        }
        answer(call, serve(call));
    }
    // Whatever is still queued is answered, so the node is not left waiting
    // for its deadline. After ctx_destroy the replies are refused, harmlessly.
    std::deque<Call> rest;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rest.swap(queue_);
    }
    for (const auto& call : rest) {
        logosdelivery_reverse_reply_err(call.ctx, call.id, "service discovery host is shutting down");
    }
    executorExited_.store(true);
}

DeliveryServiceDiscoveryPlugin::Outcome DeliveryServiceDiscoveryPlugin::serve(const Call& call)
{
    switch (call.wire) {
    case SdStart:              return start();
    case SdStop:               return stop();
    case SdLookup:             return lookup(call.key, call.limit);
    case SdRandomLookup:       return randomLookup();
    case SdStartAdvertising:   return startAdvertising(call.key, call.data, call.record);
    case SdStopAdvertising:    return stopAdvertising(call.key);
    case SdRegisterInterest:   return registerInterest(call.key);
    case SdUnregisterInterest: return unregisterInterest(call.key);
    default:                   return failed("unknown reverse call");
    }
}

void DeliveryServiceDiscoveryPlugin::answer(const Call& call, const Outcome& outcome)
{
    if (!outcome.ok) {
        logosdelivery_reverse_reply_err(call.ctx, call.id, outcome.error.c_str());
        return;
    }
    switch (call.wire) {
    case SdStart:              logosdelivery_reverse_reply_sd_start(call.ctx, call.id); break;
    case SdStop:               logosdelivery_reverse_reply_sd_stop(call.ctx, call.id); break;
    case SdLookup: {
        const PeerRecordsView view(outcome.records);
        logosdelivery_reverse_reply_sd_lookup(call.ctx, call.id, view.get());
        break;
    }
    case SdRandomLookup: {
        const PeerRecordsView view(outcome.records);
        logosdelivery_reverse_reply_sd_random_lookup(call.ctx, call.id, view.get());
        break;
    }
    case SdStartAdvertising:   logosdelivery_reverse_reply_sd_start_advertising(call.ctx, call.id); break;
    case SdStopAdvertising:    logosdelivery_reverse_reply_sd_stop_advertising(call.ctx, call.id); break;
    case SdRegisterInterest:   logosdelivery_reverse_reply_sd_register_interest(call.ctx, call.id); break;
    case SdUnregisterInterest: logosdelivery_reverse_reply_sd_unregister_interest(call.ctx, call.id); break;
    default: break;
    }
}

bool DeliveryServiceDiscoveryPlugin::quiesce(std::chrono::milliseconds timeout)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (!executor_.joinable()) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!executorExited_.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            trace("quiesce                  TIMEOUT  a verb is still inside libp2p");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    executor_.join();
    return true;
}

// --- libp2p backend ----------------------------------------------------------

bool DeliveryServiceDiscoveryPlugin::requireBackend(Outcome& out)
{
    const std::string failure = ensureBackend();
    if (failure.empty()) {
        return true;
    }
    trace("libp2p backend           UNAVAILABLE  %s", failure.c_str());
    out = failed("libp2p backend unavailable: " + failure);
    return false;
}

std::string DeliveryServiceDiscoveryPlugin::ensureBackend()
{
    if (backendReady_) {
        return {};
    }
    if (!libp2p_) {
        return "no libp2p_module client";
    }

    std::string diagnostics;
    trace("---- bringing up the libp2p backend ----");

    // No readiness probe: libp2p's `status` is rejected even while the verbs
    // below are served, so it says nothing. The config was validated at
    // createNode (discovery_config.h).
    nlohmann::json cfg = nlohmann::json::parse(libp2pConfig_, nullptr, false);
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
        trace("libp2p createNode        ALREADY DONE  bootstrapNodes=%zu", bootstrapCount);
    } else {
        logos::CallError err;
        const StdLogosResult r = libp2p_->createNode(cfg.dump(), &err, kFirstContactTimeoutMs);
        if (err.code == "object_unavailable") {
            // No module to talk to; the calls below would only time out too.
            trace("libp2p createNode        UNAVAILABLE  %s", err.message.c_str());
            return kLibp2pUnavailable;
        }
        trace("libp2p createNode        %s  bootstrapNodes=%zu",
              (!err.ok() ? "TRANSPORT-ERR" : (r.success ? "OK" : "REFUSED")),
              bootstrapCount);
        nodeCreated_ = err.ok() && r.success;
        if (!err.ok()) {
            diagnostics += "createNode: " + err.code + ": " + err.message + "; ";
        } else if (!r.success) {
            // Fatal: a default libp2p node has no bootstrap peers.
            diagnostics += "createNode: " + (r.error.empty() ? std::string("refused") : r.error) + "; ";
        }
    }

    // libp2p's start() brings up a default node if createNode did not.
    if (startIssued_) {
        // A start that succeeded or is still running must not be issued
        // again: nim-libp2p's double-start guard only trips once a start has
        // finished, so a second one runs a second switch start beside the
        // first -- two accept loops on one listener, and libp2p_module crashes
        // on the next inbound connection.
        trace("libp2p start             ALREADY ISSUED");
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
            trace("libp2p start             NOTICE  %s (bring-up continues)", why.c_str());
        } else if (!err.ok()) {
            diagnostics += "start: " + err.code + ": " + err.message + "; ";
        } else if (!r.success) {
            diagnostics += "start: " + (r.error.empty() ? std::string("failed") : r.error) + "; ";
        }
        startIssued_ = timedOut || (err.ok() && r.success);
    }

    // No discoStart: the switch start already started service discovery.

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
            trace("libp2p %-18s %s", field, v.c_str());
        }
    }

    trace("libp2p backend ready%s%s", diagnostics.empty() ? "" : " with: ",
          diagnostics.c_str());
    backendReady_ = diagnostics.empty();
    return diagnostics;
}

// --- the verbs ---------------------------------------------------------------

Outcome DeliveryServiceDiscoveryPlugin::start()
{
    return guarded("start", [&]() -> Outcome {
        // Only brings libp2p up. Its discovery is shared by the whole process and
        // outlives this node, so the node's start/stop does not drive it.
        Outcome out;
        if (!requireBackend(out)) {
            return out;
        }
        trace("%-22s OK (backend already running)", "start");
        return out;
    });
}

Outcome DeliveryServiceDiscoveryPlugin::stop()
{
    return guarded("stop", [&]() -> Outcome {
        // No-op: libp2p's discovery is shared (see start).
        trace("%-22s OK (no-op; libp2p discovery is shared)", "stop");
        return {};
    });
}

Outcome DeliveryServiceDiscoveryPlugin::lookup(const std::string& key, int64_t limit)
{
    return guarded("lookup", [&]() -> Outcome {
        Outcome out;
        if (!requireBackend(out)) {
            return out;
        }
        // libp2p's discoLookup takes (serviceId, serviceData) and has no result
        // cap, so `limit` has nowhere to go; the caller trims what it gets back.
        (void)limit;
        logos::CallError err;
        const StdLogosResult r = libp2p_->discoLookup(toServiceId(key), std::string(), &err);
        out = settle("discoLookup", r, err);
        if (!out.ok) {
            return out;
        }
        // Peer ids, so a peer can be told from our own provider record.
        trace("%-22s OK             key=%s records=%zu peers=[%s]", "discoLookup",
              toServiceId(key).c_str(), r.value.is_array() ? r.value.size() : 0,
              tracePeers(r.value).c_str());
        out.records = r.value.is_array() ? r.value : nlohmann::json::array();
        return out;
    });
}

Outcome DeliveryServiceDiscoveryPlugin::randomLookup()
{
    return guarded("randomLookup", [&]() -> Outcome {
        Outcome out;
        if (!requireBackend(out)) {
            return out;
        }
        logos::CallError err;
        const StdLogosResult r = libp2p_->discoRandomLookup(&err);
        out = settle("discoRandomLookup", r, err);
        if (!out.ok) {
            return out;
        }
        // Peer ids, to tell random-walk results from service lookups.
        trace("%-22s OK             records=%zu peers=[%s]", "discoRandomLookup",
              r.value.is_array() ? r.value.size() : 0, tracePeers(r.value).c_str());
        out.records = r.value.is_array() ? r.value : nlohmann::json::array();
        return out;
    });
}

Outcome DeliveryServiceDiscoveryPlugin::startAdvertising(const std::string& key,
                                                         const std::vector<uint8_t>& data,
                                                         const std::vector<uint8_t>& record)
{
    return guarded("startAdvertising", [&]() -> Outcome {
        Outcome out;
        if (!requireBackend(out)) {
            return out;
        }
        // `record` is the node's signed peer record (protobuf), base64-encoded for
        // libp2p's JSON transport. With a record, `data` is redundant but libp2p
        // rejects an empty serviceData and it may not be JSON-safe, so a marker
        // goes instead. Either may be empty.
        const bool hasRecord = !record.empty();
        const std::string advertisement =
            hasRecord ? delivery_base64::encode(record) : std::string();
        const std::string serviceData =
            hasRecord ? std::string("xpr")
                      : std::string(data.begin(), data.end());
        logos::CallError err;
        const StdLogosResult r = libp2p_->discoStartAdvertising(
            toServiceId(key), serviceData, advertisement, &err);
        // recordLen is the raw size, advertLen the base64 one sent.
        trace("%-22s ->  key=%s dataLen=%zu recordLen=%zu advertLen=%zu",
              "discoStartAdvertising", toServiceId(key).c_str(), serviceData.size(),
              record.size(), advertisement.size());
        out = settle("discoStartAdvertising", r, err);
        if (out.ok)
            trace("%-22s OK             key=%s data=%s", "discoStartAdvertising",
                  toServiceId(key).c_str(), serviceData.c_str());
        return out;
    });
}

Outcome DeliveryServiceDiscoveryPlugin::stopAdvertising(const std::string& key)
{
    return guarded("stopAdvertising", [&]() -> Outcome {
        Outcome out;
        if (!requireBackend(out)) {
            return out;
        }
        logos::CallError err;
        const StdLogosResult r = libp2p_->discoStopAdvertising(toServiceId(key), &err);
        out = settle("discoStopAdvertising", r, err);
        if (out.ok)
            trace("%-22s OK             key=%s", "discoStopAdvertising", toServiceId(key).c_str());
        return out;
    });
}

Outcome DeliveryServiceDiscoveryPlugin::registerInterest(const std::string& key)
{
    return guarded("registerInterest", [&]() -> Outcome {
        Outcome out;
        if (!requireBackend(out)) {
            return out;
        }
        logos::CallError err;
        const StdLogosResult r = libp2p_->discoRegisterInterest(toServiceId(key), &err);
        out = settle("discoRegisterInterest", r, err);
        if (out.ok)
            trace("%-22s OK             key=%s", "discoRegisterInterest", toServiceId(key).c_str());
        return out;
    });
}

Outcome DeliveryServiceDiscoveryPlugin::unregisterInterest(const std::string& key)
{
    return guarded("unregisterInterest", [&]() -> Outcome {
        Outcome out;
        if (!requireBackend(out)) {
            return out;
        }
        logos::CallError err;
        const StdLogosResult r = libp2p_->discoUnregisterInterest(toServiceId(key), &err);
        out = settle("discoUnregisterInterest", r, err);
        if (out.ok)
            trace("%-22s OK             key=%s", "discoUnregisterInterest", toServiceId(key).c_str());
        return out;
    });
}
