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

#include <liblogosdelivery.hpp>

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

/// libp2p's lookup records as the node's typed reply. Records without a string
/// peer id are skipped, as the node would drop them anyway; a service's base64
/// `data` is decoded to its bytes.
SdPeerRecords toPeerRecords(const nlohmann::json& records)
{
    SdPeerRecords out;
    if (!records.is_array()) {
        return out;
    }
    for (const auto& rec : records) {
        if (!rec.is_object() || !rec.contains("peerId") || !rec["peerId"].is_string()) {
            continue;
        }
        SdPeerRecord p{};
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
                SdService s;
                s.id = svc["id"].get<std::string>();
                if (svc.contains("data") && svc["data"].is_string()) {
                    s.data = delivery_base64::decode(svc["data"].get<std::string>());
                }
                p.services.push_back(std::move(s));
            }
        }
        out.peers.push_back(std::move(p));
    }
    return out;
}

/// The reply of a verb whose success carries nothing.
template <typename Token>
std::function<void(const Outcome&)> voidReply(Token token)
{
    return [token](const Outcome& o) {
        if (o.ok) {
            token.reply();
        } else {
            token.fail(o.error);
        }
    };
}

/// The reply of a lookup: the records, typed.
template <typename Token>
std::function<void(const Outcome&)> recordsReply(Token token)
{
    return [token](const Outcome& o) {
        if (o.ok) {
            token.reply(toPeerRecords(o.records));
        } else {
            token.fail(o.error);
        }
    };
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

std::string DeliveryServiceDiscoveryPlugin::registerOn(LogosDeliveryCtx& node)
{
    if (!executor_.joinable()) {
        executor_ = std::thread([this] { runExecutor(); });
    }
    // On a nim-ffi reverse worker: queue and return. The arguments are the
    // binding's decoded copies, so the queued call owns them.
    const auto queue = [this](auto token, std::function<Outcome()> run, auto reply) {
        if (!enqueue(Call{std::move(run), reply(token)})) {
            token.fail("service discovery host is shutting down");
        }
    };
    using L = LogosDeliveryCtx;
    const bool ok =
        node.setSdStartImpl([this, queue](L::SdStartCall c) {
            queue(c, [this] { return start(); }, voidReply<L::SdStartCall>);
        }) &&
        node.setSdStopImpl([this, queue](L::SdStopCall c) {
            queue(c, [this] { return stop(); }, voidReply<L::SdStopCall>);
        }) &&
        node.setSdLookupImpl([this, queue](L::SdLookupCall c, const SdLookupArgs& a) {
            queue(c, [this, a] { return lookup(a.key, a.limit); }, recordsReply<L::SdLookupCall>);
        }) &&
        node.setSdRandomLookupImpl([this, queue](L::SdRandomLookupCall c) {
            queue(c, [this] { return randomLookup(); }, recordsReply<L::SdRandomLookupCall>);
        }) &&
        node.setSdStartAdvertisingImpl([this, queue](L::SdStartAdvertisingCall c,
                                           const SdStartAdvertisingArgs& a) {
            queue(c, [this, a] { return startAdvertising(a.key, a.data, a.record); },
                  voidReply<L::SdStartAdvertisingCall>);
        }) &&
        node.setSdStopAdvertisingImpl([this, queue](L::SdStopAdvertisingCall c, const std::string& key) {
            queue(c, [this, key] { return stopAdvertising(key); },
                  voidReply<L::SdStopAdvertisingCall>);
        }) &&
        node.setSdRegisterInterestImpl([this, queue](L::SdRegisterInterestCall c, const std::string& key) {
            queue(c, [this, key] { return registerInterest(key); },
                  voidReply<L::SdRegisterInterestCall>);
        }) &&
        node.setSdUnregisterInterestImpl([this, queue](L::SdUnregisterInterestCall c,
                                             const std::string& key) {
            queue(c, [this, key] { return unregisterInterest(key); },
                  voidReply<L::SdUnregisterInterestCall>);
        });
    if (!ok) {
        return "registering the sd_* reverse implementations was refused";
    }
    trace("reverse impls            REGISTERED  8 wires");
    return {};
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
        call.reply(call.run());
    }
    // Whatever is still queued is answered, so the node is not left waiting
    // for its deadline. After ctx_destroy the replies are refused, harmlessly.
    std::deque<Call> rest;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rest.swap(queue_);
    }
    for (const auto& call : rest) {
        call.reply(failed("service discovery host is shutting down"));
    }
    executorExited_.store(true);
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
