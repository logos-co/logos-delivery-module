#include "service_discovery_plugin.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

// Generated at build time from metadata.json#dependencies.
#include "libp2p_module_api.h"

namespace {

// libp2p needs peers before its kademlia can store a provider record or answer
// a lookup, and it takes them only as `bootstrapNodes` in its own options --
// there is no call to add them later. These are the logos.dev preset's entry
// nodes, copied from logos-delivery's networks_config.nim (cluster 3).
//
// Hardcoded rather than derived: this module cannot read the preset the node
// was configured with (logos-delivery resolves it internally and exposes no
// getter), and libp2p wants the list before anything else happens. Revisit when
// either side grows a way to pass the resolved entry nodes through.
const char* const kLogosDevBootstrapNodes[] = {
    "/dns4/delivery-01.do-ams3.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAmTUbnxLGT9JvV6mu9oPyDjqHK4Phs1VDJNUgESgNSkuby",
    "/dns4/delivery-02.do-ams3.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAmMK7PYygBtKUQ8EHp7EfaD3bCEsJrkFooK8RQ2PVpJprH",
    "/dns4/delivery-01.gc-us-central1-a.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAm4S1JYkuzDKLKQvwgAhZKs9otxXqt8SCGtB4hoJP1S397",
    "/dns4/delivery-02.gc-us-central1-a.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAm8Y9kgBNtjxvCnf1X6gnZJW5EGE4UwwCL3CCm55TwqBiH",
    "/dns4/delivery-01.ac-cn-hongkong-c.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAm8YokiNun9BkeA1ZRmhLbtNUvcwRr64F69tYj9fkGyuEP",
    "/dns4/delivery-02.ac-cn-hongkong-c.logos.dev.status.im/tcp/30303/p2p/16Uiu2HAkvwhGHKNry6LACrB8TmEFoCJKEX29XR5dDUzk3UT3UNSE",
};

// How long to wait for libp2p_module to start serving calls after it loads.
// Generous because it has been observed taking longer than 20s, and because
// giving up early is expensive: see the bail-out in ensureBackend.
constexpr std::chrono::seconds kBackendReadyTimeout{90};

// Handed to logos-delivery in the vtable. It caps how long the node waits for
// one verb; the value is only an upper bound, since nim-brokers' cross-thread
// lane enforces its own (shorter) timeout on top. The generated std client
// exposes no per-call deadline, so this is the only knob on our side.
constexpr uint32_t kRequestTimeoutMs = 15000;

// Opt-in trace of the plugin boundary, written to the file named by
// LD_DISCO_TRACE. There is no other way to watch these calls in a running node:
// logos-core reads a module process's merged stdout/stderr but discards every
// line that does not look like a Qt warning (logos-liblogos
// plugin_launcher.cpp, `onOutput`), so a module's own logging never reaches the
// daemon log. Unset means no file is opened and nothing is written.
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
    if (localtime_r(&now, &tm)) {
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    }
    fprintf(f, "[%s] ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
}

void writeErr(char* errBuf, size_t errBufLen, const std::string& msg)
{
    if (!errBuf || errBufLen == 0) {
        return;
    }
    const size_t n = msg.size() < errBufLen - 1 ? msg.size() : errBufLen - 1;
    std::memcpy(errBuf, msg.data(), n);
    errBuf[n] = '\0';
}

/// Turns one typed reply into an LD_DISCO_* code.
///
/// Two failure channels, and they mean different things. `CallError` covers the
/// transport -- module not loaded ("object_unavailable"), timed out, dispatch
/// failed -- while `StdLogosResult::success` carries libp2p's own answer. Both
/// are reported, since "libp2p refused" and "libp2p was unreachable" want
/// different fixes; the transport code is included so the first is obvious.
int settle(const char* method, const StdLogosResult& r, const logos::CallError& err,
           char* errBuf, size_t errBufLen)
{
    if (!err.ok()) {
        const std::string msg =
            std::string(method) + ": " + err.code + ": " + err.message;
        fprintf(stderr, "DeliveryServiceDiscoveryPlugin: %s\n", msg.c_str());
        trace("%-22s TRANSPORT-ERR  %s", method, msg.c_str());
        writeErr(errBuf, errBufLen, msg);
        return LD_DISCO_ERROR;
    }
    if (!r.success) {
        const std::string msg =
            r.error.empty() ? std::string(method) + " failed" : r.error;
        fprintf(stderr, "DeliveryServiceDiscoveryPlugin: %s -> %s\n", method, msg.c_str());
        trace("%-22s REFUSED        %s", method, msg.c_str());
        writeErr(errBuf, errBufLen, msg);
        return LD_DISCO_ERROR;
    }
    return LD_DISCO_OK;
}

/// Hands a lookup's `value` to logos-delivery as the JSON array text the plugin
/// ABI wants. libp2p's lookups already produce
/// {peerId, seqNo, addrs, services:[{id, data}]} records, exactly what
/// logos-delivery parses, and the std client carries them as nlohmann::json all
/// the way -- so this is a `dump()`, with no intermediate representation to get
/// wrong. Ownership is plain malloc/free across the C boundary; logos-delivery
/// hands the buffer back through freeString.
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
    constexpr const char* kSvcPrefix = "svc:";
    constexpr size_t kSvcPrefixLen = 4;
    if (k.rfind(kSvcPrefix, 0) == 0) {
        return k.substr(kSvcPrefixLen);
    }
    return k;
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

    // Wait until libp2p will actually serve calls before configuring it.
    //
    // Freshly loaded, it is reachable on the transport but rejects calls at
    // dispatch for a moment. The std generated client has no dispatch-rejection
    // handling (the Qt one calls logosDispatchRejection), so that envelope
    // arrives as success=false with no value and no message, and CallError
    // stays ok() -- indistinguishable from a genuine refusal by signature.
    //
    // What separates them is the message: every real answer from libp2p carries
    // one (`status` says "libp2p not initialized" while it has no context),
    // whereas a rejection carries nothing. So "answered" means success, or a
    // failure that bothered to say why.
    //
    // This matters more than it looks: bootstrap peers can only be given at
    // createNode, so a createNode lost to this window leaves libp2p with a
    // kademlia that has no peers and can never get any -- exactly the state in
    // which advertising and lookups quietly do nothing.
    // No readiness probe: `status` is one of the calls libp2p rejects (355
    // probes over 90s, all rejected, while `start` and every disco verb answered
    // fine in the same session), so probing with it says nothing about whether
    // the module will serve the call we actually care about.
    const bool alreadyHasNode = false;

    // Bootstrap peers first, then whatever the operator passed on top, so a
    // node config can override the defaults (including with an empty list).
    nlohmann::json cfg = nlohmann::json::object();
    cfg["bootstrapNodes"] = nlohmann::json::array();
    for (const char* addr : kLogosDevBootstrapNodes) {
        cfg["bootstrapNodes"].push_back(addr);
    }
    cfg["mountKad"] = true;
    cfg["mountServiceDiscovery"] = true;

    if (!libp2pConfig_.empty()) {
        nlohmann::json overrides = nlohmann::json::parse(libp2pConfig_, nullptr, false);
        if (overrides.is_object()) {
            cfg.update(overrides);
        } else {
            trace("libp2pConfig is not a JSON object; ignoring it");
        }
    }

    if (alreadyHasNode) {
        // Someone else built it (or a previous attempt of ours did). Its options
        // are already fixed, so our bootstrap peers cannot land -- say so
        // plainly rather than letting discovery look configured when it is not.
        trace("libp2p createNode        SKIPPED  node already exists; "
              "bootstrap peers NOT applied");
    } else {
        const std::string cfgText = cfg.dump();
        logos::CallError err;
        const StdLogosResult r = libp2p_->createNode(cfgText, &err);
        trace("libp2p createNode        %s  bootstrapNodes=%zu",
              (!err.ok() ? "TRANSPORT-ERR" : (r.success ? "OK" : "REFUSED")),
              cfg["bootstrapNodes"].size());
        if (!err.ok()) {
            diagnostics += "createNode: " + err.code + ": " + err.message + "; ";
        } else if (!r.success) {
            // Not fatal, and not currently reachable either. libp2p rejects this
            // call from this module -- always, immediately, with no message --
            // as it does `status` and `discoStartAdvertising`, while `start` and
            // the other six disco verbs answer normally on the same client and
            // thread. Until that is resolved, bootstrap peers cannot be handed
            // over this way; set them through libp2p's own LIBP2P_MODULE_CONFIG
            // (its metadata documents that as the load-time config channel).
            //
            // Discovery still starts, so the node runs and the lookup loops are
            // driven -- they simply have an empty DHT to work against.
            trace("libp2p createNode        `-> %s",
                  r.error.empty() ? "(no message)" : r.error.c_str());
            trace("libp2p bootstrap peers   NOT APPLIED  "
                  "(set LIBP2P_MODULE_CONFIG to configure libp2p)");
            fprintf(stderr,
                    "DeliveryServiceDiscoveryPlugin: libp2p createNode refused; "
                    "bootstrap peers not applied\n");
        }
    }

    // libp2p's start() calls ensureContext() first, so it brings up a default
    // node when createNode was skipped or not supplied.
    {
        logos::CallError err;
        const StdLogosResult r = libp2p_->start(&err);
        if (!err.ok()) {
            diagnostics += "start: " + err.code + ": " + err.message + "; ";
        } else if (!r.success) {
            diagnostics += "start: " + (r.error.empty() ? std::string("failed") : r.error) + "; ";
        }
    }

    trace("libp2p backend ready%s%s", diagnostics.empty() ? "" : " with: ",
          diagnostics.c_str());
    backendReady_ = diagnostics.empty();
    backendFailure_ = diagnostics;
    return diagnostics;
}

// --- vtable trampolines ------------------------------------------------------

#define LD_SELF(ctx) static_cast<DeliveryServiceDiscoveryPlugin*>(ctx)

int DeliveryServiceDiscoveryPlugin::cStart(void* ctx, char* errBuf, size_t errBufLen)
{
    // First call on the discovery thread, so this is where libp2p can be
    // reached at all -- see ensureBackend.
    const std::string failure = LD_SELF(ctx)->ensureBackend();
    if (!failure.empty()) {
        trace("libp2p backend           UNAVAILABLE  %s", failure.c_str());
        writeErr(errBuf, errBufLen, "libp2p backend unavailable: " + failure);
        return LD_DISCO_ERROR;
    }

    logos::CallError err;
    const StdLogosResult r = LD_SELF(ctx)->libp2p_->discoStart(&err);
    const int rc = settle("discoStart", r, err, errBuf, errBufLen);
    if (rc == LD_DISCO_OK) trace("%-22s OK", "discoStart");
    return rc;
}

int DeliveryServiceDiscoveryPlugin::cStop(void* ctx, char* errBuf, size_t errBufLen)
{
    logos::CallError err;
    const StdLogosResult r = LD_SELF(ctx)->libp2p_->discoStop(&err);
    const int rc = settle("discoStop", r, err, errBuf, errBufLen);
    if (rc == LD_DISCO_OK) trace("%-22s OK", "discoStop");
    return rc;
}

int DeliveryServiceDiscoveryPlugin::cLookup(void* ctx, const char* key, int64_t limit,
                                            char** outJson, char* errBuf, size_t errBufLen)
{
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
    trace("%-22s OK             key=%s records=%zu", "discoLookup",
          toServiceId(key).c_str(), r.value.is_array() ? r.value.size() : 0);
    return emitJsonArray(r.value, outJson, errBuf, errBufLen) ? LD_DISCO_OK : LD_DISCO_ERROR;
}

int DeliveryServiceDiscoveryPlugin::cRandomLookup(void* ctx, char** outJson,
                                                  char* errBuf, size_t errBufLen)
{
    logos::CallError err;
    const StdLogosResult r = LD_SELF(ctx)->libp2p_->discoRandomLookup(&err);
    const int rc = settle("discoRandomLookup", r, err, errBuf, errBufLen);
    if (rc != LD_DISCO_OK) {
        return rc;
    }
    trace("%-22s OK             records=%zu", "discoRandomLookup",
          r.value.is_array() ? r.value.size() : 0);
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
    // `data` is the JSON advertisement payload logos-delivery builds; `record`
    // is a pre-signed extended peer record, which this backend never supplies
    // (libp2p signs with its own identity when the advertisement is empty).
    // Both may be (NULL, 0) -- logos-delivery passes no record, and an
    // advertising node with nothing to say passes no data. std::string(nullptr, 0)
    // is undefined, so build them only when there is something to copy.
    const std::string serviceData =
        data && dataLen ? std::string(reinterpret_cast<const char*>(data), dataLen)
                        : std::string();
    const std::string advertisement =
        record && recordLen ? std::string(reinterpret_cast<const char*>(record), recordLen)
                            : std::string();
    logos::CallError err;
    const StdLogosResult r = LD_SELF(ctx)->libp2p_->discoStartAdvertising(
        toServiceId(key), serviceData, advertisement, &err);
    const int rc = settle("discoStartAdvertising", r, err, errBuf, errBufLen);
    if (rc == LD_DISCO_OK)
        trace("%-22s OK             key=%s data=%s", "discoStartAdvertising",
              toServiceId(key).c_str(), serviceData.c_str());
    return rc;
}

int DeliveryServiceDiscoveryPlugin::cStopAdvertising(void* ctx, const char* key,
                                                     char* errBuf, size_t errBufLen)
{
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
    logos::CallError err;
    const StdLogosResult r =
        LD_SELF(ctx)->libp2p_->discoUnregisterInterest(toServiceId(key), &err);
    const int rc = settle("discoUnregisterInterest", r, err, errBuf, errBufLen);
    if (rc == LD_DISCO_OK)
        trace("%-22s OK             key=%s", "discoUnregisterInterest", toServiceId(key).c_str());
    return rc;
}

#undef LD_SELF
