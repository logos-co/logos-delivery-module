#include "delivery_module_plugin.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>

#include <nlohmann/json.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include <nim_ffi_host.hpp>
extern "C" {
#include <liblogosdelivery_poll.h>
}
#if __has_include(<QSocketNotifier>)
#include <QObject>
#include <QSocketNotifier>
#define DELIVERY_HAS_QT 1
#endif
#include "rln_bridge.h"
#include "rln_presets.h"

// Generated at build time from metadata.json#optional_dependencies; defines the
// LogosModules aggregate behind LogosModuleContext::modules().
#include "logos_sdk.h"
extern "C" {
// Kernel tier: unstable, may change without a deprecation cycle. Only
// waku_store_query is consumed from it; everything else goes through the
// stable surface above.
}

namespace {
namespace b64 = boost::beast::detail::base64;

std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.resize(b64::encoded_size(data.size()));
    out.resize(b64::encode(out.data(), data.data(), data.size()));
    return out;
}

std::vector<uint8_t> base64Decode(const std::string& encoded) {
    std::vector<uint8_t> out;
    out.resize(b64::decoded_size(encoded.size()));
    auto [written, read] = b64::decode(out.data(), encoded.data(), encoded.size());
    out.resize(written);
    return out;
}

int64_t currentTimestampNs() {
    // std::chrono, not clock_gettime(CLOCK_REALTIME): mingw declares neither,
    // and system_clock is the portable spelling of the same reading.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string toStringOrEmpty(const char* s) {
    return s ? std::string(s) : std::string();
}

// The text a generated reply carries: the result on success, else the error.
std::string replyText(int errCode, const char* const* reply, const char* errMsg) {
    return toStringOrEmpty(errCode == NIMFFI_RET_OK ? (reply ? *reply : nullptr) : errMsg);
}

// message_received and channel_message_received: base64 string.
std::vector<uint8_t> decodeBase64Payload(const nlohmann::json& payloadValue) {
    if (!payloadValue.is_string()) {
        return {};
    }
    return base64Decode(payloadValue.get<std::string>());
}

// Wire names of the events this module forwards. nim-ffi 0.3.0 replaced the
// single global event callback with a per-event listener registry, so each name
// is registered separately; the JSON payload still carries the snake_case
// "eventType" that event_callback dispatches on. Upstream also emits
// onTopicHealthChange, onConnectionChange and onReceivedMessage, which the
// module does not surface.
using Bytes = nim_ffi::Bytes;

Bytes encode(const nlohmann::json& request)
{
    return nlohmann::json::to_cbor(request.is_null() ? nlohmann::json::object() : request);
}

// A method's outcome in the module's result shape. The library's reply value
// is the result; a text reply stays text, as the old string-only ABI had it.
StdLogosResult fromReply(const nim_ffi::Reply& r, bool wantValue)
{
    if (r.ret != NIMFFI_RET_OK) {
        return {false, {}, r.error.empty() ? "rc=" + std::to_string(r.ret) : r.error};
    }
    if (!wantValue) {
        return {true, {}};
    }
    if (r.payload.empty()) {
        return {true, std::string()};
    }
    nlohmann::json value = nlohmann::json::from_cbor(r.payload, /*strict=*/true, /*allow_exceptions=*/false);
    if (value.is_null()) {
        return {true, std::string()};
    }
    return {true, value.is_string() ? value : nlohmann::json(value.dump())};
}

// One method call: submit, pump until the reply, map it.
StdLogosResult callApi(nim_ffi::Host& host, const char* name, nim_ffi::Method fn,
                       const nlohmann::json& req, std::chrono::seconds timeout, bool wantValue)
{
    nim_ffi::Reply r = host.call(fn, encode(req), timeout);
    if (r.ret != NIMFFI_RET_OK && !r.error.empty()) {
        r.error = std::string(name) + ": " + r.error;
    }
    return fromReply(r, wantValue);
}

// An event's payload is whatever the library enqueued: liblogosdelivery emits
// its events as JSON text, so that is tried first; a CBOR value second.
nlohmann::json decodeEvent(const uint8_t* payload, size_t len)
{
    if (len == 0) return nlohmann::json();
    const auto* text = reinterpret_cast<const char*>(payload);
    nlohmann::json j = nlohmann::json::parse(text, text + len, nullptr, /*allow_exceptions=*/false);
    if (!j.is_discarded()) return j;
    return nlohmann::json::from_cbor(std::vector<uint8_t>(payload, payload + len), true, false);
}

nlohmann::json decodeArgs(const uint8_t* args, size_t len)
{
    if (len == 0) return nlohmann::json::object();
    return nlohmann::json::from_cbor(std::vector<uint8_t>(args, args + len), true, false);
}

// The five fixed exports nim-ffi's host needs, bound once.
nim_ffi::Library deliveryLibrary()
{
    return {logosdelivery_create_node, logosdelivery_destroy, logosdelivery_poll, logosdelivery_poll_fd,
            logosdelivery_reverse_reply};
}

} // namespace

DeliveryModuleImpl::DeliveryModuleImpl()
    : host(std::make_unique<nim_ffi::Host>(deliveryLibrary()))
    , rlnBridge(std::make_unique<RlnBridge>())
{
    fprintf(stderr, "DeliveryModuleImpl: Initializing...\n");
    host->onEvent([this](uint64_t nameId, const uint8_t* payload, size_t len) {
        onEvent(nameId, decodeEvent(payload, len));
    });
    host->onReverseCall([this](uint64_t callId, uint64_t nameId, const uint8_t* args, size_t len) {
        onReverseCall(callId, nameId, decodeArgs(args, len));
    });
    // The bridge's answers go back into the library as reverse replies; the
    // pump takes them from any thread.
    RlnBridge::setResponder([this](uint64_t callId, const std::string& replyJson) {
        fprintf(stderr, "DeliveryModuleImpl: rln answer for call %llu: %.160s\n",
                static_cast<unsigned long long>(callId), replyJson.c_str());
        host->reverseReply(callId, NIMFFI_RET_OK, nlohmann::json::to_cbor(nlohmann::json(replyJson)));
    });
}

std::string DeliveryModuleImpl::bringUpRlnBridge()
{
    if (!isContextReady()) {
        // Unit tests construct this impl without a framework; modules() would
        // dereference an unset pointer here.
        return "module context not ready";
    }
    rlnBridge->init(&modules().liblogos_rln_module);
    return rlnBridge->enable();
}

StdLogosResult DeliveryModuleImpl::rlnBridgeEnable()
{
    const std::string err = bringUpRlnBridge();
    if (!err.empty()) {
        return {false, {}, err};
    }
    fprintf(stderr, "DeliveryModuleImpl: rln bridge enabled (in-process responder)\n");
    return {true, {}};
}

DeliveryModuleImpl::~DeliveryModuleImpl()
{
    releaseNode();
}

std::shared_ptr<const DeliveryRlnConfig> DeliveryModuleImpl::rlnConfigSnapshot() const
{
    std::lock_guard<std::mutex> lock(rlnConfigMutex);
    return rlnConfig;
}

void DeliveryModuleImpl::abortRln()
{
    // Only this thread writes rlnConfig, so reading it here needs no lock.
    if (!rlnConfig->enabled) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(rlnConfigMutex);
        rlnConfig = std::make_shared<const DeliveryRlnConfig>();
    }
    {
        std::lock_guard<std::mutex> lock(rlnStateMutex);
        rlnStateConfig = DeliveryRlnConfig{};
    }
}

void DeliveryModuleImpl::releaseNode()
{
    // The bring-up thread touches rlnBridge and rlnConfig; nothing below may
    // run while it is still in flight. A no-op when no thread was started.
    joinRlnBringUp();

    // Before ctx_destroy on purpose: clearing the RLN surface fails all
    // in-flight RLN requests, so no new RLN callback is dispatched into this
    // object while the node goes away. (A callback already executing on the
    // library thread is not joined; it holds its own rlnConfig snapshot.)
    // Guarded on rlnConfig->enabled because the library's plugin is
    // process-global -- clearing it unconditionally would disarm another
    // instance's RLN.
    abortRln();

    // Destroys the context and stops the node. Once it is gone no question
    // can arrive, and a late RLN completion finds no context to answer into.
    pollNotifier.reset();
    host->destroy();
}

StdLogosResult DeliveryModuleImpl::releaseAndFail(std::string reason)
{
    const bool rlnWasInstalled = rlnConfig->enabled;
    releaseNode();
    if (rlnWasInstalled) {
        setRlnState("Disabled", {});
    }
    return {false, {}, std::move(reason)};
}

// An event from the library. The payload is the same JSON object the
// callback ABI delivered: `eventType` names it, the rest is its fields.
void DeliveryModuleImpl::onEvent(uint64_t nameId, const nlohmann::json& jsonObj)
{
    (void)nameId;
    if (!jsonObj.is_object()) {
        fprintf(stderr, "DeliveryModuleImpl::onEvent: event payload is not a JSON object\n");
        return;
    }
    DeliveryModuleImpl* impl = this;
    try {
            std::string eventType = jsonObj.value("eventType", "");
            int64_t timestamp = currentTimestampNs();

            if (eventType == "message_queued") {
                impl->messageQueued(
                    jsonObj.value("requestId", ""),
                    jsonObj.value("messageHash", ""),
                    timestamp);

            } else if (eventType == "message_sent") {
                impl->messageSent(
                    jsonObj.value("requestId", ""),
                    jsonObj.value("messageHash", ""),
                    timestamp);

            } else if (eventType == "message_error") {
                impl->messageError(
                    jsonObj.value("requestId", ""),
                    jsonObj.value("messageHash", ""),
                    jsonObj.value("error", ""),
                    timestamp);

            } else if (eventType == "message_propagated") {
                impl->messagePropagated(
                    jsonObj.value("requestId", ""),
                    jsonObj.value("messageHash", ""),
                    timestamp);

            } else if (eventType == "message_received") {
                auto msgObj = jsonObj.value("message", nlohmann::json::object());

                std::string hash = jsonObj.value("messageHash", "");
                std::string topic = msgObj.value("contentTopic", "");
                std::string source = jsonObj.value("source", "");

                std::vector<uint8_t> payloadBytes;
                if (msgObj.contains("payload")) {
                    payloadBytes = decodeBase64Payload(msgObj["payload"]);
                }

                int64_t msgTimestamp = static_cast<int64_t>(msgObj.value("timestamp", 0.0));
                impl->messageReceived(hash, topic, payloadBytes, source, msgTimestamp);

            } else if (eventType == "connection_status_change") {
                impl->connectionStateChanged(
                    jsonObj.value("connectionStatus", ""),
                    timestamp);

            } else if (eventType == "channel_message_received") {
                std::vector<uint8_t> payloadBytes;
                if (jsonObj.contains("payload")) {
                    payloadBytes = decodeBase64Payload(jsonObj["payload"]);
                }
                impl->channelMessageReceived(
                    jsonObj.value("channelId", ""),
                    jsonObj.value("senderId", ""),
                    payloadBytes,
                    timestamp);

            } else if (eventType == "channel_message_sent") {
                impl->channelMessageSent(
                    jsonObj.value("channelId", ""),
                    jsonObj.value("requestId", ""),
                    timestamp);

            } else if (eventType == "channel_message_error") {
                impl->channelMessageError(
                    jsonObj.value("channelId", ""),
                    jsonObj.value("requestId", ""),
                    jsonObj.value("error", ""),
                    timestamp);

            } else {
                fprintf(stderr, "DeliveryModuleImpl::event_callback: Unknown event type: %s\n", eventType.c_str());
            }
    } catch (const nlohmann::json::exception& e) {
        fprintf(stderr, "DeliveryModuleImpl::onEvent: Invalid event JSON: %s\n", e.what());
    }
}

// A question from the library, as a nim-ffi reverse call: the wire name says
// which, the args are its CBOR map. The registry and identifier are this
// module's own configuration (the library is registry-agnostic). Each is
// forwarded to the RLN module and reported as an rln*Request event.
void DeliveryModuleImpl::onReverseCall(uint64_t callId, uint64_t nameId, const nlohmann::json& args)
{
    auto str = [&](const char* key) {
        auto it = args.find(key);
        return (it != args.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };
    auto ts = [&]() -> uint64_t {
        auto it = args.find("timestamp");
        return (it != args.end() && it->is_number()) ? it->get<uint64_t>() : 0;
    };
    const auto reqId = static_cast<int64_t>(callId);
    const auto cfg = rlnConfigSnapshot();
    fprintf(stderr, "DeliveryModuleImpl: rln question name_id=%llx (call %llu)\n",
            static_cast<unsigned long long>(nameId), static_cast<unsigned long long>(callId));
    const auto now = currentTimestampNs();
    static const uint64_t kGetState = nimffi_name_id("rln_get_membership_state");
    static const uint64_t kGetQuota = nimffi_name_id("rln_get_epoch_quota");
    static const uint64_t kGenerate = nimffi_name_id("rln_generate_proof");
    static const uint64_t kValidate = nimffi_name_id("rln_validate_proof");
    try {
        if (nameId == kGetState) {
            rlnBridge->getMembershipState(callId, cfg->registryId, cfg->rlnIdentifier);
            dispatchRlnGetMembershipStateRequestEvent(reqId, cfg->registryId, cfg->rlnIdentifier, now);
        } else if (nameId == kGetQuota) {
            rlnBridge->getEpochQuota(callId, cfg->registryId, cfg->rlnIdentifier, ts());
            dispatchRlnGetEpochQuotaRequestEvent(reqId, cfg->registryId, cfg->rlnIdentifier,
                                                 static_cast<int64_t>(ts()), now);
        } else if (nameId == kGenerate) {
            rlnBridge->generateProof(callId, cfg->registryId, cfg->rlnIdentifier, str("signalHex"), ts());
            dispatchRlnGenerateProofRequestEvent(reqId, cfg->registryId, cfg->rlnIdentifier,
                                                 str("signalHex"), static_cast<int64_t>(ts()), now);
        } else if (nameId == kValidate) {
            rlnBridge->validateProof(callId, cfg->registryId, cfg->rlnIdentifier, str("signalHex"),
                                     ts(), str("proofJson"));
            dispatchRlnValidateProofRequestEvent(reqId, cfg->registryId, cfg->rlnIdentifier,
                                                 str("signalHex"), static_cast<int64_t>(ts()),
                                                 str("proofJson"), now);
        } else {
            // Never leave the library waiting on a question nobody answers.
            host->reverseReply(callId, NIMFFI_RET_ERR, std::string("unknown rln question"));
        }
    } catch (const std::exception& e) {
        host->reverseReply(callId, NIMFFI_RET_ERR, std::string("rln question failed: ") + e.what());
    }
}

static std::string toLowerCopy(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Case-insensitive key lookup, matching keys the same way as the upstream
// conf parser. Returns the key as spelled in the config.
static std::optional<std::string> findKey(const nlohmann::json& cfgObj,
                                          std::initializer_list<const char*> names)
{
    for (const auto& entry : cfgObj.items()) {
        const std::string key = toLowerCopy(entry.key());
        for (const char* name : names) {
            if (key == name) return entry.key();
        }
    }
    return std::nullopt;
}

// True when the config is the legacy flat shape: any top-level key besides the
// ones the layered parser consumes marks a bare WakuNodeConf field.
static bool isFlatShape(const nlohmann::json& cfgObj)
{
    for (const auto& entry : cfgObj.items()) {
        const std::string key = toLowerCopy(entry.key());
        if (key != "entrylayer" && key != "mode" && key != "preset"
            && key != "kernelconf" && key != "messagingoverrides"
            && key != "channelsoverrides") {
            return true;
        }
    }
    return false;
}

// Locates the object that carries kernel/messaging settings for this config
// shape: kernelConf when present, messagingOverrides for the layered shapes,
// top level for the legacy flat shape.
static nlohmann::json* configTarget(nlohmann::json& cfgObj)
{
    const auto entryLayerKey = findKey(cfgObj, {"entrylayer"});
    const bool kernelEntry = entryLayerKey && cfgObj[*entryLayerKey].is_string()
        && toLowerCopy(cfgObj[*entryLayerKey].get<std::string>()) == "kernel";
    if (auto kernelConfKey = findKey(cfgObj, {"kernelconf"});
        kernelConfKey && cfgObj[*kernelConfKey].is_object()) {
        return &cfgObj[*kernelConfKey];
    }
    if (kernelEntry) {
        return nullptr;
    }
    if (isFlatShape(cfgObj)) {
        return &cfgObj;
    }
    auto overridesKey = findKey(cfgObj, {"messagingoverrides"});
    if (!overridesKey) {
        return nullptr;
    }
    return cfgObj[*overridesKey].is_object() ? &cfgObj[*overridesKey] : nullptr;
}

// Defaults the node's storage directory to the host's per-instance path, so
// side-by-side instances don't share upstream's cwd-relative "./data". Also
// carries the preset's validation switch into the config as
// rln-disable-validation: deployment policy, so it overrides any client key.
// Each setting goes where the config shape accepts it: kernelConf when
// present, messagingOverrides (created if needed) for the layered shapes,
// top level for the legacy flat shape.
static std::optional<std::string> applyConfigDefaults(const std::string& cfg,
                                                      const std::string& persistencePath,
                                                      bool disableRlnValidation)
{
    nlohmann::json cfgObj;
    try {
        cfgObj = nlohmann::json::parse(cfg);
    } catch (const nlohmann::json::parse_error&) {
        fprintf(stderr, "DeliveryModuleImpl: createNode cfg is not valid JSON\n");
        return std::nullopt;
    }

    if (!cfgObj.is_object()) {
        fprintf(stderr, "DeliveryModuleImpl: createNode cfg is not a JSON object\n");
        return std::nullopt;
    }

    if (!persistencePath.empty() || disableRlnValidation) {
        nlohmann::json* target = &cfgObj;
        const auto entryLayerKey = findKey(cfgObj, {"entrylayer"});
        const bool kernelEntry = entryLayerKey && cfgObj[*entryLayerKey].is_string()
            && toLowerCopy(cfgObj[*entryLayerKey].get<std::string>()) == "kernel";
        if (auto kernelConfKey = findKey(cfgObj, {"kernelconf"});
            kernelConfKey && cfgObj[*kernelConfKey].is_object()) {
            target = &cfgObj[*kernelConfKey];
        } else if (kernelEntry) {
            // Kernel entry without a kernelConf object: leave the config
            // untouched for the parser to reject.
            target = nullptr;
        } else if (!isFlatShape(cfgObj)) {
            auto overridesKey = findKey(cfgObj, {"messagingoverrides"});
            if (!overridesKey) {
                cfgObj["messagingOverrides"] = nlohmann::json::object();
                overridesKey = "messagingOverrides";
            }
            target = cfgObj[*overridesKey].is_object() ? &cfgObj[*overridesKey] : nullptr;
        }
        if (target && !persistencePath.empty()
            && !findKey(*target, {"localstoragepath", "local-storage-path"})) {
            (*target)["localStoragePath"] = persistencePath + "/data";
        }
        if (target && disableRlnValidation) {
            (*target)["rln-disable-validation"] = true;
        }
    }

    return cfgObj.dump();
}

StdLogosResult DeliveryModuleImpl::createNode(const std::string& cfg)
{
    std::lock_guard<std::mutex> createNodeLock(createNodeMutex);

    if (host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: createNode rejected - context already initialized\n");
        return {false, {}, "Context already initialized"};
    }

    // Don't log cfg: it can carry sensitive config.
    fprintf(stderr, "DeliveryModuleImpl::createNode called\n");

    std::string presetName;
    {
        nlohmann::json cfgObj = nlohmann::json::parse(cfg, nullptr, /*allow_exceptions=*/false);
        if (cfgObj.is_object()) {
            if (auto k = findKey(cfgObj, {"preset"}); k && cfgObj[*k].is_string()) {
                presetName = cfgObj[*k].get<std::string>();
            }
        }
    }

    // A named presets file that cannot be used is fatal: the caller asked for
    // a deployment this node cannot reproduce, and coming up without RLN
    // would look like success.
    std::string presetError;
    const RlnPresetEntry rlnPreset = resolveRlnPreset(presetName, presetError);
    if (!presetError.empty()) {
        return {false, {}, presetError};
    }

    auto cfgWithDefaults = applyConfigDefaults(cfg, instancePersistencePath(),
                                               rlnPreset.enabled && !rlnPreset.enableValidation);
    if (!cfgWithDefaults) {
        return {false, {}, "Invalid JSON config"};
    }
    const std::string& cfgWithPorts = *cfgWithDefaults;

    joinRlnBringUp();

    if (rlnPreset.enabled) {
        DeliveryRlnConfig fromPreset;
        fromPreset.registryId = rlnPreset.registryId;
        fromPreset.rlnIdentifier = rlnPreset.rlnIdentifier;
        fromPreset.epochSizeSec = rlnPreset.epochSizeSec;
        fromPreset.maxEpochGap = rlnPreset.maxEpochGap;
        if (std::string failure = installRlnPlugin(fromPreset); !failure.empty()) {
            return {false, {}, failure};
        }
        setRlnState("Initializing", {});
    }

    // logosdelivery_create_node encodes the config and turns the context address
    // the FFI reports back into a LogosDeliveryCtx handle.
    // `rlnPlugin` tells the library this module answers its RLN questions.
    const nim_ffi::Reply ready = host->create(
        encode(nlohmann::json{{"configJson", cfgWithPorts}, {"rlnPlugin", rlnPreset.enabled}}), CALLBACK_TIMEOUT);
    if (ready.ret != NIMFFI_RET_OK) {
        fprintf(stderr, "DeliveryModuleImpl: create_node: %s\n", ready.error.c_str());
        return releaseAndFail("Failed to create Delivery context: " + ready.error);
    }
#ifdef DELIVERY_HAS_QT
    // Between calls, the Qt loop drains the library's queue as it fills.
    if (const int fd = host->fd(); fd >= 0) {
        auto notifier = std::make_shared<QSocketNotifier>(fd, QSocketNotifier::Read);
        QObject::connect(notifier.get(), &QSocketNotifier::activated, [this](int) { host->drain(); });
        pollNotifier = notifier;
    }
#endif
    if (rlnPreset.enabled) {
        rlnBringUpDone.store(false, std::memory_order_release);
        rlnBringUpThread = std::thread([this] {
            const std::string failure = startRlnBackend();
            if (failure.empty()) {
                setRlnState("Ready", {});
            } else {
                setRlnState("Failed", failure);
            }
            rlnBringUpDone.store(true, std::memory_order_release);
        });
    }

    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::start()
{
    fprintf(stderr, "DeliveryModuleImpl::start called\n");

    reapRlnBringUp();
    if (!host->alive()) {
        return {false, {}, "Context not initialized"};
    }

    // Node start can block for a long time (relay reconnect backoff), so return
    // once dispatched. Completion arrives via nodeStarted.
    reapRlnBringUp();
    // start's outcome is its reply, reported as nodeStarted when pumped.
    const int rc = host->submit(logosdelivery_start_node, encode(nlohmann::json::object()),
        [this](const nim_ffi::Reply& r) {
            nodeStarted(r.ret == NIMFFI_RET_OK, r.ret == NIMFFI_RET_OK ? std::string() : r.error, currentTimestampNs());
        });
    if (rc != NIMFFI_RET_OK) {
        return {false, {}, "failed to initiate start"};
    }
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::stop()
{
    fprintf(stderr, "DeliveryModuleImpl::stop called\n");

    reapRlnBringUp();
    if (!host->alive()) {
        return {false, {}, "Context not initialized"};
    }

    const int rc = host->submit(logosdelivery_stop_node, encode(nlohmann::json::object()),
        [this](const nim_ffi::Reply& r) {
            nodeStopped(r.ret == NIMFFI_RET_OK, r.ret == NIMFFI_RET_OK ? std::string() : r.error, currentTimestampNs());
        });
    if (rc != NIMFFI_RET_OK) {
        return {false, {}, "failed to initiate stop"};
    }

    // This module started the RLN backend, so it stops it too. Stopping one
    // that is still starting would race the bring-up thread.
    joinRlnBringUp();
    if (rlnConfig->enabled) {
        const std::string failure = rlnBridge->stopBackend();
        if (!failure.empty()) {
            fprintf(stderr, "DeliveryModuleImpl: rln module stop failed: %s\n",
                    failure.c_str());
        }
    }
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::send(const std::string& contentTopic, const std::vector<uint8_t>& payload)
{
    fprintf(stderr, "DeliveryModuleImpl::send called with contentTopic: %s\n", contentTopic.c_str());

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot send message - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    nlohmann::json messageObj;
    messageObj["contentTopic"] = contentTopic;
    messageObj["payload"] = base64Encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = callApi(*host, "send", logosdelivery_send, nlohmann::json{{"messageJson", messageJson}}, CALLBACK_TIMEOUT, true);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Send failed for topic: %s, reason: %s\n",
                contentTopic.c_str(), outcome.error.c_str());
    }

    if (outcome.success && outcome.value.is_string()) {
        fprintf(stderr, "DeliveryModuleImpl: Send initiated for topic: %s, with success, requestId: %s\n",
                contentTopic.c_str(), outcome.value.get<std::string>().c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::subscribe(const std::string& contentTopic)
{
    fprintf(stderr, "DeliveryModuleImpl::subscribe called with contentTopic: %s\n", contentTopic.c_str());

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot subscribe - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApi(*host, "subscribe", logosdelivery_subscribe, nlohmann::json{{"contentTopicStr", contentTopic}}, CALLBACK_TIMEOUT, false);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Subscribe failed for topic: %s, reason: %s\n",
                contentTopic.c_str(), outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Subscribe completed for topic: %s with success\n", contentTopic.c_str());
    return outcome;
}

StdLogosResult DeliveryModuleImpl::unsubscribe(const std::string& contentTopic)
{
    fprintf(stderr, "DeliveryModuleImpl::unsubscribe called with contentTopic: %s\n", contentTopic.c_str());

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot unsubscribe - context not initialized.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApi(*host, "unsubscribe", logosdelivery_unsubscribe, nlohmann::json{{"contentTopicStr", contentTopic}}, CALLBACK_TIMEOUT, false);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Unsubscribe failed for topic: %s, reason: %s\n",
                contentTopic.c_str(), outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Unsubscribe completed for topic: %s with success\n", contentTopic.c_str());
    return outcome;
}

StdLogosResult DeliveryModuleImpl::storeQuery(const std::string& jsonQuery,
                                              const std::string& peerAddr,
                                              int64_t timeoutMs)
{
    fprintf(stderr, "DeliveryModuleImpl::storeQuery called with peerAddr: %s\n", peerAddr.c_str());

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot run store query - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    // timeoutMs bounds the query on the FFI side; wait longer than that for the
    // completion callback so the query's own timeout error reaches the caller
    // instead of a callback timeout.
    auto callbackTimeout = std::max(
        CALLBACK_TIMEOUT, std::chrono::seconds(timeoutMs / 1000 + 5));

    auto outcome = callApi(*host, "store_query", waku_store_query, nlohmann::json{{"jsonQuery", jsonQuery}, {"peerAddr", peerAddr}, {"timeoutMs", static_cast<int32_t>(timeoutMs)}}, callbackTimeout, true);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Store query failed for peer: %s, reason: %s\n",
                peerAddr.c_str(), outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::channelCreate(const std::string& channelId,
                                                 const std::string& contentTopic,
                                                 const std::string& senderId)
{
    fprintf(stderr, "DeliveryModuleImpl::channelCreate called with channelId: %s, contentTopic: %s\n",
            channelId.c_str(), contentTopic.c_str());

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot create channel - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApi(*host, "channel_create", logosdelivery_channel_create, nlohmann::json{{"channelIdStr", channelId}, {"contentTopicStr", contentTopic}, {"senderIdStr", senderId}, {"encryptFn", uint64_t{0}}, {"decryptFn", uint64_t{0}}, {"userData", uint64_t{0}}}, CALLBACK_TIMEOUT, true);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel create failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::channelExists(const std::string& channelId)
{
    fprintf(stderr, "DeliveryModuleImpl::channelExists called with channelId: %s\n", channelId.c_str());

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot query channel - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApi(*host, "channel_exists", logosdelivery_channel_exists, nlohmann::json{{"channelIdStr", channelId}}, CALLBACK_TIMEOUT, true);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel exists failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::channelSend(const std::string& channelId, const std::vector<uint8_t>& payload)
{
    fprintf(stderr, "DeliveryModuleImpl::channelSend called with channelId: %s\n", channelId.c_str());

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot send channel message - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    nlohmann::json messageObj;
    messageObj["payload"] = base64Encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = callApi(*host, "channel_send", logosdelivery_channel_send, nlohmann::json{{"channelIdStr", channelId}, {"messageJson", messageJson}}, CALLBACK_TIMEOUT, true);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel send failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }

    if (outcome.success && outcome.value.is_string()) {
        fprintf(stderr, "DeliveryModuleImpl: Channel send initiated for id: %s, with success, requestId: %s\n",
                channelId.c_str(), outcome.value.get<std::string>().c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::channelClose(const std::string& channelId)
{
    fprintf(stderr, "DeliveryModuleImpl::channelClose called with channelId: %s\n", channelId.c_str());

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot close channel - context not initialized.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApi(*host, "channel_close", logosdelivery_channel_close, nlohmann::json{{"channelIdStr", channelId}}, CALLBACK_TIMEOUT, false);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel close failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getAvailableNodeInfoIDs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableNodeInfoIDs called\n");

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available node info IDs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApi(*host, "get_available_node_info_ids", logosdelivery_get_available_node_info_ids, nlohmann::json::object(), CALLBACK_TIMEOUT, true);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available node info IDs failed, reason: %s\n", outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getNodeInfo(const std::string& nodeInfoId) {
    fprintf(stderr, "DeliveryModuleImpl::getNodeInfo called with nodeInfoId: %s\n", nodeInfoId.c_str());

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get node info - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApi(*host, "get_node_info", logosdelivery_get_node_info, nlohmann::json{{"nodeInfoId", nodeInfoId}}, CALLBACK_TIMEOUT, true);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get node info failed for ID: %s, reason: %s\n",
                nodeInfoId.c_str(), outcome.error.c_str());
    }

    return outcome;
}

StdLogosResult DeliveryModuleImpl::getAvailableConfigs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableConfigs called\n");

    reapRlnBringUp();
    if (!host->alive()) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available configs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApi(*host, "get_available_configs", logosdelivery_get_available_configs, nlohmann::json::object(), CALLBACK_TIMEOUT, true);

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available configs failed, reason: %s\n", outcome.error.c_str());
    }

    return outcome;
}

std::string DeliveryModuleImpl::collectOpenMetricsText()
{
    reapRlnBringUp();
    if (!host->alive()) {
        // No node yet — empty document; the openmetrics scraper renders nothing
        // for this module rather than treating the scrape as a hard error.
        return "";
    }

    auto outcome = callApi(*host, "get_node_info", logosdelivery_get_node_info, nlohmann::json{{"nodeInfoId", "Metrics"}}, CALLBACK_TIMEOUT, true);

    if (!outcome.success || !outcome.value.is_string()) {
        fprintf(stderr, "DeliveryModuleImpl: collectOpenMetricsText failed to read Metrics node info: %s\n",
                outcome.error.c_str());
        return "";
    }

    // Hand the exposition text back verbatim; the openmetrics module parses it,
    // injects the module="delivery_module" label, and merges it with others.
    return outcome.value.get<std::string>();
}

StdLogosResult DeliveryModuleImpl::rlnState()
{
    reapRlnBringUp();
    std::lock_guard<std::mutex> lock(rlnStateMutex);
    nlohmann::json out{{"state", rlnStateName}, {"message", rlnStateMessage}};
    if (rlnStateConfig.enabled) {
        out["registryId"] = rlnStateConfig.registryId;
        out["rlnIdentifier"] = rlnStateConfig.rlnIdentifier;
        out["epochSizeSec"] = rlnStateConfig.epochSizeSec;
    }
    return {true, std::move(out)};
}

void DeliveryModuleImpl::setRlnState(const char* state, const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(rlnStateMutex);
        if (rlnStateName == state && rlnStateMessage == message) {
            return;
        }
        rlnStateName = state;
        rlnStateMessage = message;
    }
    fprintf(stderr, "DeliveryModuleImpl: rln %s%s%s\n", state,
            message.empty() ? "" : ": ", message.c_str());
    rlnStateChanged(state, message, currentTimestampNs());
}

std::string DeliveryModuleImpl::installRlnPlugin(const DeliveryRlnConfig& cfg)
{
    // The setter is process-global: one delivery module instance per process.
    auto next = std::make_shared<DeliveryRlnConfig>(cfg);
    next->enabled = true;
    {
        std::lock_guard<std::mutex> lock(rlnConfigMutex);
        rlnConfig = std::move(next);
    }
    // The setter is no nim-ffi entry point, so it does not bring the Nim runtime
    // up; before that its lock is uninitialized (fatal on Windows). This call does.
    {
        std::lock_guard<std::mutex> lock(rlnStateMutex);
        rlnStateConfig = *rlnConfig;
    }
    return {};
}

std::string DeliveryModuleImpl::startRlnBackend()
{
    // The in-process bridge is one way to answer; the rln*Request events plus
    // rlnRespond are the other, so a bridge that cannot come up is not fatal.
    // Only a bridge that IS up starts the backend, because only it can reach
    // the RLN module.
    const std::string failure = bringUpRlnBridge();
    if (!failure.empty()) {
        return "rln bridge unavailable (" + failure + "); answering falls to rlnRespond";
    }

    // The delivery library no longer starts the backend, so this module does:
    // a node that mounts RLN over a stopped module would Ignore every inbound
    // RLN message.
    nlohmann::json startCfg{{"registries", nlohmann::json::array({rlnConfig->registryId})}};
    if (rlnConfig->epochSizeSec != 0) {
        startCfg["epoch_size_sec"] = rlnConfig->epochSizeSec;
    }
    if (rlnConfig->maxEpochGap != 0) {
        startCfg["max_epoch_gap"] = rlnConfig->maxEpochGap;
    }
    const std::string startFailure = rlnBridge->startBackend(startCfg.dump());
    if (!startFailure.empty()) {
        return "rln module start failed: " + startFailure;
    }
    return {};
}

void DeliveryModuleImpl::reapRlnBringUp()
{
    if (rlnBringUpThread.joinable() && rlnBringUpDone.load(std::memory_order_acquire)) {
        rlnBringUpThread.join();
    }
}

void DeliveryModuleImpl::joinRlnBringUp()
{
    if (rlnBringUpThread.joinable()) {
        rlnBringUpThread.join();
    }
}

