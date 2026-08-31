#include "service_discovery_plugin.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

// Generated at build time from metadata.json#dependencies.
#include "libp2p_module_api.h"

namespace {

// Handed to logos-delivery in the vtable. It caps how long the node waits for
// one verb; the value is only an upper bound, since nim-brokers' cross-thread
// lane enforces its own (shorter) timeout on top. The generated std client
// exposes no per-call deadline, so this is the only knob on our side.
constexpr uint32_t kRequestTimeoutMs = 15000;

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
        writeErr(errBuf, errBufLen, msg);
        return LD_DISCO_ERROR;
    }
    if (!r.success) {
        const std::string msg =
            r.error.empty() ? std::string(method) + " failed" : r.error;
        fprintf(stderr, "DeliveryServiceDiscoveryPlugin: %s -> %s\n", method, msg.c_str());
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

DeliveryServiceDiscoveryPlugin::DeliveryServiceDiscoveryPlugin(Libp2pModule* libp2p)
    : libp2p_(libp2p)
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

std::string DeliveryServiceDiscoveryPlugin::initialiseBackend(const std::string& libp2pConfig)
{
    if (!libp2p_) {
        return "no libp2p_module client";
    }

    std::string diagnostics;

    if (!libp2pConfig.empty()) {
        logos::CallError err;
        const StdLogosResult r = libp2p_->createNode(libp2pConfig, &err);
        if (!err.ok()) {
            diagnostics += "createNode: " + err.code + ": " + err.message + "; ";
        } else if (!r.success) {
            // Another module may already own the node; that is not our failure.
            if (r.error.find("already created") == std::string::npos) {
                diagnostics += "createNode: " + r.error + "; ";
            }
            fprintf(stderr, "DeliveryServiceDiscoveryPlugin: libp2p createNode: %s\n",
                    r.error.c_str());
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

    return diagnostics;
}

// --- vtable trampolines ------------------------------------------------------

#define LD_SELF(ctx) static_cast<DeliveryServiceDiscoveryPlugin*>(ctx)

int DeliveryServiceDiscoveryPlugin::cStart(void* ctx, char* errBuf, size_t errBufLen)
{
    logos::CallError err;
    const StdLogosResult r = LD_SELF(ctx)->libp2p_->discoStart(&err);
    return settle("discoStart", r, err, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cStop(void* ctx, char* errBuf, size_t errBufLen)
{
    logos::CallError err;
    const StdLogosResult r = LD_SELF(ctx)->libp2p_->discoStop(&err);
    return settle("discoStop", r, err, errBuf, errBufLen);
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
    const std::string serviceData(reinterpret_cast<const char*>(data), dataLen);
    const std::string advertisement(reinterpret_cast<const char*>(record), recordLen);
    logos::CallError err;
    const StdLogosResult r = LD_SELF(ctx)->libp2p_->discoStartAdvertising(
        toServiceId(key), serviceData, advertisement, &err);
    return settle("discoStartAdvertising", r, err, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cStopAdvertising(void* ctx, const char* key,
                                                     char* errBuf, size_t errBufLen)
{
    logos::CallError err;
    const StdLogosResult r =
        LD_SELF(ctx)->libp2p_->discoStopAdvertising(toServiceId(key), &err);
    return settle("discoStopAdvertising", r, err, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cRegisterInterest(void* ctx, const char* key,
                                                      char* errBuf, size_t errBufLen)
{
    logos::CallError err;
    const StdLogosResult r =
        LD_SELF(ctx)->libp2p_->discoRegisterInterest(toServiceId(key), &err);
    return settle("discoRegisterInterest", r, err, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cUnregisterInterest(void* ctx, const char* key,
                                                        char* errBuf, size_t errBufLen)
{
    logos::CallError err;
    const StdLogosResult r =
        LD_SELF(ctx)->libp2p_->discoUnregisterInterest(toServiceId(key), &err);
    return settle("discoUnregisterInterest", r, err, errBuf, errBufLen);
}

#undef LD_SELF
