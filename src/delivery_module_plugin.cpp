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
#include "disco_trace.h"
#include "discovery_config.h"
#include "libp2p_bring_up.h"
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
        // A progress tick, not the reply: the ticket is freed on the reply.
        return;
    }
    auto* ticket = static_cast<StartTicket*>(userData);
    if (!ticket || !ticket->impl) {
        fprintf(stderr, "DeliveryModuleImpl::start_callback: Invalid userData\n");
        return;
    }
    DeliveryModuleImpl* impl = ticket->impl;
    impl->nodeStarted(errCode == RET_OK, replyText(errCode, reply, errMsg),
                      currentTimestampNs());
    impl->startFinished(ticket, errCode != RET_OK);
}

void DeliveryModuleImpl::startFinished(StartTicket* ticket, bool failed)
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    const uint64_t generation = ticket->generation;
    // Only the terminal reply gets here (start_callback drops progress ticks).
    const auto ticketIt = std::find_if(
        startTickets.begin(), startTickets.end(),
        [ticket](const std::unique_ptr<StartTicket>& held) { return held.get() == ticket; });
    if (ticketIt != startTickets.end()) {
        startTickets.erase(ticketIt);
    }
    if (failed) {
        // The same two halves as stop(): the node, then the RLN backend this
        // module started for it. On the worker, as the library's thread
        // refuses re-entrant calls. start() is refused until it has run.
        if (postLifecycleLocked(LifecycleKind::StopFailedStart, generation) != 0) {
            ++failedStartCleanups;
        }
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
    // Also retries a failed bring-up. Posted after the enable above, so the
    // worker's own enable does not run alongside it.
    (void)postRlnUp();
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
    clearRlnMembershipResults();
}

void DeliveryModuleImpl::releaseNode()
{
    // Joins the worker first: it uses the context handle, rlnBridge and
    // rlnConfig. It runs what was already posted but begins no bring-up.
    stopLifecycleWorker();

    // Before ctx_destroy on purpose: clearing the RLN surface fails all
    // in-flight RLN requests, so no new RLN callback is dispatched into this
    // object while the node goes away. (A callback already executing on the
    // library thread is not joined; it holds its own rlnConfig snapshot.)
    // Guarded on rlnConfig->enabled because the library's plugin is
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
    libp2pBringUp.reset();

    // The node is gone, so no start reply can arrive for a ticket any more.
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    startTickets.clear();
}

StdLogosResult DeliveryModuleImpl::releaseAndFail(std::string reason)
{
    const bool rlnWasInstalled = rlnConfig->enabled;
    releaseNode();
    if (rlnWasInstalled) {
        setRlnState(RlnState::Disabled, {});
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

// How a config decides Mix, as logos-delivery reads it since
// logos-messaging/logos-delivery#4780: an explicit `mix` decides; else the kernel entry runs no Mix; else Core mode runs Mix; else
// (Edge) the anonymity level does (with `level`), and an absent level is
// Preferred. `requested` is true when the config asks for Mix itself: `mix:
// true`, or a level of Preferred / Required.
//
// The values are read as the library reads them, through confutils: a scalar
// is taken as its text, a boolean is true for an empty string and for y, yes,
// true, 1 and on, an enum by its name, and both ignore letter case and
// underscores. A value the library cannot parse fails its own createNode.
struct MixDecision {
    bool on = false;
    bool requested = false;
};

// The text confutils parses, normalized as its parsers do; nullopt for a
// value that is no scalar, or is a JSON null (the key is then unset).
static std::optional<std::string> confutilsText(const nlohmann::json& value)
{
    std::string text;
    if (value.is_string()) {
        text = value.get<std::string>();
    } else if (value.is_boolean()) {
        text = value.get<bool>() ? "true" : "false";
    } else if (value.is_number_integer()) {
        text = std::to_string(value.get<int64_t>());
    } else {
        return std::nullopt;
    }
    text = toLowerCopy(text);
    text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
    return text;
}

static MixDecision mixDecision(const nlohmann::json* target, bool level, bool kernelEntry,
                               RlnMode mode)
{
    const bool object = target && target->is_object();
    if (object) {
        if (const auto key = findKey(*target, {"mix"})) {
            if (const auto value = confutilsText((*target)[*key])) {
                const bool on = value->empty() || *value == "y" || *value == "yes"
                    || *value == "true" || *value == "1" || *value == "on";
                return {on, on};
            }
        }
    }
    std::optional<std::string> levelValue;
    if (object && level) {
        if (const auto key = findKey(*target, {"anonymitylevel", "anonymity-level"})) {
            levelValue = confutilsText((*target)[*key]);
        }
    }
    const bool levelAsks =
        levelValue && (*levelValue == "preferred" || *levelValue == "required");
    if (kernelEntry) {
        return {false, false};
    }
    if (mode == RlnMode::Core) {
        return {true, levelAsks};
    }
    if (levelValue) {
        return {levelAsks, levelAsks};
    }
    return {true, false};
}

// The object a createNode config takes node keys in, by its shape: kernelConf
// when present (where an anonymity level is not read: `level` false), the top
// level of a bare WakuNodeConf, messagingOverrides for the layered shapes.
// Null for a kernel entry without a kernelConf, left for the parser to reject,
// and for a layered config without a messagingOverrides object.
static nlohmann::json* nodeKeyTarget(nlohmann::json& cfgObj, bool& level)
{
    level = true;
    if (const auto kernelConfKey = findKey(cfgObj, {"kernelconf"});
        kernelConfKey && cfgObj[*kernelConfKey].is_object()) {
        level = false;
        return &cfgObj[*kernelConfKey];
    }
    if (isKernelEntry(cfgObj)) {
        return nullptr;
    }
    if (!isFlatShape(cfgObj)) {
        const auto overridesKey = findKey(cfgObj, {"messagingoverrides"});
        return overridesKey && cfgObj[*overridesKey].is_object() ? &cfgObj[*overridesKey]
                                                                 : nullptr;
    }
    return &cfgObj;
}

// Why the caller's mix-rln-registry-id is not a registry the backend serves
// at Mix's epoch size and gap; empty when it is.
static std::string callerMixRegistryError(const nlohmann::json& value, const RlnPresetEntry& preset)
{
    const std::string asked = value.is_string() ? value.get<std::string>() : value.dump();
    if (preset.hasMix()) {
        if (!value.is_string() || asked != preset.mixRegistryId) {
            return "mix-rln-registry-id " + asked + " is not the preset's mix registry "
                   + preset.mixRegistryId;
        }
        return {};
    }
    if (!value.is_string() || asked != preset.registryId) {
        return "mix-rln-registry-id " + asked
               + " is not a registry of the preset, which names no mix registry and"
                 " whose relay registry is " + preset.registryId;
    }
    if (preset.epochSizeSec != kMixEpochSizeSec || preset.maxEpochGap != kMixMaxEpochGap) {
        return "mix-rln-registry-id names the preset's relay registry, which Mix can"
               " use only at epoch-size-sec " + std::to_string(kMixEpochSizeSec)
               + " and max-epoch-gap " + std::to_string(kMixMaxEpochGap)
               + "; the preset has epoch-size-sec " + std::to_string(preset.epochSizeSec)
               + " and max-epoch-gap "
               + (preset.maxEpochGap == 0 ? std::string("unset")
                                          : std::to_string(preset.maxEpochGap));
    }
    return {};
}

// Defaults the node's storage directory to the host's per-instance path, so
// side-by-side instances don't share upstream's cwd-relative "./data". Also
// carries the preset's validation switch into the config as
// rln-disable-validation: deployment policy, so it overrides any client key.
// With a mix preset, sets each mix-rln-* key the caller left unset where the
// target turns Mix on (mixDecision). Each setting goes where the config shape
// accepts it: kernelConf when present, messagingOverrides (created if needed)
// for the layered shapes, top level for a bare WakuNodeConf. A
// mix-rln-registry-id the caller wrote, in any of those objects, must pass
// callerMixRegistryError. `mixOn` is whether the config turns Mix on. A
// config that asks for Mix is refused unless the preset runs RLN and the node
// gets Mix RLN; Mix that is only on by default is turned off instead. nullopt on failure, with the createNode error in `error`.
static std::optional<std::string> applyConfigDefaults(const std::string& cfg,
                                                      const std::string& persistencePath,
                                                      const RlnPresetEntry& preset,
                                                      bool& mixOn,
                                                      std::string& error)
{
    error.clear();
    mixOn = false;
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

    // Checked only when this module starts the backend: a host-managed one
    // serves registries the preset does not describe.
    if (preset.enabled && preset.manageBackend) {
        // Every object the library reads node keys from, whichever exist, and
        // every spelling of the key findKey's lower-casing matches.
        std::vector<const nlohmann::json*> layers{&cfgObj};
        for (const char* layer : {"kernelconf", "messagingoverrides"}) {
            if (const auto layerKey = findKey(cfgObj, {layer});
                layerKey && cfgObj[*layerKey].is_object()) {
                layers.push_back(&cfgObj[*layerKey]);
            }
        }
        for (const nlohmann::json* layer : layers) {
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
    const bool mixRegistry = preset.enabled && preset.hasMix();
    const bool writes = !persistencePath.empty() || disableRlnValidation || mixRegistry;
    // A layered config takes node keys in messagingOverrides: made when there
    // is something to write there.
    if (writes && !isKernelEntry(cfgObj) && !isFlatShape(cfgObj)
        && !findKey(cfgObj, {"kernelconf"}) && !findKey(cfgObj, {"messagingoverrides"})) {
        cfgObj["messagingOverrides"] = nlohmann::json::object();
    }
    bool level = true;
    nlohmann::json* target = nodeKeyTarget(cfgObj, level);
    const bool kernelEntry = isKernelEntry(cfgObj);
    const MixDecision decision =
        mixDecision(target, level, kernelEntry, configMode(cfgObj));
    // Only a key where this shape takes node keys gives the node Mix RLN: one
    // in an object the library does not read it from does not count.
    const bool targetMixRln =
        target && findKey(*target, {"mix-rln-registry-id", "mixrlnregistryid"}).has_value();
    mixOn = decision.on;
    // Mix is on by default only where the node gets Mix RLN. Elsewhere this
    // module turns it off, as the caller did not ask for it: `mix: false`, which
    // with no level also makes the level None.
    if (decision.on && !decision.requested
        && !(preset.enabled && (preset.hasMix() || targetMixRln))) {
        if (!target && !kernelEntry && !isFlatShape(cfgObj)) {
            cfgObj["messagingOverrides"] = nlohmann::json::object();
            target = nodeKeyTarget(cfgObj, level);
        }
        if (target) {
            (*target)["mix"] = false;
        }
        mixOn = false;
    }

    // Mix is valid only with RLN, and then only with Mix RLN: a mix hop that
    // nothing rates is open to anyone, with or without an RLN-protected relay.
    if (mixOn && !preset.enabled) {
        error = "this config turns Mix on, which needs RLN, but the preset runs no RLN";
        return std::nullopt;
    }
    if (mixOn && preset.enabled && !preset.hasMix() && !targetMixRln) {
        error = "this config turns Mix on and the preset runs RLN, so Mix needs Mix RLN: the"
                " preset names no mix-registry-id and the config no mix-rln-registry-id";
        return std::nullopt;
    }

    if (writes) {
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
            if (mixOn) {
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
    {
        nlohmann::json cfgObj = nlohmann::json::parse(cfg, nullptr, /*allow_exceptions=*/false);
        if (cfgObj.is_object()) {
            if (auto k = findKey(cfgObj, {"preset"}); k && cfgObj[*k].is_string()) {
                presetName = cfgObj[*k].get<std::string>();
            }
            rlnMode = configMode(cfgObj);
        }
    }

    // A named presets file that cannot be used is fatal: the caller asked for
    // a deployment this node cannot reproduce, and coming up without RLN
    // would look like success.
    std::string presetError;
    const RlnPresetEntry namedPreset = resolveRlnPreset(presetName, presetError);
    if (!presetError.empty()) {
        return {false, {}, presetError};
    }
    const RlnPresetEntry modePreset = namedPreset.forMode(rlnMode);

    std::string cfgError;
    bool mixOn = false;
    auto cfgWithDefaults =
        applyConfigDefaults(cfg, instancePersistencePath(), modePreset, mixOn, cfgError);
    if (!cfgWithDefaults) {
        return {false, {}, cfgError};
    }
    const std::string& cfgWithPorts = *cfgWithDefaults;

    // A node that does not run Mix takes no Mix membership: the library mounts
    // no Mix RLN on it, so a membership on the mix registry would only cost
    // the payer, or the allocator, a registration.
    const bool dropMix = modePreset.enabled && modePreset.hasMix() && !mixOn;
    if (dropMix && !modePreset.needsRelayMembership()) {
        return {false, {},
                "preset \"" + presetName + "\" gives a " + rlnModeName(rlnMode)
                    + " node no Relay membership (relay-membership false) and this config does"
                      " not turn Mix on: the node would hold no membership"};
    }
    // Everything below sees the preset resolved for this node's mode, and for
    // whether it runs Mix.
    const RlnPresetEntry rlnPreset = dropMix ? modePreset.withoutMix() : modePreset;

    rlnBridge->init(&modules().rln_module);

    if (rlnPreset.enabled) {
        {
            std::lock_guard<std::mutex> lock(rlnStateMutex);
            rlnStateMode = rlnMode;
        }
        if (std::string failure = installRlnPlugin(rlnPreset); !failure.empty()) {
            return {false, {}, failure};
        }
        setRlnState(RlnState::Initializing, {});
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
    bool requirementsKnown = false;
    if (!result.success) {
        fprintf(stderr, "DeliveryModuleImpl: no discovery requirements, no plugin installed: %s\n",
                result.error.c_str());
    } else {
        const std::string requirements = result.value.is_string()
            ? result.value.get<std::string>()
            : result.value.dump();
        failure = delivery_discovery::fromRequirements(
            requirements, delivery_discovery::libp2pEnvConfig(), discovery,
            rlnPreset.hasAllocator() ? rlnPreset.allocatorBootstrapNodes : std::vector<std::string>{});
        requirementsKnown = failure.empty() || discovery.requested;
        if (!requirementsKnown) {
            fprintf(stderr, "DeliveryModuleImpl: discovery requirements not understood, no plugin installed: %s\n",
                    failure.c_str());
            failure.clear();
        }
    }
    // The allocator path does not depend on the node's answer: without one
    // its libp2p node still mounts discovery and joins the preset's DHT peers.
    if (!requirementsKnown && rlnPreset.hasAllocator()) {
        const std::string bad = delivery_discovery::withoutRequirements(
            delivery_discovery::libp2pEnvConfig(), discovery, rlnPreset.allocatorBootstrapNodes);
        if (!bad.empty()) {
            fprintf(stderr, "DeliveryModuleImpl: no libp2p config for the allocator path: %s\n",
                    bad.c_str());
        }
    }
    // libp2p is not contacted here: outbound calls fail on the Qt main thread.
    // Without a framework (unit tests) there is no modules(); pass null. One
    // libp2p node, mounted for service discovery and DHT-bootstrapped, serves
    // plugin discovery and the allocator path.
    {
        Libp2pModule* libp2p = isContextReady() ? &modules().libp2p_module : nullptr;
        libp2pBringUp = std::make_shared<Libp2pBringUp>(
            libp2p, discovery.libp2pConfig,
            [](const std::string& line) {
                fprintf(stderr, "DeliveryModuleImpl: %s\n", line.c_str());
                disco_trace::traceLine(line);
            });
    }
    if (failure.empty() && discovery.enabled) {
        failure = installServiceDiscoveryPlugin();
    }
    if (!failure.empty()) {
        // A node configured for plugin discovery cannot start without a
        // registered plugin, so a half-built context is worse than none:
        // unwind it and report, rather than failing later at start().
        return releaseAndFail("service discovery setup failed: " + failure);
    }

    // Past every failure exit: from here on releaseNode() has a worker to join.
    startLifecycleWorker();
    if (rlnPreset.enabled) {
        (void)postRlnUp();
    }

    return {true, {}};
}

void DeliveryModuleImpl::startLifecycleWorker()
{
    if (lifecycleThread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        lifecycleQueue.clear();
        lifecycleQuit = false;
        lifecycleUpInFlight = false;
        startIssued = 0;
        startGeneration = 0;
        failedStartCleanups = 0;
        lifecycleAccepting = true;
    }
    lifecycleThread = std::thread([this] { runLifecycleWorker(); });
}

void DeliveryModuleImpl::stopLifecycleWorker()
{
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        lifecycleAccepting = false;
        lifecycleQuit = true;
    }
    lifecycleCv.notify_all();
    if (lifecycleThread.joinable()) {
        lifecycleThread.join();
    }
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    lifecycleQueue.clear();
    lifecycleQuit = false;
    lifecycleUpInFlight = false;
    startIssued = 0;
    startGeneration = 0;
    failedStartCleanups = 0;
    rlnBackendRunning = false;
    rlnReady = false;
    membershipWatch = MembershipWatch{};
}

uint64_t DeliveryModuleImpl::postLifecycleLocked(LifecycleKind kind, uint64_t generation)
{
    if (!lifecycleAccepting) {
        return 0;
    }
    if (kind != LifecycleKind::StopFailedStart) {
        // A request equal to the last one posted adds nothing; an "up" is also
        // satisfied by a bring-up still in flight when nothing follows it.
        const bool sameAsLast = lifecycleQueue.empty()
            ? (kind == LifecycleKind::RlnUp && lifecycleUpInFlight)
            : lifecycleQueue.back().kind == kind;
        if (sameAsLast) {
            return 0;
        }
    }
    const uint64_t id = ++lifecycleNextId;
    lifecycleQueue.push_back(LifecycleRequest{kind, id, generation});
    lifecycleCv.notify_one();
    return id;
}

uint64_t DeliveryModuleImpl::postRlnUp()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return postLifecycleLocked(LifecycleKind::RlnUp, 0);
}

void DeliveryModuleImpl::postRlnDown()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    (void)postLifecycleLocked(LifecycleKind::RlnDown, 0);
}

void DeliveryModuleImpl::runLifecycleWorker()
{
    for (;;) {
        LifecycleRequest request{};
        bool quitting = false;
        bool readBackTick = false;
        {
            std::unique_lock<std::mutex> lock(lifecycleMutex);
            const auto woken = [this] { return lifecycleQuit || !lifecycleQueue.empty(); };
            if (membershipWatchDue()) {
                // The same wait with a timeout: a request or quit still wakes
                // it at once, and only a full quiet interval is a tick.
                readBackTick = !lifecycleCv.wait_for(lock, membershipWatch.interval, woken);
            } else {
                lifecycleCv.wait(lock, woken);
            }
        }
        if (readBackTick) {
            // Off the mutex. A request or quit posted meanwhile waits for the
            // read in flight (bounded by its timeout) and is taken next.
            readRlnMemberships();
            continue;
        }
        {
            std::unique_lock<std::mutex> lock(lifecycleMutex);
            if (lifecycleQueue.empty()) {
                if (lifecycleQuit) {
                    return; // quit, and everything posted before it is done
                }
                // start() took its request back between the wait and here.
                continue;
            }
            request = lifecycleQueue.front();
            lifecycleQueue.pop_front();
            quitting = lifecycleQuit;
            if (request.kind == LifecycleKind::StopFailedStart
                && request.generation < startGeneration) {
                // A later accepted start owns the node: this failure's
                // cleanup is stale.
                --failedStartCleanups;
                continue;
            }
            lifecycleUpInFlight = request.kind == LifecycleKind::RlnUp && !quitting;
        }

        switch (request.kind) {
        case LifecycleKind::RlnUp:
            if (!quitting) {
                bringRlnUp();
            }
            break;
        case LifecycleKind::RlnDown:
            stopRlnBackend();
            break;
        case LifecycleKind::StopFailedStart:
            if (logosdelivery_ctx_stop_node(asCtx(deliveryCtxHandle), stop_callback, this) != RET_OK) {
                fprintf(stderr, "DeliveryModuleImpl: failed to stop the node after a failed start\n");
            }
            // Whether or not the node stop was taken: a failed start ends in
            // Stopped.
            stopRlnBackend();
            {
                // Only now may start() be accepted again.
                std::lock_guard<std::mutex> lock(lifecycleMutex);
                --failedStartCleanups;
            }
            break;
        }
    }
}

void DeliveryModuleImpl::bringRlnUp()
{
    const bool attempt = rlnConfig->enabled && !rlnReady;
    std::string failure;
    if (attempt) {
        resetMembershipReadBack();
        setRlnState(RlnState::Initializing, {});
        failure = startRlnBackend();
        rlnReady = failure.empty();
    }
    {
        // Before the verdict is published: a start() or rlnBridgeEnable()
        // issued in reaction to it must be queued as a new attempt, not taken
        // as satisfied by this one.
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        lifecycleUpInFlight = false;
    }
    if (!attempt) {
        return;
    }
    if (rlnReady) {
        setRlnState(RlnState::Ready, {});
        startMembershipWatch();
    } else {
        setRlnState(RlnState::Failed, failure);
    }
}

void DeliveryModuleImpl::resetMembershipReadBack()
{
    membershipWatch = MembershipWatch{};
    clearRlnMembershipResults();
}

void DeliveryModuleImpl::startMembershipWatch()
{
    membershipWatch = MembershipWatch{};
    membershipWatch.running = true;
    membershipWatch.deadline = std::chrono::steady_clock::now() + kMembershipReadWindow;
    membershipWatch.relayDone = !rlnConfig->needsRelayMembership();
    // A shared registry has one membership, read with the relay's.
    membershipWatch.mixDone = !rlnConfig->hasMix() || rlnConfig->sharesRegistry();
}

bool DeliveryModuleImpl::membershipWatchDue()
{
    if (!membershipWatch.running) {
        return false;
    }
    if ((membershipWatch.relayDone && membershipWatch.mixDone)
        || std::chrono::steady_clock::now() >= membershipWatch.deadline) {
        membershipWatch.running = false;
        return false;
    }
    return true;
}

bool DeliveryModuleImpl::lifecycleWorkPending()
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    return lifecycleQuit || !lifecycleQueue.empty();
}

void DeliveryModuleImpl::clearRlnMembershipResults()
{
    std::lock_guard<std::mutex> lock(rlnStateMutex);
    rlnStateMembership = nullptr;
    rlnStateMixMembership = nullptr;
    rlnStateGrantedRate.reset();
    rlnStateGrantedMixRate.reset();
}

RlnMembershipReading DeliveryModuleImpl::readRlnMembership(const std::string& registryId,
                                                           const std::string& rlnIdentifier,
                                                           std::string& lastFailure,
                                                           bool& unanswered)
{
    nlohmann::json reply;
    const std::string failure =
        rlnBridge->readMembershipState(registryId, rlnIdentifier, reply, &unanswered);
    if (failure.empty()) {
        lastFailure.clear();
        membershipWatch.interval = kMembershipReadInterval;
        return readRlnMembershipReply(reply);
    }
    if (unanswered) {
        membershipWatch.interval =
            std::min(membershipWatch.interval * 2,
                     std::chrono::seconds(kMembershipReadIntervalMax));
    }
    if (failure != lastFailure) {
        fprintf(stderr, "DeliveryModuleImpl: registry %s membership not read (%s)\n",
                registryId.c_str(), failure.c_str());
        lastFailure = failure;
    }
    return {};
}

void DeliveryModuleImpl::readRlnMemberships()
{
    // The worker's reads of rlnConfig need no lock (see rlnConfigMutex).
    const DeliveryRlnConfig& cfg = *rlnConfig;
    const bool shared = cfg.sharesRegistry();

    bool relayUnanswered = false;
    if (!membershipWatch.relayDone) {
        const RlnMembershipReading reading =
            readRlnMembership(cfg.registryId, cfg.rlnIdentifier,
                              membershipWatch.relayLastFailure, relayUnanswered);
        if (!reading.membership.is_null()) {
            {
                std::lock_guard<std::mutex> lock(rlnStateMutex);
                rlnStateMembership = reading.membership;
                rlnStateGrantedRate = reading.grantedRate;
                if (shared) {
                    rlnStateMixMembership = reading.membership;
                    rlnStateGrantedMixRate = reading.grantedRate;
                }
            }
            if (reading.active) {
                membershipWatch.relayDone = true;
                membershipWatch.relayGranted = reading.grantedRate;
            }
        }
    }
    // One read timeout per round at most: the mix read is skipped after an
    // unanswered relay read or when work is queued.
    if (!membershipWatch.mixDone && !relayUnanswered && !lifecycleWorkPending()) {
        bool mixUnanswered = false;
        const RlnMembershipReading reading =
            readRlnMembership(cfg.mixRegistryId, cfg.mixRlnIdentifier,
                              membershipWatch.mixLastFailure, mixUnanswered);
        if (!reading.membership.is_null()) {
            {
                std::lock_guard<std::mutex> lock(rlnStateMutex);
                rlnStateMixMembership = reading.membership;
                rlnStateGrantedMixRate = reading.grantedRate;
            }
            if (reading.active) {
                membershipWatch.mixDone = true;
                membershipWatch.mixGranted = reading.grantedRate;
            }
        }
    }

    // Still Ready: only the worker leaves it, and it is here.
    const std::string capped = rlnCappedGrantMessage(
        shared ? cfg.sharedRateLimit() : cfg.rateLimit, membershipWatch.relayGranted,
        cfg.mixRateLimit, membershipWatch.mixGranted, shared);
    if (!capped.empty()) {
        setRlnState(RlnState::Ready, capped);
    }
}

RlnMembershipReading readRlnMembershipReply(const nlohmann::json& reply)
{
    RlnMembershipReading reading;
    if (!reply.is_object()) {
        return reading;
    }
    const auto state = reply.find("state");
    if (state == reply.end() || !state->is_string()) {
        return reading;
    }
    reading.membership = nlohmann::json::object();
    reading.membership["state"] = state->get<std::string>();
    if (const auto provisioning = reply.find("provisioning");
        provisioning != reply.end() && provisioning->is_object()) {
        const auto step = provisioning->find("step");
        if (step != provisioning->end() && step->is_string()) {
            reading.membership["step"] = step->get<std::string>();
        }
    }
    reading.active = state->get<std::string>() == "active";
    if (reading.active) {
        const auto rate = reply.find("rate_limit");
        if (rate != reply.end() && rate->is_number_unsigned()) {
            reading.grantedRate = rate->get<uint64_t>();
        }
    }
    return reading;
}

std::string rlnCappedGrantMessage(uint64_t askedRate, std::optional<uint64_t> grantedRate,
                                  uint64_t askedMixRate, std::optional<uint64_t> grantedMixRate,
                                  bool shared)
{
    const bool relayDiffers = askedRate != 0 && grantedRate && *grantedRate != askedRate;
    const bool mixDiffers =
        !shared && askedMixRate != 0 && grantedMixRate && *grantedMixRate != askedMixRate;
    const std::string relayName = shared ? "relay and mix membership" : "relay membership";
    if (relayDiffers && mixDiffers) {
        return relayName + " granted at rate " + std::to_string(*grantedRate)
               + ", mix membership at rate " + std::to_string(*grantedMixRate)
               + "; the preset asks " + std::to_string(askedRate) + " and "
               + std::to_string(askedMixRate);
    }
    if (relayDiffers) {
        return relayName + " granted at rate " + std::to_string(*grantedRate)
               + "; the preset asks " + std::to_string(askedRate);
    }
    if (mixDiffers) {
        return "mix membership granted at rate " + std::to_string(*grantedMixRate)
               + "; the preset asks " + std::to_string(askedMixRate);
    }
    return {};
}

std::string DeliveryModuleImpl::installServiceDiscoveryPlugin()
{
    if (!deliveryCtx) {
        return "context not initialized";
    }

    // The shared bring-up was configured with the libp2p config at createNode.
    discoPlugin = std::make_unique<DeliveryServiceDiscoveryPlugin>(libp2pBringUp);

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

    // A node started again needs RLN up again. Posted before the dispatch, so
    // a failed start's cleanup queues after it.
    StartTicket* ticket = nullptr;
    uint64_t upRequest = 0;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (failedStartCleanups > 0) {
            // Starting now would race the stop of the half-started node, or
            // start a node that was never stopped.
            return {false, {},
                    "the previous failed start is still being cleaned up; retry once "
                    "rlnState/nodeStopped settles"};
        }
        startTickets.push_back(
            std::make_unique<StartTicket>(StartTicket{this, ++startIssued}));
        ticket = startTickets.back().get();
        upRequest = postLifecycleLocked(LifecycleKind::RlnUp, 0);
    }
    const uint64_t generation = ticket->generation; // the reply may free the ticket

    // Node start can block for a long time (relay reconnect backoff), so return
    // once dispatched. Completion arrives via nodeStarted.
    if (logosdelivery_ctx_start_node(asCtx(deliveryCtxHandle), start_callback, ticket) != RET_OK) {
        // Nothing was started and no reply will come: take back the ticket
        // and, if the worker has not begun it, the bring-up.
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        const auto ticketIt = std::find_if(
            startTickets.begin(), startTickets.end(),
            [ticket](const std::unique_ptr<StartTicket>& held) { return held.get() == ticket; });
        if (ticketIt != startTickets.end()) {
            startTickets.erase(ticketIt);
        }
        if (upRequest != 0) {
            const auto requestIt = std::find_if(
                lifecycleQueue.begin(), lifecycleQueue.end(),
                [upRequest](const LifecycleRequest& queued) { return queued.id == upRequest; });
            if (requestIt != lifecycleQueue.end()) {
                lifecycleQueue.erase(requestIt);
            }
        }
        return {false, {}, "failed to initiate start"};
    }
    {
        // Accepted: from here on an older start's late failure is stale.
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        startGeneration = std::max(startGeneration, generation);
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
    postRlnDown();
    return {true, {}};
}

void DeliveryModuleImpl::stopRlnBackend()
{
    if (!rlnConfig->enabled) {
        return;
    }
    // This module stops the RLN backend it started; one the host manages is
    // the host's to stop.
    if (rlnConfig->manageBackend && rlnBridge->enabled() && rlnBackendRunning) {
        const std::string failure = rlnBridge->stopBackend();
        if (!failure.empty()) {
            fprintf(stderr, "DeliveryModuleImpl: rln module stop failed: %s\n",
                    failure.c_str());
        }
    }
    // Either way the next start() brings it up again: the RLN module's start
    // is idempotent for a backend that never stopped.
    rlnBackendRunning = false;
    rlnReady = false;
    // No read-back of a stopped backend; what it found stays published until
    // the next bring-up.
    membershipWatch.running = false;
    setRlnState(RlnState::Stopped, {});
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
    nlohmann::json out{{"state", rlnStateName(rlnStateValue)}, {"message", rlnStateMessage}};
    if (rlnStateConfig.enabled) {
        out["registryId"] = rlnStateConfig.registryId;
        out["rlnIdentifier"] = rlnStateConfig.rlnIdentifier;
        out["epochSizeSec"] = rlnStateConfig.epochSizeSec;
        out["mode"] = rlnModeName(rlnStateMode);
        // A shared registry has one rate, whichever key named it.
        const bool shared = rlnStateConfig.sharesRegistry();
        const uint64_t rateLimit =
            shared ? rlnStateConfig.sharedRateLimit() : rlnStateConfig.rateLimit;
        const uint64_t mixRateLimit =
            shared ? rlnStateConfig.sharedRateLimit() : rlnStateConfig.mixRateLimit;
        if (!rlnStateConfig.needsRelayMembership()) {
            out["relayMembership"] = false;
        } else if (rateLimit != 0) {
            out["rateLimit"] = rateLimit;
        }
        if (rlnStateConfig.hasMix() && mixRateLimit != 0) {
            out["mixRateLimit"] = mixRateLimit;
        }
        if (!rlnStateMembership.is_null()) {
            out["membership"] = rlnStateMembership;
        }
        if (rlnStateGrantedRate) {
            out["grantedRateLimit"] = *rlnStateGrantedRate;
        }
        if (!rlnStateMixMembership.is_null()) {
            out["mixMembership"] = rlnStateMixMembership;
        }
        if (rlnStateGrantedMixRate) {
            out["grantedMixRateLimit"] = *rlnStateGrantedMixRate;
        }
        if (rlnStateConfig.hasMix()) {
            out["mixRegistryId"] = rlnStateConfig.mixRegistryId;
            out["mixRlnIdentifier"] = rlnStateConfig.mixRlnIdentifier;
            out["mixEpochSizeSec"] = rlnStateConfig.mixEpochSizeSec;
            out["mixMaxEpochGap"] = rlnStateConfig.mixMaxEpochGap;
        }
        if (rlnStateConfig.hasAllocator()) {
            out["allocatorDiscover"] = rlnStateConfig.allocatorDiscover;
            if (!rlnStateConfig.allocatorPeerId.empty()) {
                out["allocatorPeerId"] = rlnStateConfig.allocatorPeerId;
                out["allocatorMultiaddr"] = rlnStateConfig.allocatorMultiaddr;
            }
            out["allocatorAuthType"] = rlnStateConfig.allocatorAuthType;
        }
    }
    return {true, std::move(out)};
}

const char* rlnStateName(RlnState state)
{
    switch (state) {
    case RlnState::Disabled: return "Disabled";
    case RlnState::Initializing: return "Initializing";
    case RlnState::Ready: return "Ready";
    case RlnState::Failed: return "Failed";
    case RlnState::Stopped: return "Stopped";
    }
    return "Disabled";
}

void DeliveryModuleImpl::setRlnState(RlnState state, const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(rlnStateMutex);
        if (rlnStateValue == state && rlnStateMessage == message) {
            return;
        }
        rlnStateValue = state;
        rlnStateMessage = message;
    }
    const char* name = rlnStateName(state);
    fprintf(stderr, "DeliveryModuleImpl: rln %s%s%s\n", name,
            message.empty() ? "" : ": ", message.c_str());
    rlnStateChanged(name, message, currentTimestampNs());
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

    // A host sharing the backend with other consumers owns its configuration
    // and lifetime; this module then only checks that what it will prove with
    // fits the chain.
    if (!rlnConfig->manageBackend) {
        return checkRlnRateBounds(*rlnConfig);
    }

    // The allocator client dials through libp2p_module. If that is not up,
    // provisioning falls back to a funded registration: a notice, not a failure.
    if (rlnConfig->hasAllocator()) {
        const std::string failure = libp2pBringUp ? libp2pBringUp->ensure() : "no libp2p_module client";
        if (!failure.empty()) {
            char notice[768];
            snprintf(notice, sizeof(notice),
                     "libp2p_module not up (%s); no allocator can be "
                     "dialled (discover=%s, configured=%s), so the RLN module's provisioning "
                     "falls back to a funded registration",
                     failure.c_str(), rlnConfig->allocatorDiscover ? "true" : "false",
                     rlnConfig->allocatorPeerId.empty() ? "none" : rlnConfig->allocatorPeerId.c_str());
            fprintf(stderr, "DeliveryModuleImpl: %s\n", notice);
            disco_trace::traceLine(notice);
        }
    }

    // A node that mounts RLN over a stopped backend Ignores every inbound RLN
    // message. One start names every registry the node takes a membership on.
    const std::string startFailure = rlnBridge->startBackend(rlnBackendStartConfig(*rlnConfig));
    if (!startFailure.empty()) {
        return "rln module start failed: " + startFailure;
    }
    rlnBackendRunning = true;
    if (std::string refused = checkRlnRateBounds(*rlnConfig); !refused.empty()) {
        // The module would keep provisioning, and refusing, in the background.
        (void)rlnBridge->stopBackend();
        rlnBackendRunning = false;
        return refused;
    }
    return {};
}

// A registry entry's `allocator` block: the provider when the preset names
// one, else the payload, else neither (a `peer-` auth vector).
static nlohmann::json allocatorBlock(const DeliveryRlnConfig& cfg)
{
    nlohmann::json block{{"discover", cfg.allocatorDiscover}, {"auth_type", cfg.allocatorAuthType}};
    if (!cfg.allocatorPeerId.empty()) {
        block["peer_id"] = cfg.allocatorPeerId;
        block["multiaddr"] = cfg.allocatorMultiaddr;
    }
    if (!cfg.allocatorAuthProvider.empty()) {
        block["auth_provider"] = cfg.allocatorAuthProvider;
    } else if (!cfg.allocatorAuthPayload.empty()) {
        block["auth_payload"] = cfg.allocatorAuthPayload;
    }
    if (!cfg.allocatorAuthArgs.empty()) {
        block["auth_args"] = cfg.allocatorAuthArgs;
    }
    return block;
}

std::string rlnBackendStartConfig(const DeliveryRlnConfig& cfg)
{
    // The top level carries the relay registry's epoch size, and its gap and
    // rate when set: the module's defaults for a registry that sets none.
    nlohmann::json startCfg{{"epoch_size_sec", cfg.epochSizeSec},
                            {"registries", nlohmann::json::array()}};
    if (cfg.maxEpochGap != 0) {
        startCfg["max_epoch_gap"] = cfg.maxEpochGap;
    }
    // A registry relay and mix share is named once, by the relay entry, at
    // the one rate the preset agreed on: a second entry with the same
    // registry_id would overwrite the first in the module.
    const bool shared = cfg.sharesRegistry();
    if (cfg.needsRelayMembership()) {
        nlohmann::json relay = cfg.hasAllocator()
            ? nlohmann::json{{"registry_id", cfg.registryId}, {"allocator", allocatorBlock(cfg)}}
            : nlohmann::json(cfg.registryId);
        startCfg["registries"].push_back(std::move(relay));
        const uint64_t rateLimit = shared ? cfg.sharedRateLimit() : cfg.rateLimit;
        if (rateLimit != 0) {
            startCfg["rate_limit"] = rateLimit;
        }
    }
    if (cfg.hasMix() && !shared) {
        // Its own epoch size and gap — what native Mix's proof plugin runs
        // at — and its own rate; the relay defaults do not apply to it.
        nlohmann::json mix{{"registry_id", cfg.mixRegistryId},
                           {"epoch_size_sec", cfg.mixEpochSizeSec},
                           {"max_epoch_gap", cfg.mixMaxEpochGap}};
        if (cfg.mixRateLimit != 0) {
            mix["rate_limit"] = cfg.mixRateLimit;
        }
        if (cfg.hasAllocator()) {
            mix["allocator"] = allocatorBlock(cfg);
        }
        startCfg["registries"].push_back(std::move(mix));
    }
    return startCfg.dump();
}

std::string DeliveryModuleImpl::checkRlnRateBounds(const DeliveryRlnConfig& cfg)
{
    struct Named {
        const char* what;
        const std::string& registryId;
        const std::string& rlnIdentifier;
        uint64_t rate;
    };
    const Named named[] = {
        {"rate-limit", cfg.registryId, cfg.rlnIdentifier,
         cfg.needsRelayMembership() ? cfg.rateLimit : 0},
        {"mix-rate-limit", cfg.mixRegistryId, cfg.mixRlnIdentifier, cfg.mixRateLimit},
    };
    for (const Named& n : named) {
        if (n.rate == 0 || n.registryId.empty()) {
            continue;
        }
        nlohmann::json params;
        const std::string failure =
            rlnBridge->readRegistryParameters(n.registryId, n.rlnIdentifier, params);
        if (!failure.empty()) {
            // Bounds that cannot be read now are not a refusal: the module
            // checks them again before it registers.
            fprintf(stderr, "DeliveryModuleImpl: registry %s bounds unavailable (%s)\n",
                    n.registryId.c_str(), failure.c_str());
            continue;
        }
        const auto bound = [&params](const char* key) -> uint64_t {
            auto it = params.find(key);
            return it != params.end() && it->is_number_unsigned() ? it->get<uint64_t>() : 0;
        };
        const uint64_t min = bound("min_rate_limit");
        const uint64_t max = bound("max_rate_limit");
        if ((min != 0 && n.rate < min) || (max != 0 && n.rate > max)) {
            return "registry " + n.registryId + " admits rates " + std::to_string(min) + "..="
                   + std::to_string(max) + "; preset " + n.what + " is " + std::to_string(n.rate);
        }
    }
    return {};
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
