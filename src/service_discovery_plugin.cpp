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

#include <nlohmann/json.hpp>

// Generated at build time from metadata.json#optional_dependencies.
#include "base64.h"
#include "libp2p_bring_up.h"
#include "libp2p_module_api.h"

namespace {

// The node's per-verb wait; nim-brokers' lane may enforce a shorter one.
constexpr uint32_t kRequestTimeoutMs = 15000;

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

/// Runs one entry point's body. They are called from Nim over the C ABI, where
/// an escaping exception terminates the process, so report it instead. The
/// message is written without allocating, so reporting cannot throw again.
template <typename Body>
int guarded(const char* verb, char* errBuf, size_t errBufLen, Body&& body)
{
    const char* what = "unknown exception";
    try {
        return body();
    } catch (const std::exception& e) {
        what = e.what();
    } catch (...) {
    }
    trace("%-22s EXCEPTION      %s", verb, what);
    if (errBuf && errBufLen > 0) {
        std::snprintf(errBuf, errBufLen, "%s: %s", verb, what);
    }
    return LD_DISCO_ERROR;
}

} // namespace

DeliveryServiceDiscoveryPlugin::DeliveryServiceDiscoveryPlugin(std::shared_ptr<Libp2pBringUp> bringUp)
    : bringUp_(std::move(bringUp))
    , libp2p_(bringUp_ ? bringUp_->module() : nullptr)
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
    if (!bringUp_) {
        return "no libp2p_module client";
    }
    const std::string failure = bringUp_->ensure();
    if (failure == Libp2pBringUp::kUnavailable) {
        return kLibp2pUnavailable;
    }
    return failure;
}

// --- vtable trampolines ------------------------------------------------------

#define LD_SELF(ctx) static_cast<DeliveryServiceDiscoveryPlugin*>(ctx)

int DeliveryServiceDiscoveryPlugin::cStart(void* ctx, char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    return guarded("start", errBuf, errBufLen, [&]() -> int {
        // Only brings libp2p up. Its discovery is shared by the whole process and
        // outlives this node, so the node's start/stop does not drive it.
        const int ready = LD_SELF(ctx)->requireBackend(errBuf, errBufLen);
        if (ready != LD_DISCO_OK) {
            return ready;
        }
        trace("%-22s OK (backend already running)", "start");
        return LD_DISCO_OK;
    });
}

int DeliveryServiceDiscoveryPlugin::cStop(void* ctx, char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    return guarded("stop", errBuf, errBufLen, [&]() -> int {
        // No-op: libp2p's discovery is shared (see cStart).
        (void)ctx;
        (void)errBuf;
        (void)errBufLen;
        trace("%-22s OK (no-op; libp2p discovery is shared)", "stop");
        return LD_DISCO_OK;
    });
}

int DeliveryServiceDiscoveryPlugin::cLookup(void* ctx, const char* key, int64_t limit,
                                            char** outJson, char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    return guarded("lookup", errBuf, errBufLen, [&]() -> int {
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
                if (!rec.is_object() || !rec.contains("peerId") || !rec["peerId"].is_string()) continue;
                const std::string id = rec["peerId"].get<std::string>();
                if (!peers.empty()) peers += ",";
                peers += id.size() > 12 ? id.substr(id.size() - 8) : id;
            }
        }
        trace("%-22s OK             key=%s records=%zu peers=[%s]", "discoLookup",
              toServiceId(key).c_str(), r.value.is_array() ? r.value.size() : 0,
              peers.c_str());
        return emitJsonArray(r.value, outJson, errBuf, errBufLen) ? LD_DISCO_OK : LD_DISCO_ERROR;
    });
}

int DeliveryServiceDiscoveryPlugin::cRandomLookup(void* ctx, char** outJson,
                                                  char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    return guarded("randomLookup", errBuf, errBufLen, [&]() -> int {
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
                if (!rec.is_object() || !rec.contains("peerId") || !rec["peerId"].is_string()) continue;
                const std::string id = rec["peerId"].get<std::string>();
                if (!rpeers.empty()) rpeers += ",";
                rpeers += id.size() > 12 ? id.substr(id.size() - 8) : id;
            }
        }
        trace("%-22s OK             records=%zu peers=[%s]", "discoRandomLookup",
              r.value.is_array() ? r.value.size() : 0, rpeers.c_str());
        return emitJsonArray(r.value, outJson, errBuf, errBufLen) ? LD_DISCO_OK : LD_DISCO_ERROR;
    });
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
    return guarded("startAdvertising", errBuf, errBufLen, [&]() -> int {
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
    });
}

int DeliveryServiceDiscoveryPlugin::cStopAdvertising(void* ctx, const char* key,
                                                     char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    return guarded("stopAdvertising", errBuf, errBufLen, [&]() -> int {
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
    });
}

int DeliveryServiceDiscoveryPlugin::cRegisterInterest(void* ctx, const char* key,
                                                      char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    return guarded("registerInterest", errBuf, errBufLen, [&]() -> int {
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
    });
}

int DeliveryServiceDiscoveryPlugin::cUnregisterInterest(void* ctx, const char* key,
                                                        char* errBuf, size_t errBufLen)
{
    const InFlightGuard inFlight(LD_SELF(ctx)->inFlight_);
    return guarded("unregisterInterest", errBuf, errBufLen, [&]() -> int {
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
    });
}

#undef LD_SELF
