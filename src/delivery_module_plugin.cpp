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
#include <semaphore>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include "base64.h"

#include "api_call_handler.h"
#include "discovery_config.h"
#include "service_discovery_plugin.h"
#include "rln_bridge.h"
#include "rln_presets.h"

// Generated at build time from metadata.json#dependencies and
// #optional_dependencies; defines the
// LogosModules aggregate behind LogosModuleContext::modules().
#include "logos_sdk.h"
extern "C" {
#include <liblogosdelivery.h>
// Kernel tier: unstable, may change without a deprecation cycle. Only
// waku_store_query is consumed from it; everything else goes through the
// stable surface above.
#include <liblogosdelivery_kernel.h>
#include <liblogosdelivery_rln.h>
}

namespace {
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
    return toStringOrEmpty(errCode == RET_OK ? (reply ? *reply : nullptr) : errMsg);
}

const LogosDeliveryCtx* asCtx(void* handle) {
    return static_cast<const LogosDeliveryCtx*>(handle);
}

// message_received and channel_message_received: base64 string.
std::vector<uint8_t> decodeBase64Payload(const nlohmann::json& payloadValue) {
    if (!payloadValue.is_string()) {
        return {};
    }
    return delivery_base64::decode(payloadValue.get<std::string>());
}

// Wire names of the events this module forwards. nim-ffi 0.3.0 replaced the
// single global event callback with a per-event listener registry, so each name
// is registered separately; the JSON payload still carries the snake_case
// "eventType" that event_callback dispatches on. Upstream also emits
// onTopicHealthChange, onConnectionChange and onReceivedMessage, which the
// module does not surface.
constexpr const char* kEventNames[] = {
    "onMessageQueued",
    "onMessageSent",
    "onMessageError",
    "onMessagePropagated",
    "onMessageReceived",
    "onConnectionStatusChange",
    "onChannelMessageReceived",
    "onChannelMessageSent",
    "onChannelMessageError",
};
} // namespace

void DeliveryModuleImpl::start_callback(int errCode, const char* const* reply,
                                        const char* errMsg, void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::start_callback: Invalid userData\n");
        return;
    }
    impl->nodeStarted(errCode == RET_OK, replyText(errCode, reply, errMsg),
                      currentTimestampNs());
    if (errCode != RET_OK) {
        impl->stopAfterFailedStart();
    }
}

void DeliveryModuleImpl::stopAfterFailedStart()
{
    std::lock_guard<std::mutex> lock(failedStartStopMutex);
    if (!failedStartStopArmed) {
        return;
    }
    failedStartStopArmed = false;
    // The same two halves as stop(): the node, then the RLN backend this
    // module started for it.
    failedStartStopThread = std::thread([this] {
        if (logosdelivery_ctx_stop_node(asCtx(deliveryCtxHandle), stop_callback, this) != RET_OK) {
            fprintf(stderr, "DeliveryModuleImpl: failed to stop the node after a failed start\n");
            return;
        }
        stopRlnBackend();
    });
}

void DeliveryModuleImpl::joinFailedStartStop()
{
    {
        std::lock_guard<std::mutex> lock(failedStartStopMutex);
        failedStartStopArmed = false;
    }
    // Disarmed under the lock, so no thread can be assigned after this point.
    if (failedStartStopThread.joinable()) {
        failedStartStopThread.join();
    }
}

void DeliveryModuleImpl::stop_callback(int errCode, const char* const* reply,
                                       const char* errMsg, void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::stop_callback: Invalid userData\n");
        return;
    }
    impl->nodeStopped(errCode == RET_OK, replyText(errCode, reply, errMsg),
                      currentTimestampNs());
}

// The rln_*_callback trampolines are C callbacks invoked from the Nim runtime,
// possibly on a foreign thread: a C++ exception escaping them would unwind
// into Nim frames and terminate the process, so each body is fenced with
// catch (...). String arguments are borrowed and copied immediately;
// options/proof JSON is opaque (RLN module's wire schema) and passed through
// verbatim, never parsed here. Responses come back later via rlnRespond;
// response timeouts are the library's job.

void DeliveryModuleImpl::rln_get_membership_state_callback(uint64_t reqId, void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::rln_get_membership_state_callback: Invalid userData\n");
        return;
    }
    try {
        const auto cfg = impl->rlnConfigSnapshot();
        impl->rlnBridge->getMembershipState(reqId, cfg->registryId,
                                            cfg->rlnIdentifier);
        impl->dispatchRlnGetMembershipStateRequestEvent(static_cast<int64_t>(reqId),
                                             cfg->registryId,
                                             cfg->rlnIdentifier,
                                             currentTimestampNs());
    } catch (const std::exception& e) {
        fprintf(stderr, "DeliveryModuleImpl: dropped RLN get_membership_state request %llu: %s\n",
                static_cast<unsigned long long>(reqId), e.what());
    } catch (...) {
        fprintf(stderr, "DeliveryModuleImpl: dropped RLN get_membership_state request %llu\n",
                static_cast<unsigned long long>(reqId));
    }
}

void DeliveryModuleImpl::rln_get_epoch_quota_callback(uint64_t reqId, uint64_t timestamp,
                                                      void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::rln_get_epoch_quota_callback: Invalid userData\n");
        return;
    }
    try {
        const auto cfg = impl->rlnConfigSnapshot();
        impl->rlnBridge->getEpochQuota(reqId, cfg->registryId,
                                       cfg->rlnIdentifier, timestamp);
        impl->dispatchRlnGetEpochQuotaRequestEvent(static_cast<int64_t>(reqId),
                                      cfg->registryId,
                                      cfg->rlnIdentifier,
                                      static_cast<int64_t>(timestamp), currentTimestampNs());
    } catch (const std::exception& e) {
        fprintf(stderr, "DeliveryModuleImpl: dropped RLN get_epoch_quota request %llu: %s\n",
                static_cast<unsigned long long>(reqId), e.what());
    } catch (...) {
        fprintf(stderr, "DeliveryModuleImpl: dropped RLN get_epoch_quota request %llu\n",
                static_cast<unsigned long long>(reqId));
    }
}

void DeliveryModuleImpl::rln_generate_proof_callback(uint64_t reqId, const char* signalHex,
                                                     uint64_t timestamp, void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::rln_generate_proof_callback: Invalid userData\n");
        return;
    }
    try {
        const auto cfg = impl->rlnConfigSnapshot();
        impl->rlnBridge->generateProof(reqId, cfg->registryId,
                                       cfg->rlnIdentifier,
                                       toStringOrEmpty(signalHex), timestamp);
        impl->dispatchRlnGenerateProofRequestEvent(static_cast<int64_t>(reqId),
                                      cfg->registryId,
                                      cfg->rlnIdentifier,
                                      toStringOrEmpty(signalHex),
                                      static_cast<int64_t>(timestamp), currentTimestampNs());
    } catch (const std::exception& e) {
        fprintf(stderr, "DeliveryModuleImpl: dropped RLN generate_proof request %llu: %s\n",
                static_cast<unsigned long long>(reqId), e.what());
    } catch (...) {
        fprintf(stderr, "DeliveryModuleImpl: dropped RLN generate_proof request %llu\n",
                static_cast<unsigned long long>(reqId));
    }
}

void DeliveryModuleImpl::rln_validate_proof_callback(uint64_t reqId, const char* signalHex,
                                                     uint64_t timestamp, const char* proofJson,
                                                     void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::rln_validate_proof_callback: Invalid userData\n");
        return;
    }
    try {
        const auto cfg = impl->rlnConfigSnapshot();
        impl->rlnBridge->validateProof(reqId, cfg->registryId,
                                       cfg->rlnIdentifier,
                                       toStringOrEmpty(signalHex), timestamp,
                                       toStringOrEmpty(proofJson));
        impl->dispatchRlnValidateProofRequestEvent(static_cast<int64_t>(reqId),
                                      cfg->registryId,
                                      cfg->rlnIdentifier,
                                      toStringOrEmpty(signalHex),
                                      static_cast<int64_t>(timestamp),
                                      toStringOrEmpty(proofJson), currentTimestampNs());
    } catch (const std::exception& e) {
        fprintf(stderr, "DeliveryModuleImpl: dropped RLN validate_proof request %llu: %s\n",
                static_cast<unsigned long long>(reqId), e.what());
    } catch (...) {
        fprintf(stderr, "DeliveryModuleImpl: dropped RLN validate_proof request %llu\n",
                static_cast<unsigned long long>(reqId));
    }
}

DeliveryModuleImpl::DeliveryModuleImpl()
    : rlnBridge(std::make_unique<RlnBridge>())
    , deliveryCtx(nullptr)
    , deliveryCtxHandle(nullptr)
{
    fprintf(stderr, "DeliveryModuleImpl: Initializing...\n");
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

void DeliveryModuleImpl::releaseServiceDiscoveryPlugin()
{
    if (!discoPlugin) {
        return;
    }
    // A timed-out call may leave a thread inside the plugin; leak the object
    // rather than free it under that thread.
    if (discoPlugin->quiesce(kQuiesceTimeout)) {
        discoPlugin.reset();
        return;
    }
    fprintf(stderr,
            "DeliveryModuleImpl: service discovery plugin still in use; "
            "leaking it rather than freeing it under a live thread\n");
    (void)discoPlugin.release();
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
    logosdelivery_rln_set_plugin(nullptr, nullptr);
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
    // A stop issued after a failed start holds the context handle, and joins
    // the bring-up thread itself: finish it first, so that join is not racing
    // the one below.
    joinFailedStartStop();
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

    if (deliveryCtxHandle) {
        // Frees the handle and stops the node, tearing down the event
        // listeners registered against it along the way.
        logosdelivery_ctx_destroy(static_cast<LogosDeliveryCtx*>(deliveryCtxHandle));
        deliveryCtxHandle = nullptr;
        deliveryCtx = nullptr;
    }

    // After ctx_destroy, unlike RLN above: destroying the node joins the
    // discovery worker, the only caller of the plugin, so no new call can
    // arrive once it returns.
    releaseServiceDiscoveryPlugin();
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

void DeliveryModuleImpl::event_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    (void)callerRet;
    DeliveryModuleImpl* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid userData\n");
        return;
    }

    if (msg && len > 0) {
        std::string message(msg, len);

        // This function is a C callback invoked from the Nim runtime: a C++
        // exception escaping here would unwind into Nim frames and terminate
        // the process. Catch the whole nlohmann exception hierarchy (parse
        // errors and type mismatches from .value()/.get()) and drop the event.
        try {
            nlohmann::json jsonObj = nlohmann::json::parse(message);

            if (!jsonObj.is_object()) {
                fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid JSON\n");
                return;
            }

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
            fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid event JSON: %s\n", e.what());
        }
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

    if (deliveryCtx != nullptr) {
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

    // logosdelivery_ctx_create encodes the config and turns the context address
    // the FFI reports back into a LogosDeliveryCtx handle.
    struct CreateContext {
        std::binary_semaphore sem{0};
        int callerRet{RET_ERR};
        std::string message;
        LogosDeliveryCtx* ctx{nullptr};
    };

    static std::mutex pendingMutex;
    static std::unordered_map<void*, std::shared_ptr<CreateContext>> pendingContexts;

    // Keyed by a counter, not the context's address: a createNode that timed
    // out leaves its key behind, and a retry allocating its CreateContext at
    // the recycled address would let that late reply wake the retry and hand
    // it the abandoned node. Same reasoning as the ticket in api_call_handler.h.
    static std::atomic<uintptr_t> createTicket{0};

    auto callbackCtx = std::make_shared<CreateContext>();
    void* callbackKey = reinterpret_cast<void*>(++createTicket);

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts[callbackKey] = callbackCtx;
    }

    auto callback = +[](int errCode, LogosDeliveryCtx* ctx, const char* errMsg, void* userData) {
        fprintf(stderr, "DeliveryModuleImpl::createNode callback called with ret: %d\n", errCode);

        std::shared_ptr<CreateContext> callbackCtx;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            auto it = pendingContexts.find(userData);
            if (it == pendingContexts.end()) {
                // createNode already gave up waiting. Destroy the node we were
                // handed rather than leaving it running with no owner.
                if (ctx) {
                    logosdelivery_ctx_destroy(ctx);
                }
                return;
            }
            callbackCtx = it->second;
            pendingContexts.erase(it);
        }

        if (!callbackCtx) {
            return;
        }

        callbackCtx->callerRet = errCode;
        callbackCtx->ctx = ctx;
        if (errCode != RET_OK && errMsg) {
            callbackCtx->message = errMsg;
            fprintf(stderr, "DeliveryModuleImpl::createNode callback message: %s\n", errMsg);
        }

        callbackCtx->sem.release();
    };

    if (logosdelivery_ctx_create(cfgWithPorts.c_str(), callback, callbackKey) != RET_OK) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);

        fprintf(stderr, "DeliveryModuleImpl: Failed to initiate createNode\n");
        return releaseAndFail("Failed to initiate createNode");
    }

    fprintf(stderr, "DeliveryModuleImpl: Waiting for createNode callback...\n");

    if (!callbackCtx->sem.try_acquire_for(CALLBACK_TIMEOUT)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);

        fprintf(stderr, "DeliveryModuleImpl: Timeout waiting for createNode callback\n");
        return releaseAndFail("Timeout waiting for createNode callback");
    }

    if (callbackCtx->callerRet != RET_OK || callbackCtx->ctx == nullptr
        || callbackCtx->ctx->ptr == nullptr) {
        if (!callbackCtx->message.empty()) {
            fprintf(stderr, "DeliveryModuleImpl: createNode callback error: %s\n", callbackCtx->message.c_str());
        }
        // A handle carrying a null context is still a handle: free it.
        if (callbackCtx->ctx) {
            logosdelivery_ctx_destroy(callbackCtx->ctx);
        }

        fprintf(stderr, "DeliveryModuleImpl: Failed to create Delivery context\n");
        return releaseAndFail("Failed to create Delivery context");
    }

    deliveryCtxHandle = callbackCtx->ctx;
    deliveryCtx = callbackCtx->ctx->ptr;

    fprintf(stderr, "DeliveryModuleImpl: Delivery context created successfully\n");

    for (const char* eventName : kEventNames) {
        if (logosdelivery_add_event_listener(deliveryCtx, eventName, event_callback, this) == 0) {
            fprintf(stderr, "DeliveryModuleImpl: Failed to register listener for event %s\n", eventName);
        }
    }

    // Only the node knows whether it wants a discovery plugin and which DHT
    // peers its config resolved; ask it.
    const StdLogosResult result = callApiRetValue(
        "get_discovery_requirements", CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_get_discovery_requirements, asCtx(deliveryCtxHandle)));
    delivery_discovery::PluginRequest discovery;
    std::string failure;
    if (!result.success) {
        failure = "discovery requirements: " + result.error;
    } else {
        const std::string requirements = result.value.is_string()
            ? result.value.get<std::string>()
            : result.value.dump();
        failure = delivery_discovery::fromRequirements(
            requirements, delivery_discovery::libp2pEnvConfig(), discovery);
    }
    if (failure.empty() && discovery.enabled) {
        failure = installServiceDiscoveryPlugin(discovery.libp2pConfig);
    }
    if (!failure.empty()) {
        // A node configured for plugin discovery cannot start without a
        // registered plugin, so a half-built context is worse than none:
        // unwind it and report, rather than failing later at start().
        return releaseAndFail("service discovery setup failed: " + failure);
    }

    if (rlnPreset.enabled) {
        rlnBringUpThread = std::thread([this] {
            const std::string failure = startRlnBackend();
            if (failure.empty()) {
                setRlnState("Ready", {});
            } else {
                setRlnState("Failed", failure);
            }
        });
    }

    return {true, {}};
}

std::string DeliveryModuleImpl::installServiceDiscoveryPlugin(const std::string& libp2pConfig)
{
    if (!deliveryCtx) {
        return "context not initialized";
    }

    // libp2p is not contacted here: outbound calls fail on the Qt main thread.
    // Without a framework (unit tests) there is no modules(); pass null.
    Libp2pModule* libp2p = isContextReady() ? &modules().libp2p_module : nullptr;
    discoPlugin = std::make_unique<DeliveryServiceDiscoveryPlugin>(libp2p, libp2pConfig);

    // The vtable is borrowed for the duration of the call and copied by the
    // node, but discoPlugin owns the object every entry point dispatches on,
    // so it must outlive the context -- hence a member, not a local.
    const StdLogosResult installed = callApiRetVoid(
        "install service discovery plugin", CALLBACK_TIMEOUT,
        [this](void* ticket) {
            return logosdelivery_install_service_discovery_plugin(
                static_cast<const LogosDeliveryCtx*>(deliveryCtxHandle),
                discoPlugin->vtable(),
                replyTrampoline,
                ticket);
        });

    if (!installed.success) {
        return installed.error;
    }

    fprintf(stderr, "DeliveryModuleImpl: service discovery plugin installed\n");
    return {};
}

StdLogosResult DeliveryModuleImpl::start()
{
    fprintf(stderr, "DeliveryModuleImpl::start called\n");

    if (!deliveryCtx) {
        return {false, {}, "Context not initialized"};
    }

    joinFailedStartStop();
    {
        std::lock_guard<std::mutex> lock(failedStartStopMutex);
        failedStartStopArmed = true;
    }

    // Node start can block for a long time (relay reconnect backoff), so return
    // once dispatched. Completion arrives via nodeStarted.
    if (logosdelivery_ctx_start_node(asCtx(deliveryCtxHandle), start_callback, this) != RET_OK) {
        joinFailedStartStop();
        return {false, {}, "failed to initiate start"};
    }
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::stop()
{
    fprintf(stderr, "DeliveryModuleImpl::stop called\n");

    if (!deliveryCtx) {
        return {false, {}, "Context not initialized"};
    }

    if (logosdelivery_ctx_stop_node(asCtx(deliveryCtxHandle), stop_callback, this) != RET_OK) {
        return {false, {}, "failed to initiate stop"};
    }
    stopRlnBackend();
    return {true, {}};
}

void DeliveryModuleImpl::stopRlnBackend()
{
    std::lock_guard<std::mutex> lock(rlnBackendStopMutex);
    // This module started the RLN backend, so it stops it too. Stopping one
    // that is still starting would race the bring-up thread.
    joinRlnBringUp();
    if (rlnConfigSnapshot()->enabled) {
        const std::string failure = rlnBridge->stopBackend();
        if (!failure.empty()) {
            fprintf(stderr, "DeliveryModuleImpl: rln module stop failed: %s\n",
                    failure.c_str());
        }
    }
}

StdLogosResult DeliveryModuleImpl::send(const std::string& contentTopic, const std::vector<uint8_t>& payload)
{
    fprintf(stderr, "DeliveryModuleImpl::send called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot send message - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    nlohmann::json messageObj;
    messageObj["contentTopic"] = contentTopic;
    messageObj["payload"] = delivery_base64::encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = callApiRetValue(
        "send",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_send, asCtx(deliveryCtxHandle), messageJson.c_str()));

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

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot subscribe - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "subscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_subscribe, asCtx(deliveryCtxHandle), contentTopic.c_str()));

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

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot unsubscribe - context not initialized.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "unsubscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_unsubscribe, asCtx(deliveryCtxHandle), contentTopic.c_str()));

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

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot run store query - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    // timeoutMs bounds the query on the FFI side; wait longer than that for the
    // completion callback so the query's own timeout error reaches the caller
    // instead of a callback timeout.
    auto callbackTimeout = std::max(
        CALLBACK_TIMEOUT, std::chrono::seconds(timeoutMs / 1000 + 5));

    auto outcome = callApiRetValue(
        "store_query",
        callbackTimeout,
        bindApiCall(logosdelivery_ctx_waku_store_query, asCtx(deliveryCtxHandle),
                    jsonQuery.c_str(), peerAddr.c_str(), static_cast<int32_t>(timeoutMs)));

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

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot create channel - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetValue(
        "channel_create",
        CALLBACK_TIMEOUT,
        // Zero cipher callbacks and user data: an unencrypted channel.
        bindApiCall(logosdelivery_ctx_channel_create, asCtx(deliveryCtxHandle),
                    channelId.c_str(), contentTopic.c_str(), senderId.c_str(),
                    uint64_t{0}, uint64_t{0}, uint64_t{0}));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel create failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::channelExists(const std::string& channelId)
{
    fprintf(stderr, "DeliveryModuleImpl::channelExists called with channelId: %s\n", channelId.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot query channel - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetValue(
        "channel_exists",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_channel_exists, asCtx(deliveryCtxHandle), channelId.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel exists failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::channelSend(const std::string& channelId, const std::vector<uint8_t>& payload)
{
    fprintf(stderr, "DeliveryModuleImpl::channelSend called with channelId: %s\n", channelId.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot send channel message - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    nlohmann::json messageObj;
    messageObj["payload"] = delivery_base64::encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = callApiRetValue(
        "channel_send",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_channel_send, asCtx(deliveryCtxHandle),
                    channelId.c_str(), messageJson.c_str()));

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

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot close channel - context not initialized.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "channel_close",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_channel_close, asCtx(deliveryCtxHandle), channelId.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel close failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getAvailableNodeInfoIDs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableNodeInfoIDs called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available node info IDs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_available_node_info_ids",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_get_available_node_info_ids, asCtx(deliveryCtxHandle)));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available node info IDs failed, reason: %s\n", outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getNodeInfo(const std::string& nodeInfoId) {
    fprintf(stderr, "DeliveryModuleImpl::getNodeInfo called with nodeInfoId: %s\n", nodeInfoId.c_str());

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get node info - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_get_node_info, asCtx(deliveryCtxHandle), nodeInfoId.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get node info failed for ID: %s, reason: %s\n",
                nodeInfoId.c_str(), outcome.error.c_str());
    }

    return outcome;
}

StdLogosResult DeliveryModuleImpl::getAvailableConfigs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableConfigs called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available configs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_available_configs",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_get_available_configs, asCtx(deliveryCtxHandle)));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available configs failed, reason: %s\n", outcome.error.c_str());
    }

    return outcome;
}

std::string DeliveryModuleImpl::collectOpenMetricsText()
{
    if (!deliveryCtx) {
        // No node yet — empty document; the openmetrics scraper renders nothing
        // for this module rather than treating the scrape as a hard error.
        return "";
    }

    auto outcome = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_get_node_info, asCtx(deliveryCtxHandle), "Metrics"));

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
    static const LogosDeliveryRlnPlugin rlnPlugin = {
        .get_membership_state = rln_get_membership_state_callback,
        .get_epoch_quota = rln_get_epoch_quota_callback,
        .generate_proof = rln_generate_proof_callback,
        .validate_proof = rln_validate_proof_callback,
    };

    auto next = std::make_shared<DeliveryRlnConfig>(cfg);
    next->enabled = true;
    {
        std::lock_guard<std::mutex> lock(rlnConfigMutex);
        rlnConfig = std::move(next);
    }
    // The setter is no nim-ffi entry point, so it does not bring the Nim runtime
    // up; before that its lock is uninitialized (fatal on Windows). This call does.
    (void)logosdelivery_version();
    if (logosdelivery_rln_set_plugin(&rlnPlugin, this) != 0) {
        std::lock_guard<std::mutex> lock(rlnConfigMutex);
        rlnConfig = std::make_shared<const DeliveryRlnConfig>();
        return "failed to install the RLN plugin";
    }
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

void DeliveryModuleImpl::joinRlnBringUp()
{
    if (rlnBringUpThread.joinable()) {
        rlnBringUpThread.join();
    }
}

StdLogosResult DeliveryModuleImpl::rlnRespond(int64_t reqId, const std::string& resultJson)
{
    fprintf(stderr, "DeliveryModuleImpl::rlnRespond called with reqId: %lld\n",
            static_cast<long long>(reqId));

    if (!deliveryCtx) {
        return {false, {}, "Context not initialized"};
    }

    // resultJson passes through verbatim (opaque JSON, RLN module's schema).
    // A non-zero return means the reqId is unknown — typically the request
    // already timed out library-side and was answered with a synthetic
    // TRANSIENT failure.
    if (logosdelivery_rln_response(static_cast<uint64_t>(reqId), resultJson.c_str()) != 0) {
        fprintf(stderr, "DeliveryModuleImpl: rlnRespond rejected for reqId: %lld\n",
                static_cast<long long>(reqId));
        return {false, {}, "unknown or already-completed reqId"};
    }

    return {true, {}};
}
