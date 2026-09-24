#include "service_discovery_plugin.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <thread>

#include <nlohmann/json.hpp>

// Generated at build time from metadata.json#optional_dependencies.
#include "libp2p_module_api.h"
#include "base64.h"

namespace {

// Bootstrap peers handed to libp2p's createNode. libp2p's start dials them
// within its fixed 10s call budget, and two DNS-resolved peers exceed it.
constexpr size_t kMaxBootstrapNodes = 1;

// The node's per-verb wait; nim-brokers' lane may enforce a shorter one.
constexpr uint32_t kRequestTimeoutMs = 15000;

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

/// Counts an entry point in and out, for quiesce().
struct InFlightGuard {
    std::atomic<int>& n;
    explicit InFlightGuard(std::atomic<int>& counter) : n(counter) { n.fetch_add(1); }
    ~InFlightGuard() { n.fetch_sub(1); }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;
};

void writeErr(char* errBuf, size_t errBufLen, const std::string& msg)
{
    if (!errBuf || errBufLen == 0) {
        return;
    }
    const size_t n = msg.size() < errBufLen - 1 ? msg.size() : errBufLen - 1;
    std::memcpy(errBuf, msg.data(), n);
    errBuf[n] = '\0';
}

/// Turns one typed reply into an LD_DISCO_* code. Transport failures
/// (CallError) and libp2p's own refusals are reported apart.
int settle(const char* method, const StdLogosResult& r, const logos::CallError& err,
           char* errBuf, size_t errBufLen)
{
    if (!err.ok()) {
        const std::string msg =
            std::string(method) + ": " + err.code + ": " + err.message;
        trace("%-22s TRANSPORT-ERR  %s", method, msg.c_str());
        writeErr(errBuf, errBufLen, msg);
        return LD_DISCO_ERROR;
    }
    if (!r.success) {
        const std::string msg =
            r.error.empty() ? std::string(method) + " failed" : r.error;
        trace("%-22s REFUSED        %s", method, msg.c_str());
        writeErr(errBuf, errBufLen, msg);
        return LD_DISCO_ERROR;
    }
    return LD_DISCO_OK;
}

/// Hands a lookup's records to logos-delivery as JSON array text; libp2p's
/// record shape is already the one it parses. Freed through freeString.
bool emitJsonArray(const nlohmann::json& value, char** outJson,
                   char* errBuf, size_t errBufLen)
{
    const std::string json = value.is_array() ? value.dump() : std::string("[]");
    *outJson = strdup(json.c_str());
    if (!*outJson) {
        writeErr(errBuf, errBufLen, "out of memory copying lookup result");
        return false;
    }
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
    , vtable_{}
{
    vtable_.abiVersion = LD_DISCO_ABI_VERSION;
    vtable_.pluginCtx = this;
    vtable_.requestTimeoutMs = kRequestTimeoutMs;
    vtable_.start = &DeliveryServiceDiscoveryPlugin::cStart;
    vtable_.stop = &DeliveryServiceDiscoveryPlugin::cStop;
    vtable_.lookup = &DeliveryServiceDiscoveryPlugin::cLookup;
    vtable_.randomLookup = &DeliveryServiceDiscoveryPlugin::cRandomLookup;
    vtable_.freeString = &DeliveryServiceDiscoveryPlugin::cFreeString;
    vtable_.startAdvertising = &DeliveryServiceDiscoveryPlugin::cStartAdvertising;
    vtable_.stopAdvertising = &DeliveryServiceDiscoveryPlugin::cStopAdvertising;
    vtable_.registerInterest = &DeliveryServiceDiscoveryPlugin::cRegisterInterest;
    vtable_.unregisterInterest = &DeliveryServiceDiscoveryPlugin::cUnregisterInterest;
}

std::string DeliveryServiceDiscoveryPlugin::toServiceId(const char* key)
{
    if (!key) {
        return {};
    }
    const std::string k(key);
    constexpr const char* kServicePrefix = "service:";
    constexpr size_t kServicePrefixLen = 8;
    if (k.rfind(kServicePrefix, 0) == 0) {
        return k.substr(kServicePrefixLen);
    }
    return k;
}

bool DeliveryServiceDiscoveryPlugin::quiesce(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (inFlight_.load() > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            trace("quiesce                  TIMEOUT  %d call(s) still in flight",
                  inFlight_.load());
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

int DeliveryServiceDiscoveryPlugin::requireBackend(char* errBuf, size_t errBufLen)
{
    const std::string failure = ensureBackend();
    if (failure.empty()) {
        return LD_DISCO_OK;
    }
    trace("libp2p backend           UNAVAILABLE  %s", failure.c_str());
    writeErr(errBuf, errBufLen, "libp2p backend unavailable: " + failure);
    return LD_DISCO_ERROR;
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
    size_t bootstrapCount = 0;
    if (const auto nodes = cfg.find("bootstrapNodes");
        nodes != cfg.end() && nodes->is_array()) {
        if (nodes->size() > kMaxBootstrapNodes) {
            trace("libp2p bootstrapNodes    %zu configured, handing over the first %zu",
                  nodes->size(), kMaxBootstrapNodes);
            nlohmann::json kept = nlohmann::json::array();
            for (size_t i = 0; i < kMaxBootstrapNodes; ++i) {
                kept.push_back((*nodes)[i]);
            }
            *nodes = kept;
        }
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

// --- vtable trampolines ------------------------------------------------------

#define LD_SELF(ctx) static_cast<DeliveryServiceDiscoveryPlugin*>(ctx)

int DeliveryServiceDiscoveryPlugin::cStart(void* ctx, char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    // Only brings libp2p up. Its discovery is shared by the whole process and
    // outlives this node, so the node's start/stop does not drive it.
    const int ready = LD_SELF(ctx)->requireBackend(errBuf, errBufLen);
    if (ready != LD_DISCO_OK) {
        return ready;
    }
    trace("%-22s OK (backend already running)", "start");
    return LD_DISCO_OK;
}

int DeliveryServiceDiscoveryPlugin::cStop(void* ctx, char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    // No-op: libp2p's discovery is shared (see cStart).
    (void)ctx;
    (void)errBuf;
    (void)errBufLen;
    trace("%-22s OK (no-op; libp2p discovery is shared)", "stop");
    return LD_DISCO_OK;
}

int DeliveryServiceDiscoveryPlugin::cLookup(void* ctx, const char* key, int64_t limit,
                                            char** outJson, char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    const int ready = LD_SELF(ctx)->requireBackend(errBuf, errBufLen);
    if (ready != LD_DISCO_OK) {
        return ready;
    }
    // libp2p's discoLookup takes (serviceId, serviceData) and has no result
    // cap, so `limit` has nowhere to go; the caller trims what it gets back.
    (void)limit;
    logos::CallError err;
    const StdLogosResult r =
        LD_SELF(ctx)->libp2p_->discoLookup(toServiceId(key), std::string(), &err);
    const int rc = settle("discoLookup", r, err, errBuf, errBufLen);
    if (rc != LD_DISCO_OK) {
        return rc;
    }
    // Peer ids, so a peer can be told from our own provider record.
    std::string peers;
    if (r.value.is_array()) {
        for (const auto& rec : r.value) {
            if (!rec.is_object() || !rec.contains("peerId")) continue;
            const std::string id = rec["peerId"].get<std::string>();
            if (!peers.empty()) peers += ",";
            peers += id.size() > 12 ? id.substr(id.size() - 8) : id;
        }
    }
    trace("%-22s OK             key=%s records=%zu peers=[%s]", "discoLookup",
          toServiceId(key).c_str(), r.value.is_array() ? r.value.size() : 0,
          peers.c_str());
    return emitJsonArray(r.value, outJson, errBuf, errBufLen) ? LD_DISCO_OK : LD_DISCO_ERROR;
}

int DeliveryServiceDiscoveryPlugin::cRandomLookup(void* ctx, char** outJson,
                                                  char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    const int ready = LD_SELF(ctx)->requireBackend(errBuf, errBufLen);
    if (ready != LD_DISCO_OK) {
        return ready;
    }
    logos::CallError err;
    const StdLogosResult r = LD_SELF(ctx)->libp2p_->discoRandomLookup(&err);
    const int rc = settle("discoRandomLookup", r, err, errBuf, errBufLen);
    if (rc != LD_DISCO_OK) {
        return rc;
    }
    // Peer ids, to tell random-walk results from service lookups.
    std::string rpeers;
    if (r.value.is_array()) {
        for (const auto& rec : r.value) {
            if (!rec.is_object() || !rec.contains("peerId")) continue;
            const std::string id = rec["peerId"].get<std::string>();
            if (!rpeers.empty()) rpeers += ",";
            rpeers += id.size() > 12 ? id.substr(id.size() - 8) : id;
        }
    }
    trace("%-22s OK             records=%zu peers=[%s]", "discoRandomLookup",
          r.value.is_array() ? r.value.size() : 0, rpeers.c_str());
    return emitJsonArray(r.value, outJson, errBuf, errBufLen) ? LD_DISCO_OK : LD_DISCO_ERROR;
}

void DeliveryServiceDiscoveryPlugin::cFreeString(void* ctx, char* s)
{
    (void)ctx;
    free(s);
}

int DeliveryServiceDiscoveryPlugin::cStartAdvertising(void* ctx, const char* key,
                                                      const uint8_t* data, size_t dataLen,
                                                      const uint8_t* record, size_t recordLen,
                                                      char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    const int ready = LD_SELF(ctx)->requireBackend(errBuf, errBufLen);
    if (ready != LD_DISCO_OK) {
        return ready;
    }
    // `record` is the node's signed peer record (protobuf), base64-encoded for
    // libp2p's JSON transport. With a record, `data` is redundant but libp2p
    // rejects an empty serviceData and it may not be JSON-safe, so a marker
    // goes instead. Either may be (NULL, 0).
    const bool hasRecord = record && recordLen;
    const std::string advertisement =
        hasRecord ? delivery_base64::encode(record, recordLen) : std::string();
    const std::string serviceData =
        hasRecord ? std::string("xpr")
        : data && dataLen ? std::string(reinterpret_cast<const char*>(data), dataLen)
                          : std::string();
    logos::CallError err;
    const StdLogosResult r = LD_SELF(ctx)->libp2p_->discoStartAdvertising(
        toServiceId(key), serviceData, advertisement, &err);
    // recordLen is the raw size, advertLen the base64 one sent.
    trace("%-22s ->  key=%s dataLen=%zu recordLen=%zu advertLen=%zu",
          "discoStartAdvertising", toServiceId(key).c_str(), serviceData.size(),
          recordLen, advertisement.size());
    const int rc = settle("discoStartAdvertising", r, err, errBuf, errBufLen);
    if (rc == LD_DISCO_OK)
        trace("%-22s OK             key=%s data=%s", "discoStartAdvertising",
              toServiceId(key).c_str(), serviceData.c_str());
    return rc;
}

int DeliveryServiceDiscoveryPlugin::cStopAdvertising(void* ctx, const char* key,
                                                     char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    const int ready = LD_SELF(ctx)->requireBackend(errBuf, errBufLen);
    if (ready != LD_DISCO_OK) {
        return ready;
    }
    logos::CallError err;
    const StdLogosResult r =
        LD_SELF(ctx)->libp2p_->discoStopAdvertising(toServiceId(key), &err);
    const int rc = settle("discoStopAdvertising", r, err, errBuf, errBufLen);
    if (rc == LD_DISCO_OK)
        trace("%-22s OK             key=%s", "discoStopAdvertising", toServiceId(key).c_str());
    return rc;
}

int DeliveryServiceDiscoveryPlugin::cRegisterInterest(void* ctx, const char* key,
                                                      char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    const int ready = LD_SELF(ctx)->requireBackend(errBuf, errBufLen);
    if (ready != LD_DISCO_OK) {
        return ready;
    }
    logos::CallError err;
    const StdLogosResult r =
        LD_SELF(ctx)->libp2p_->discoRegisterInterest(toServiceId(key), &err);
    const int rc = settle("discoRegisterInterest", r, err, errBuf, errBufLen);
    if (rc == LD_DISCO_OK)
        trace("%-22s OK             key=%s", "discoRegisterInterest", toServiceId(key).c_str());
    return rc;
}

int DeliveryServiceDiscoveryPlugin::cUnregisterInterest(void* ctx, const char* key,
                                                        char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    const int ready = LD_SELF(ctx)->requireBackend(errBuf, errBufLen);
    if (ready != LD_DISCO_OK) {
        return ready;
    }
    logos::CallError err;
    const StdLogosResult r =
        LD_SELF(ctx)->libp2p_->discoUnregisterInterest(toServiceId(key), &err);
    const int rc = settle("discoUnregisterInterest", r, err, errBuf, errBufLen);
    if (rc == LD_DISCO_OK)
        trace("%-22s OK             key=%s", "discoUnregisterInterest", toServiceId(key).c_str());
    return rc;
}

#undef LD_SELF
