#include "delivery_module_plugin.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <unordered_map>
#include <vector>

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
    "onMessageSent",
    "onMessageError",
    "onMessagePropagated",
    "onMessageReceived",
    "onConnectionStatusChange",
    "onChannelMessageReceived",
    "onChannelMessageSent",
    "onChannelMessageError",
};

std::mutex gMixRlnBridgeMutex;
std::vector<RlnBridge*> gMixRlnBridges;

void dispatchMixRlnCall(uint64_t id, const char* method, const char* args, void*)
{
    std::lock_guard<std::mutex> lock(gMixRlnBridgeMutex);
    const auto bridgeIt = std::find_if(gMixRlnBridges.rbegin(), gMixRlnBridges.rend(),
        [](const RlnBridge* bridge) { return bridge->enabled(); });
    if (bridgeIt == gMixRlnBridges.rend()) {
        logosdelivery_rln_response(
            id, "{\"error\":{\"class\":\"transient\",\"message\":\"Mix RLN bridge unavailable\"}}");
        return;
    }

    try {
        (*bridgeIt)->callMix(id, method ? method : "", args ? args : "[]");
    } catch (...) {
        logosdelivery_rln_response(
            id, "{\"error\":{\"class\":\"transient\",\"message\":\"Mix RLN submission failed\"}}");
    }
}

bool registerMixRlnBridge(RlnBridge* bridge)
{
    std::lock_guard<std::mutex> lock(gMixRlnBridgeMutex);
    if (std::find(gMixRlnBridges.begin(), gMixRlnBridges.end(), bridge)
        != gMixRlnBridges.end()) {
        return true;
    }
    if (gMixRlnBridges.empty()
        && logosdelivery_mix_rln_set_callback(dispatchMixRlnCall, nullptr) != 0) {
        return false;
    }
    gMixRlnBridges.push_back(bridge);
    return true;
}

void unregisterMixRlnBridge(RlnBridge* bridge)
{
    std::lock_guard<std::mutex> lock(gMixRlnBridgeMutex);
    const auto bridgeIt = std::find(gMixRlnBridges.begin(), gMixRlnBridges.end(), bridge);
    if (bridgeIt == gMixRlnBridges.end()) {
        return;
    }
    gMixRlnBridges.erase(bridgeIt);
    if (gMixRlnBridges.empty()) {
        logosdelivery_mix_rln_set_callback(nullptr, nullptr);
    }
}
} // namespace

void DeliveryModuleImpl::start_callback(int errCode, const char* const* reply,
                                        const char* errMsg, void* userData)
{
    if (errCode == NIMFFI_RET_STALE_WARN) {
        return; // a progress tick, not the reply
    }
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
    failedStartStopThread = std::thread([this] {
        if (logosdelivery_ctx_stop_node(asCtx(deliveryCtxHandle), stop_callback, this) != RET_OK) {
            fprintf(stderr, "DeliveryModuleImpl: failed to stop the node after a failed start\n");
        }
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
    if (errCode == NIMFFI_RET_STALE_WARN) {
        return; // a progress tick, not the reply
    }
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
    rlnBridge->init(&modules().rln_module);
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

std::shared_ptr<const RlnPresetEntry> DeliveryModuleImpl::rlnConfigSnapshot() const
{
    std::lock_guard<std::mutex> lock(rlnConfigMutex);
    return rlnConfig;
}

void DeliveryModuleImpl::abortRln()
{
    // Only this thread writes rlnConfig, so reading it here needs no lock.
    if (!rlnConfig->runsRln()) {
        return;
    }
    if (rlnConfig->enabled) {
        logosdelivery_rln_set_plugin(nullptr, nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(rlnConfigMutex);
        rlnConfig = std::make_shared<const RlnPresetEntry>();
    }
    {
        std::lock_guard<std::mutex> lock(rlnStateMutex);
        rlnStateConfig = RlnPresetEntry{};
    }
}

void DeliveryModuleImpl::releaseNode()
{
    // A stop issued after a failed start holds the context handle: finish it
    // first.
    joinFailedStartStop();
    // The bring-up thread touches rlnBridge and rlnConfig; nothing below may
    // run while it is still in flight. A no-op when no thread was started.
    joinRlnBringUp();

    // The backend this module started goes with the node context. Before
    // abortRln, which forgets that this module manages one.
    stopRlnBackend();

    // Before ctx_destroy on purpose: clearing the RLN surface fails all
    // in-flight RLN requests, so no new RLN callback is dispatched into this
    // object while the node goes away. (A callback already executing on the
    // library thread is not joined; it holds its own rlnConfig snapshot.)
    // Guarded on rlnConfig->enabled (inside) because the library's plugin is
    // process-global -- clearing it unconditionally would disarm another
    // instance's RLN.
    abortRln();
    unregisterMixRlnBridge(rlnBridge.get());
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
    const bool rlnWasInstalled = rlnConfig->runsRln();
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

            if (eventType == "message_sent") {
                impl->messageSent(
                    jsonObj.value("messageHash", ""),
                    timestamp);

            } else if (eventType == "message_error") {
                impl->messageError(
                    jsonObj.value("messageHash", ""),
                    jsonObj.value("kind", ""),
                    jsonObj.value("error", ""),
                    timestamp);

            } else if (eventType == "message_propagated") {
                impl->messagePropagated(
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

// Without leading and trailing whitespace, as the library strips the values
// it matches (entryLayer, mode).
static std::string trimCopy(std::string s)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), isSpace));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), isSpace).base(), s.end());
    return s;
}

// Whether the config names the kernel entry: `entryLayer` (key matched
// without regard to case), its value trimmed and lower-cased as the library
// reads it.
static bool isKernelEntry(const nlohmann::json& cfgObj);

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

// True when the config is a bare WakuNodeConf: any top-level key besides the
// ones the layered parser consumes is a WakuNodeConf field.
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

static bool isKernelEntry(const nlohmann::json& cfgObj)
{
    const auto entryLayerKey = findKey(cfgObj, {"entrylayer"});
    return entryLayerKey && cfgObj[*entryLayerKey].is_string()
        && toLowerCopy(trimCopy(cfgObj[*entryLayerKey].get<std::string>())) == "kernel";
}

RlnMode configMode(const nlohmann::json& cfg)
{
    if (!cfg.is_object()) {
        return RlnMode::Core;
    }
    // The library reads no mode on the kernel entry.
    if (isKernelEntry(cfg)) {
        return RlnMode::Core;
    }
    const auto modeKey = findKey(cfg, {"mode"});
    if (!modeKey || !cfg[*modeKey].is_string()) {
        return RlnMode::Core;
    }
    const std::string mode = toLowerCopy(trimCopy(cfg[*modeKey].get<std::string>()));
    return mode == "edge" ? RlnMode::Edge : RlnMode::Core;
}

// The objects the library reads node keys from, in the order a later one
// wins: the top level (flat shape), kernelConf, messagingOverrides. This
// module reads these keys through it, each in every spelling findKey matches:
// `rln-relay`, `rln-relay-eth-contract-address`, `rln-relay-cred-path`
// (relayRlnIsOn), `mix`, `anonymityLevel` (mixIsOn) and
// `mix-rln-registry-id` (callerMixRegistryError); membershipKeyError checks
// the types of the first five.
static std::vector<const nlohmann::json*> configLayers(const nlohmann::json& cfgObj)
{
    std::vector<const nlohmann::json*> layers{&cfgObj};
    for (const char* layer : {"kernelconf", "messagingoverrides"}) {
        if (const auto layerKey = findKey(cfgObj, {layer});
            layerKey && cfgObj[*layerKey].is_object()) {
            layers.push_back(&cfgObj[*layerKey]);
        }
    }
    return layers;
}

// The last value of a key in `layers`, in any of its spellings.
static std::optional<nlohmann::json> lastValue(const std::vector<const nlohmann::json*>& layers,
                                               std::initializer_list<const char*> names)
{
    std::optional<nlohmann::json> found;
    for (const nlohmann::json* layer : layers) {
        if (const auto key = findKey(*layer, names)) {
            found = (*layer)[*key];
        }
    }
    return found;
}

// The delivery library's network presets that run Mix (`mix: true` in
// networks_config.nim), so default to anonymity level Preferred.
static bool libraryPresetRunsMix(const std::string& preset)
{
    return preset == "logos.dev" || preset == "logos.test";
}

// Whether the node mounts Mix, as the delivery library decides it
// (api/conf/logos_delivery_conf.nim): `mix: true` mounts it whatever the
// anonymity level, a level of None included; otherwise a level above None
// turns it on and None leaves it off; without a level, `mix` decides; without
// either, the network preset (libraryPresetRunsMix). The kernel entry reads no
// level.
bool mixIsOn(const nlohmann::json& cfgObj, const std::string& preset)
{
    if (!cfgObj.is_object()) {
        return false;
    }
    const auto layers = configLayers(cfgObj);
    const auto mix = lastValue(layers, {"mix"});
    const bool mixSet = mix && mix->is_boolean();
    if (mixSet && mix->get<bool>()) {
        return true;
    }
    if (!isKernelEntry(cfgObj)) {
        if (const auto level = lastValue(layers, {"anonymitylevel", "anonymity-level"});
            level && level->is_string()) {
            return toLowerCopy(level->get<std::string>()) != "none";
        }
    }
    if (mixSet) {
        return false;
    }
    return libraryPresetRunsMix(preset);
}

// Whether the caller's config keeps the preset's relay RLN membership:
// `rln-relay: false` turns relay RLN off, and an on-chain contract address or
// a keystore (`rln-relay-cred-path`) picks the library's own backend instead,
// as the library's RLN conf builder does.
bool relayRlnIsOn(const nlohmann::json& cfgObj)
{
    if (!cfgObj.is_object()) {
        return true;
    }
    const auto layers = configLayers(cfgObj);
    if (const auto rlnRelay = lastValue(layers, {"rln-relay", "rlnrelay"});
        rlnRelay && rlnRelay->is_boolean() && !rlnRelay->get<bool>()) {
        return false;
    }
    const auto contract = lastValue(layers, {"rln-relay-eth-contract-address",
                                             "rlnrelayethcontractaddress", "rlncontractaddress"});
    const auto credPath = lastValue(layers, {"rln-relay-cred-path", "rlnrelaycredpath"});
    const auto named = [](const std::optional<nlohmann::json>& v) {
        return v && v->is_string() && !v->get<std::string>().empty();
    };
    return !named(contract) && !named(credPath);
}

RlnPresetEntry nodeRlnPreset(const RlnPresetEntry& preset, const nlohmann::json& cfgObj,
                             const std::string& presetName)
{
    RlnPresetEntry node = preset;
    if (node.enabled && !relayRlnIsOn(cfgObj)) {
        node.enabled = false;
    }
    if (node.hasMix() && !mixIsOn(cfgObj, presetName)) {
        node.mixRegistryId.clear();
    }
    return node;
}

// Why a key that decides the node's memberships (relayRlnIsOn, mixIsOn) has a
// value they would not read; empty when every one, in every layer, is usable.
// A `"false"` string, a number or a misspelt level would otherwise be ignored
// and leave a membership on that the caller meant to drop.
static std::string membershipKeyError(const nlohmann::json& cfgObj)
{
    struct Key {
        std::initializer_list<const char*> names;
        const char* expected;
        bool (*valid)(const nlohmann::json&);
    };
    static const auto isBool = [](const nlohmann::json& v) { return v.is_boolean(); };
    static const auto isString = [](const nlohmann::json& v) { return v.is_string(); };
    static const auto isLevel = [](const nlohmann::json& v) {
        if (!v.is_string()) {
            return false;
        }
        const std::string level = toLowerCopy(v.get<std::string>());
        return level == "none" || level == "preferred" || level == "required";
    };
    const Key keys[] = {
        {{"rln-relay", "rlnrelay"}, "a boolean", isBool},
        {{"mix"}, "a boolean", isBool},
        {{"anonymitylevel", "anonymity-level"}, "one of None, Preferred, Required", isLevel},
        {{"rln-relay-eth-contract-address", "rlnrelayethcontractaddress", "rlncontractaddress"},
         "a string",
         isString},
        {{"rln-relay-cred-path", "rlnrelaycredpath"}, "a string", isString},
    };
    for (const nlohmann::json* layer : configLayers(cfgObj)) {
        for (const Key& key : keys) {
            if (const auto name = findKey(*layer, key.names);
                name && !key.valid((*layer)[*name])) {
                return *name + " must be " + key.expected + ", not " + (*layer)[*name].dump();
            }
        }
    }
    return {};
}

// Why the caller's mix-rln-registry-id is not the registry the backend serves
// for Mix; empty when it is.
static std::string callerMixRegistryError(const nlohmann::json& value, const RlnPresetEntry& preset)
{
    const std::string asked = value.is_string() ? value.get<std::string>() : value.dump();
    if (!preset.hasMix()) {
        return "mix-rln-registry-id " + asked
               + " is set, but this node takes no Mix membership (the preset names no mix "
                 "registry, or the node does not run Mix)";
    }
    if (!value.is_string() || asked != preset.mixRegistryId) {
        return "mix-rln-registry-id " + asked + " is not the preset's mix registry "
               + preset.mixRegistryId;
    }
    return {};
}

// Defaults the node's storage directory to the host's per-instance path, so
// side-by-side instances don't share upstream's cwd-relative "./data". Also
// carries the preset's validation switch into the config as
// rln-disable-validation: deployment policy, so it overrides any client key.
// With a mix preset, sets each mix-rln-* key the caller left unset where the
// target turns Mix on (mixIsOn). Each setting goes where the config shape
// accepts it: kernelConf when present, messagingOverrides (created if needed)
// for the layered shapes, top level for a bare WakuNodeConf. A
// mix-rln-registry-id the caller wrote, in any of those objects, must pass
// callerMixRegistryError. `namedPreset` is the preset as named, `preset` the
// memberships this node takes (nodeRlnPreset). nullopt on failure, with the
// createNode error in `error`.
static std::optional<std::string> applyConfigDefaults(const std::string& cfg,
                                                      const std::string& persistencePath,
                                                      const RlnPresetEntry& namedPreset,
                                                      const RlnPresetEntry& preset,
                                                      std::string& error)
{
    error.clear();
    nlohmann::json cfgObj;
    try {
        cfgObj = nlohmann::json::parse(cfg);
    } catch (const nlohmann::json::parse_error&) {
        fprintf(stderr, "DeliveryModuleImpl: createNode cfg is not valid JSON\n");
        error = "Invalid JSON config";
        return std::nullopt;
    }

    if (!cfgObj.is_object()) {
        fprintf(stderr, "DeliveryModuleImpl: createNode cfg is not a JSON object\n");
        error = "Invalid JSON config";
        return std::nullopt;
    }

    error = membershipKeyError(cfgObj);
    if (!error.empty()) {
        return std::nullopt;
    }

    // Checked only when this module starts the backend: a host-managed one
    // serves registries the preset does not describe. Gated on the named
    // preset, so a node whose config drops both memberships is still checked:
    // a mix-rln-registry-id there names a membership it does not take.
    if (namedPreset.runsRln() && namedPreset.manageBackend) {
        // Every object the library reads node keys from, whichever exist, and
        // every spelling of the key findKey's lower-casing matches.
        for (const nlohmann::json* layer : configLayers(cfgObj)) {
            for (const auto& item : layer->items()) {
                const std::string key = toLowerCopy(item.key());
                if (key != "mix-rln-registry-id" && key != "mixrlnregistryid") {
                    continue;
                }
                error = callerMixRegistryError(item.value(), preset);
                if (!error.empty()) {
                    return std::nullopt;
                }
            }
        }
    }

    const bool disableRlnValidation = preset.enabled && !preset.enableValidation;
    // `preset` is this node's (nodeRlnPreset): a mix registry is left only
    // when the node runs Mix.
    const bool mixRegistry = preset.hasMix();
    if (!persistencePath.empty() || disableRlnValidation || mixRegistry) {
        nlohmann::json* target = &cfgObj;
        const bool kernelEntry = isKernelEntry(cfgObj);
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
        if (target && mixRegistry) {
            const auto callerSet = [&](const char* dashed, const char* joined) {
                return findKey(*target, {dashed, joined}).has_value();
            };
            if (!callerSet("mix-rln-registry-id", "mixrlnregistryid")) {
                (*target)["mix-rln-registry-id"] = preset.mixRegistryId;
            }
            if (!callerSet("mix-rln-identifier-hex", "mixrlnidentifierhex")) {
                (*target)["mix-rln-identifier-hex"] = preset.mixRlnIdentifier;
            }
            if (!callerSet("mix-rln-metadata-topic", "mixrlnmetadatatopic")) {
                (*target)["mix-rln-metadata-topic"] = preset.mixMetadataTopic;
            }
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
    RlnMode rlnMode = RlnMode::Core;
    const nlohmann::json cfgObj =
        nlohmann::json::parse(cfg, nullptr, /*allow_exceptions=*/false);
    if (cfgObj.is_object()) {
        if (auto k = findKey(cfgObj, {"preset"}); k && cfgObj[*k].is_string()) {
            presetName = cfgObj[*k].get<std::string>();
        }
        rlnMode = configMode(cfgObj);
    }

    // A named presets file that cannot be used is fatal: the caller asked for
    // a deployment this node cannot reproduce, and coming up without RLN
    // would look like success.
    std::string presetError;
    const RlnPresetEntry namedPreset = resolveRlnPreset(presetName, presetError);
    if (!presetError.empty()) {
        return {false, {}, presetError};
    }
    // Everything below sees the memberships this node takes.
    const RlnPresetEntry rlnPreset = nodeRlnPreset(namedPreset, cfgObj, presetName);

    std::string cfgError;
    auto cfgWithDefaults =
        applyConfigDefaults(cfg, instancePersistencePath(), namedPreset, rlnPreset, cfgError);
    if (!cfgWithDefaults) {
        return {false, {}, cfgError};
    }
    const std::string& cfgWithPorts = *cfgWithDefaults;

    rlnBridge->init(&modules().rln_module);

    if (rlnPreset.runsRln()) {
        if (std::string failure = installRlnPlugin(rlnPreset, rlnMode); !failure.empty()) {
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

    // ctx_create initializes the Nim runtime. Install the process-global Mix
    // callback only after that initialization and after this instance owns a
    // valid context.
    if (!registerMixRlnBridge(rlnBridge.get())) {
        return releaseAndFail("Failed to install the Mix RLN callback");
    }

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
    // Fail only when the node asked for a plugin and we cannot provide it. An
    // unanswered or unreadable reply installs none: a node that did ask then
    // fails its start, which logos-delivery enforces without a plugin.
    if (!result.success) {
        fprintf(stderr, "DeliveryModuleImpl: no discovery requirements, no plugin installed: %s\n",
                result.error.c_str());
    } else {
        const std::string requirements = result.value.is_string()
            ? result.value.get<std::string>()
            : result.value.dump();
        failure = delivery_discovery::fromRequirements(
            requirements, delivery_discovery::libp2pEnvConfig(), discovery);
        if (!failure.empty() && !discovery.requested) {
            fprintf(stderr, "DeliveryModuleImpl: discovery requirements not understood, no plugin installed: %s\n",
                    failure.c_str());
            failure.clear();
        }
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

    if (rlnPreset.runsRln()) {
        rlnBringUpThread = std::thread([this, rlnMode] {
            const std::string failure = startRlnBackend(rlnMode);
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

    // RLN stays up: it lives as long as the node context, not as long as the
    // node runs.
    if (logosdelivery_ctx_stop_node(asCtx(deliveryCtxHandle), stop_callback, this) != RET_OK) {
        return {false, {}, "failed to initiate stop"};
    }
    return {true, {}};
}

void DeliveryModuleImpl::stopRlnBackend()
{
    std::lock_guard<std::mutex> lock(rlnBackendStopMutex);
    // This module started the RLN backend, so it stops it too. Stopping one
    // that is still starting would race the bring-up thread.
    joinRlnBringUp();
    const auto config = rlnConfigSnapshot();
    if (config->runsRln() && config->manageBackend && rlnBridge->enabled()) {
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
        fprintf(stderr, "DeliveryModuleImpl: Send initiated for topic: %s, with success, messageHash: %s\n",
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

StdLogosResult DeliveryModuleImpl::getConnectionStatus() {
    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get connection status - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_connection_status",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_get_connection_status, asCtx(deliveryCtxHandle)));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get connection status failed, reason: %s\n",
                outcome.error.c_str());
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
    if (rlnStateConfig.runsRln()) {
        out["mode"] = rlnModeName(rlnStateMode);
        const RlnRates& rates = rlnStateConfig.rates.of(rlnStateMode);
        if (rlnStateConfig.enabled) {
            out["registryId"] = rlnStateConfig.registryId;
            out["rlnIdentifier"] = rlnStateConfig.rlnIdentifier;
            out["epochSizeSec"] = rlnStateConfig.epochSizeSec;
            if (rates.rateLimit != 0) {
                out["rateLimit"] = rates.rateLimit;
            }
        }
        if (rlnStateConfig.hasMix()) {
            if (rates.mixRateLimit != 0) {
                out["mixRateLimit"] = rates.mixRateLimit;
            }
            out["mixRegistryId"] = rlnStateConfig.mixRegistryId;
            out["mixRlnIdentifier"] = rlnStateConfig.mixRlnIdentifier;
            out["mixEpochSizeSec"] = kMixEpochSizeSec;
            out["mixMaxEpochGap"] = kMixMaxEpochGap;
        }
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

std::string DeliveryModuleImpl::installRlnPlugin(const RlnPresetEntry& preset, RlnMode mode)
{
    // The setter is process-global: one delivery module instance per process.
    static const LogosDeliveryRlnPlugin rlnPlugin = {
        .get_membership_state = rln_get_membership_state_callback,
        .get_epoch_quota = rln_get_epoch_quota_callback,
        .generate_proof = rln_generate_proof_callback,
        .validate_proof = rln_validate_proof_callback,
    };

    {
        std::lock_guard<std::mutex> lock(rlnConfigMutex);
        rlnConfig = std::make_shared<const RlnPresetEntry>(preset);
    }
    // The library's relay plugin only for relay RLN: Mix RLN reaches the
    // backend over the mix bridge (registerMixRlnBridge), not this plugin.
    // The setter is no nim-ffi entry point, so it does not bring the Nim runtime
    // up; before that its lock is uninitialized (fatal on Windows). This call does.
    (void)logosdelivery_version();
    if (preset.enabled && logosdelivery_rln_set_plugin(&rlnPlugin, this) != 0) {
        std::lock_guard<std::mutex> lock(rlnConfigMutex);
        rlnConfig = std::make_shared<const RlnPresetEntry>();
        return "failed to install the RLN plugin";
    }
    {
        std::lock_guard<std::mutex> lock(rlnStateMutex);
        rlnStateConfig = *rlnConfig;
        rlnStateMode = mode;
    }
    return {};
}

std::string DeliveryModuleImpl::startRlnBackend(RlnMode mode)
{
    // The in-process bridge is one way to answer; the rln*Request events plus
    // rlnRespond are the other, so a bridge that cannot come up is not fatal.
    // Only a bridge that IS up starts the backend, because only it can reach
    // the RLN module.
    const std::string failure = bringUpRlnBridge();
    if (!failure.empty()) {
        return "rln bridge unavailable (" + failure + "); answering falls to rlnRespond";
    }

    // A host sharing the backend with other consumers owns its configuration
    // and lifetime.
    if (!rlnConfig->manageBackend) {
        return {};
    }

    // A node that mounts RLN over a stopped backend Ignores every inbound RLN
    // message. One start names every registry the node takes a membership on.
    const std::string startFailure =
        rlnBridge->startBackend(rlnBackendStartConfig(*rlnConfig, mode));
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

std::string rlnBackendStartConfig(const RlnPresetEntry& preset, RlnMode mode)
{
    const RlnRates& rates = preset.rates.of(mode);
    // Each registry is an entry of its own: rln_module reads a top-level gap
    // or rate as the default for every entry, so the relay registry's would
    // reach the mix one. The top-level epoch size is the one key it requires.
    // Relay and Mix RLN are independent, so either list entry may be absent;
    // with no relay registry the top-level epoch size is Mix's.
    nlohmann::json startCfg{
        {"epoch_size_sec", preset.enabled ? preset.epochSizeSec : kMixEpochSizeSec},
        {"registries", nlohmann::json::array()}};
    if (preset.enabled) {
        nlohmann::json relay{{"registry_id", preset.registryId},
                             {"epoch_size_sec", preset.epochSizeSec}};
        if (preset.maxEpochGap != 0) {
            relay["max_epoch_gap"] = preset.maxEpochGap;
        }
        if (rates.rateLimit != 0) {
            relay["rate_limit"] = rates.rateLimit;
        }
        startCfg["registries"].push_back(std::move(relay));
    }
    if (preset.hasMix()) {
        // The epoch size and gap native Mix's proof plugin runs at.
        nlohmann::json mix{{"registry_id", preset.mixRegistryId},
                           {"epoch_size_sec", kMixEpochSizeSec},
                           {"max_epoch_gap", kMixMaxEpochGap}};
        if (rates.mixRateLimit != 0) {
            mix["rate_limit"] = rates.mixRateLimit;
        }
        startCfg["registries"].push_back(std::move(mix));
    }
    return startCfg.dump();
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

StdLogosResult DeliveryModuleImpl::getLocalMixPeerRecord() {
    if (!deliveryCtx) return {false, {}, "Context not initialized"};
    return callApiRetValue("waku_mix_get_peer_record", CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_waku_mix_get_peer_record, asCtx(deliveryCtxHandle)));
}

StdLogosResult DeliveryModuleImpl::addMixPeer(const std::string& recordJson) {
    if (!deliveryCtx) return {false, {}, "Context not initialized"};
    return callApiRetValue("waku_mix_add_peer", CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_ctx_waku_mix_add_peer, asCtx(deliveryCtxHandle),
                    recordJson.c_str()));
}
