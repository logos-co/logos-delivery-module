#include "service_discovery_plugin.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <QByteArray>
#include <QJsonDocument>
#include <QMetaType>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <logos_api_client.h>
#include <logos_types.h>
#include <token_manager.h>

namespace {

constexpr const char* kLibp2pModule = "libp2p_module";
constexpr const char* kOriginModule = "delivery_module";

// Above libp2p's own kDefaultOpTimeoutMs (10s) so a slow operation surfaces as
// libp2p's error rather than as an SDK transport timeout with no diagnostic.
constexpr int kCallTimeoutMs = 15000;

// Handed to logos-delivery in the vtable. It caps how long the node waits for
// one verb; the value is only an upper bound, since nim-brokers' cross-thread
// lane enforces its own (shorter) timeout on top.
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

/// Decodes the reply of a StdLogosResult-returning module method.
///
/// Two shapes are accepted. Cross-process (the default) the reply arrives as a
/// plain map, because the wire encoder special-cases LogosResult into
/// {success, value, error} but the decoder has no reverse case and no
/// QVariantMap->LogosResult converter is registered. In-process the QVariant
/// carries a real LogosResult, since nothing is serialized.
bool decodeResult(const QVariant& reply, QVariant& valueOut, std::string& errorOut)
{
    if (!reply.isValid()) {
        errorOut = "no reply from " + std::string(kLibp2pModule)
            + " (not loaded, or the call timed out)";
        return false;
    }

    const int logosResultId = QMetaType::fromName("LogosResult").id();
    if (logosResultId != QMetaType::UnknownType && reply.userType() == logosResultId) {
        const LogosResult r = reply.value<LogosResult>();
        valueOut = r.value;
        errorOut = r.error.toString().toStdString();
        return r.success;
    }

    if (reply.canConvert<QVariantMap>()) {
        const QVariantMap map = reply.toMap();
        if (map.contains("success")) {
            valueOut = map.value("value");
            errorOut = map.value("error").toString().toStdString();
            return map.value("success").toBool();
        }
    }

    errorOut = "unrecognised reply shape from " + std::string(kLibp2pModule);
    return false;
}

/// Serialises the `value` half of a reply back into the JSON array text the
/// plugin ABI expects. libp2p's lookups already produce
/// {peerId, seqNo, addrs, services:[{id, data}]} records, which is exactly what
/// logos-delivery parses, so this is a pass-through with no reshaping.
std::string toJsonArrayText(const QVariant& value)
{
    if (!value.isValid()) {
        return "[]";
    }
    const QJsonDocument doc = QJsonDocument::fromVariant(value);
    if (!doc.isArray()) {
        return "[]";
    }
    return doc.toJson(QJsonDocument::Compact).toStdString();
}

} // namespace

DeliveryServiceDiscoveryPlugin::DeliveryServiceDiscoveryPlugin()
    : client_(std::make_unique<LogosAPIClient>(
          QString::fromUtf8(kLibp2pModule), QString::fromUtf8(kOriginModule),
          &TokenManager::instance()))
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

DeliveryServiceDiscoveryPlugin::~DeliveryServiceDiscoveryPlugin() = default;

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

int DeliveryServiceDiscoveryPlugin::forward(const char* method,
                                            const std::vector<std::string>& args,
                                            char** outJson,
                                            char* errBuf,
                                            size_t errBufLen)
{
    LogosAPIClient* c = client();
    if (!c) {
        writeErr(errBuf, errBufLen,
                 std::string("no client for ") + kLibp2pModule
                     + "; is the module loaded?");
        return LD_DISCO_ERROR;
    }

    QVariantList qargs;
    qargs.reserve(static_cast<int>(args.size()));
    for (const std::string& a : args) {
        qargs.append(QVariant(QString::fromStdString(a)));
    }

    const QVariant reply = c->invokeRemoteMethod(
        kLibp2pModule, method, qargs, Timeout(kCallTimeoutMs));

    QVariant value;
    std::string error;
    if (!decodeResult(reply, value, error)) {
        if (error.empty()) {
            error = std::string(method) + " failed";
        }
        fprintf(stderr, "DeliveryServiceDiscoveryPlugin: %s -> %s\n", method, error.c_str());
        trace("%-22s FAILED  %s%s%s", method, args.empty() ? "" : "key=",
              args.empty() ? "" : args[0].c_str(), args.empty() ? "" : "  ");
        trace("%-22s   `-> %s", method, error.c_str());
        writeErr(errBuf, errBufLen, error);
        return LD_DISCO_ERROR;
    }

    if (outJson) {
        const std::string json = toJsonArrayText(value);
        // strdup so ownership is plain malloc/free across the C boundary;
        // logos-delivery hands the buffer back through freeString.
        *outJson = strdup(json.c_str());
        if (!*outJson) {
            writeErr(errBuf, errBufLen, "out of memory copying lookup result");
            return LD_DISCO_ERROR;
        }
        trace("%-22s OK      %srecords=%d", method,
              args.empty() ? "" : ("key=" + args[0] + "  ").c_str(),
              value.isValid() ? value.toList().size() : 0);
    } else {
        trace("%-22s OK      %s", method,
              args.empty() ? "" : ("key=" + args[0]).c_str());
    }
    return LD_DISCO_OK;
}

std::string DeliveryServiceDiscoveryPlugin::initialiseBackend(const std::string& libp2pConfig)
{
    LogosAPIClient* c = client();
    if (!c) {
        return std::string("could not reach ") + kLibp2pModule
            + "; load it before enabling plugin kad discovery";
    }

    std::string diagnostics;
    trace("---- installing service discovery plugin ----");

    if (!libp2pConfig.empty()) {
        const QVariant reply = c->invokeRemoteMethod(
            kLibp2pModule, "createNode",
            QVariantList() << QVariant(QString::fromStdString(libp2pConfig)),
            Timeout(kCallTimeoutMs));
        QVariant value;
        std::string error;
        if (!decodeResult(reply, value, error)) {
            // Another module may already own the node; that is not our failure.
            if (error.find("already created") == std::string::npos) {
                diagnostics += "createNode: " + error + "; ";
            }
            fprintf(stderr, "DeliveryServiceDiscoveryPlugin: libp2p createNode: %s\n",
                    error.c_str());
        }
    }

    // libp2p's start() calls ensureContext() first, so it brings up a default
    // node when createNode was skipped or not supplied.
    {
        const QVariant reply = c->invokeRemoteMethod(
            kLibp2pModule, "start", QVariantList(), Timeout(kCallTimeoutMs));
        QVariant value;
        std::string error;
        if (!decodeResult(reply, value, error)) {
            diagnostics += "start: " + error + "; ";
            fprintf(stderr, "DeliveryServiceDiscoveryPlugin: libp2p start: %s\n",
                    error.c_str());
        }
    }

    trace("libp2p backend ready%s%s", diagnostics.empty() ? "" : " with: ",
          diagnostics.c_str());
    return diagnostics;
}

// --- vtable trampolines ------------------------------------------------------

#define LD_SELF(ctx) static_cast<DeliveryServiceDiscoveryPlugin*>(ctx)

int DeliveryServiceDiscoveryPlugin::cStart(void* ctx, char* errBuf, size_t errBufLen)
{
    return LD_SELF(ctx)->forward("discoStart", {}, nullptr, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cStop(void* ctx, char* errBuf, size_t errBufLen)
{
    return LD_SELF(ctx)->forward("discoStop", {}, nullptr, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cLookup(void* ctx, const char* key, int64_t limit,
                                            char** outJson, char* errBuf, size_t errBufLen)
{
    // libp2p's discoLookup takes (serviceId, serviceData) and has no result
    // cap, so `limit` has nowhere to go; the caller trims what it gets back.
    (void)limit;
    return LD_SELF(ctx)->forward(
        "discoLookup", {toServiceId(key), std::string()}, outJson, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cRandomLookup(void* ctx, char** outJson,
                                                  char* errBuf, size_t errBufLen)
{
    return LD_SELF(ctx)->forward("discoRandomLookup", {}, outJson, errBuf, errBufLen);
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
    return LD_SELF(ctx)->forward(
        "discoStartAdvertising",
        {toServiceId(key), serviceData, advertisement},
        nullptr, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cStopAdvertising(void* ctx, const char* key,
                                                     char* errBuf, size_t errBufLen)
{
    return LD_SELF(ctx)->forward(
        "discoStopAdvertising", {toServiceId(key)}, nullptr, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cRegisterInterest(void* ctx, const char* key,
                                                      char* errBuf, size_t errBufLen)
{
    return LD_SELF(ctx)->forward(
        "discoRegisterInterest", {toServiceId(key)}, nullptr, errBuf, errBufLen);
}

int DeliveryServiceDiscoveryPlugin::cUnregisterInterest(void* ctx, const char* key,
                                                        char* errBuf, size_t errBufLen)
{
    return LD_SELF(ctx)->forward(
        "discoUnregisterInterest", {toServiceId(key)}, nullptr, errBuf, errBufLen);
}

#undef LD_SELF
