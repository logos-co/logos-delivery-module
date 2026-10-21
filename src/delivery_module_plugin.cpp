#include "delivery_module_plugin.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include "base64.h"

#include "discovery_config.h"
#include "service_discovery_plugin.h"
#include "rln_bridge.h"
#include "rln_presets.h"

// Generated at build time from metadata.json#dependencies and
// #optional_dependencies; defines the
// LogosModules aggregate behind LogosModuleContext::modules().
#include "logos_sdk.h"
// The node's API: liblogosdelivery's generated C++ binding (LogosDeliveryCtx).
// The kernel tier (waku_store_query) is methods of the same class.
#include <liblogosdelivery.hpp>
extern "C" {
#include <liblogosdelivery_rln.h>
// Hand-written export, outside the generated binding: initializes the Nim
// runtime (see installRlnPlugin).
const char* logosdelivery_version(void);
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

// A binding call's result as this module's: the reply text as the value on
// success, the reason otherwise.
StdLogosResult toLogos(const Result<std::string>& r) {
    if (r.isErr()) {
        return {false, {}, r.error()};
    }
    return {true, r.value()};
}

StdLogosResult toLogosVoid(const Result<std::string>& r) {
    if (r.isErr()) {
        return {false, {}, r.error()};
    }
    return {true, {}};
}

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

void DeliveryModuleImpl::runLifecycleCall(std::function<void()> call)
{
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    lifecycleThreads.emplace_back(std::move(call));
}

void DeliveryModuleImpl::joinLifecycleCalls()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        threads.swap(lifecycleThreads);
    }
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
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
    // A verb still inside libp2p keeps the plugin's executor thread; leak the
    // object rather than free it under that thread.
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
    // A start or stop still running holds the node, and a failed start joins
    // the bring-up thread itself: finish them first, so that join is not
    // racing the one below.
    joinLifecycleCalls();
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
    unregisterMixRlnBridge(rlnBridge.get());
    // Destroying the context stops the node, tearing down the event
    // listeners registered against it along the way.
    node.reset();

    // After ctx_destroy, unlike RLN above: destroying the node unregisters the
    // plugin's reverse implementations, so no new call can arrive once it
    // returns.
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

void DeliveryModuleImpl::registerEventListeners()
{
    // The listeners run on the library's event thread; the binding decodes the
    // CBOR payload, and the forwarding below cannot throw past it.
    node->addOnMessageQueuedListener([this](const MessageQueuedPayload& e) {
        messageQueued(e.requestId, e.messageHash, currentTimestampNs());
    });
    node->addOnMessageSentListener([this](const MessageSentPayload& e) {
        messageSent(e.requestId, e.messageHash, currentTimestampNs());
    });
    node->addOnMessageErrorListener([this](const MessageErrorPayload& e) {
        messageError(e.requestId, e.messageHash, e.error, currentTimestampNs());
    });
    node->addOnMessagePropagatedListener([this](const MessagePropagatedPayload& e) {
        messagePropagated(e.requestId, e.messageHash, currentTimestampNs());
    });
    node->addOnMessageReceivedListener([this](const MessageReceivedPayload& e) {
        messageReceived(e.messageHash, e.message.contentTopic, e.message.payload, e.source,
                        e.message.timestamp);
    });
    node->addOnConnectionStatusChangeListener([this](const ConnectionStatusChangePayload& e) {
        connectionStateChanged(e.connectionStatus, currentTimestampNs());
    });
    node->addOnChannelMessageReceivedListener([this](const ChannelMessageReceivedPayload& e) {
        channelMessageReceived(e.channelId, e.senderId, e.payload, currentTimestampNs());
    });
    node->addOnChannelMessageSentListener([this](const ChannelMessageSentPayload& e) {
        channelMessageSent(e.channelId, e.requestId, currentTimestampNs());
    });
    node->addOnChannelMessageErrorListener([this](const ChannelMessageErrorPayload& e) {
        channelMessageError(e.channelId, e.requestId, e.error, currentTimestampNs());
    });
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

    if (node) {
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
    rlnBridge->init(&modules().liblogos_rln_module);

    if (rlnPreset.enabled) {
        DeliveryRlnConfig fromPreset;
        fromPreset.manageBackend = rlnPreset.manageBackend;
        fromPreset.registryId = rlnPreset.registryId;
        fromPreset.rlnIdentifier = rlnPreset.rlnIdentifier;
        fromPreset.epochSizeSec = rlnPreset.epochSizeSec;
        fromPreset.maxEpochGap = rlnPreset.maxEpochGap;
        if (std::string failure = installRlnPlugin(fromPreset); !failure.empty()) {
            return {false, {}, failure};
        }
        setRlnState("Initializing", {});
    }

    // create encodes the config, waits for the ctor, and owns the context it
    // returns; one that times out is destroyed by the binding when it settles.
    fprintf(stderr, "DeliveryModuleImpl: Waiting for createNode...\n");
    auto created = LogosDeliveryCtx::create(cfgWithPorts, CALLBACK_TIMEOUT);
    if (created.isErr()) {
        fprintf(stderr, "DeliveryModuleImpl: createNode failed: %s\n", created.error().c_str());
        return releaseAndFail("Failed to create Delivery context: " + created.error());
    }
    node = created.take();

    fprintf(stderr, "DeliveryModuleImpl: Delivery context created successfully\n");

    // ctx_create initializes the Nim runtime. Install the process-global Mix
    // callback only after that initialization and after this instance owns a
    // valid context.
    if (!registerMixRlnBridge(rlnBridge.get())) {
        return releaseAndFail("Failed to install the Mix RLN callback");
    }

    registerEventListeners();

    // Only the node knows whether it wants a discovery plugin and which DHT
    // peers its config resolved; ask it.
    const StdLogosResult result = toLogos(node->get_discovery_requirements());
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
    if (!node) {
        return "context not initialized";
    }

    // libp2p is not contacted here: outbound calls fail on the Qt main thread.
    // Without a framework (unit tests) there is no modules(); pass null.
    Libp2pModule* libp2p = isContextReady() ? &modules().libp2p_module : nullptr;
    discoPlugin = std::make_unique<DeliveryServiceDiscoveryPlugin>(libp2p, libp2pConfig);

    // The node calls back into discoPlugin until its context is destroyed, so
    // it must outlive the context -- hence a member, not a local. Registering
    // is synchronous: nothing waits on a reply.
    const std::string failure = discoPlugin->registerOn(*node);
    if (!failure.empty()) {
        return failure;
    }

    fprintf(stderr, "DeliveryModuleImpl: service discovery plugin installed\n");
    return {};
}

StdLogosResult DeliveryModuleImpl::start()
{
    fprintf(stderr, "DeliveryModuleImpl::start called\n");

    if (!node) {
        return {false, {}, "Context not initialized"};
    }

    // Node start can block for a long time (relay reconnect backoff), so return
    // once dispatched. Completion arrives via nodeStarted.
    runLifecycleCall([this] {
        const auto started = node->start_node();
        nodeStarted(started.isOk(), started.isOk() ? started.value() : started.error(),
                    currentTimestampNs());
        if (started.isOk()) {
            return;
        }
        // A failed start leaves the node half up: stop it again, and the RLN
        // backend this module started for it -- the same two halves as stop().
        const auto stopped = node->stop_node();
        nodeStopped(stopped.isOk(), stopped.isOk() ? stopped.value() : stopped.error(),
                    currentTimestampNs());
        stopRlnBackend();
    });
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::stop()
{
    fprintf(stderr, "DeliveryModuleImpl::stop called\n");

    if (!node) {
        return {false, {}, "Context not initialized"};
    }

    runLifecycleCall([this] {
        const auto stopped = node->stop_node();
        nodeStopped(stopped.isOk(), stopped.isOk() ? stopped.value() : stopped.error(),
                    currentTimestampNs());
    });
    stopRlnBackend();
    return {true, {}};
}

void DeliveryModuleImpl::stopRlnBackend()
{
    std::lock_guard<std::mutex> lock(rlnBackendStopMutex);
    // This module started the RLN backend, so it stops it too. Stopping one
    // that is still starting would race the bring-up thread.
    joinRlnBringUp();
    const auto config = rlnConfigSnapshot();
    if (config->enabled && config->manageBackend && rlnBridge->enabled()) {
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

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot send message - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    nlohmann::json messageObj;
    messageObj["contentTopic"] = contentTopic;
    messageObj["payload"] = delivery_base64::encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = toLogos(node->send(messageJson));

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

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot subscribe - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = toLogosVoid(node->subscribe(contentTopic));

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

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot unsubscribe - context not initialized.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = toLogosVoid(node->unsubscribe(contentTopic));

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

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot run store query - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    // timeoutMs bounds the query on the FFI side. The binding waits its
    // context-wide timeout (CALLBACK_TIMEOUT) for the reply, with no per-call
    // override: a query allowed longer than that is cut off here first.
    auto outcome = toLogos(node->waku_store_query(jsonQuery, peerAddr,
                                                  static_cast<int32_t>(timeoutMs)));

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

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot create channel - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    // Zero cipher callbacks and user data: an unencrypted channel.
    auto outcome = toLogos(node->channel_create(channelId, contentTopic, senderId,
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

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot query channel - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = toLogos(node->channel_exists(channelId));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel exists failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::channelSend(const std::string& channelId, const std::vector<uint8_t>& payload)
{
    fprintf(stderr, "DeliveryModuleImpl::channelSend called with channelId: %s\n", channelId.c_str());

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot send channel message - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    nlohmann::json messageObj;
    messageObj["payload"] = delivery_base64::encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = toLogos(node->channel_send(channelId, messageJson));

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

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot close channel - context not initialized.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = toLogosVoid(node->channel_close(channelId));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel close failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getAvailableNodeInfoIDs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableNodeInfoIDs called\n");

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available node info IDs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = toLogos(node->get_available_node_info_ids());

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available node info IDs failed, reason: %s\n", outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getNodeInfo(const std::string& nodeInfoId) {
    fprintf(stderr, "DeliveryModuleImpl::getNodeInfo called with nodeInfoId: %s\n", nodeInfoId.c_str());

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get node info - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = toLogos(node->get_node_info(nodeInfoId));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get node info failed for ID: %s, reason: %s\n",
                nodeInfoId.c_str(), outcome.error.c_str());
    }

    return outcome;
}

StdLogosResult DeliveryModuleImpl::getConnectionStatus() {
    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get connection status - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = toLogos(node->get_connection_status());

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get connection status failed, reason: %s\n",
                outcome.error.c_str());
    }

    return outcome;
}

StdLogosResult DeliveryModuleImpl::getAvailableConfigs() {
    fprintf(stderr, "DeliveryModuleImpl::getAvailableConfigs called\n");

    if (!node) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get available configs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = toLogos(node->get_available_configs());

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get available configs failed, reason: %s\n", outcome.error.c_str());
    }

    return outcome;
}

std::string DeliveryModuleImpl::collectOpenMetricsText()
{
    if (!node) {
        // No node yet — empty document; the openmetrics scraper renders nothing
        // for this module rather than treating the scrape as a hard error.
        return "";
    }

    auto outcome = toLogos(node->get_node_info("Metrics"));

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

    // A host sharing the backend with Mix owns its configuration and lifetime.
    if (!rlnConfig->manageBackend) return {};

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

    if (!node) {
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
    if (!node) return {false, {}, "Context not initialized"};
    return toLogos(node->waku_mix_get_peer_record());
}

StdLogosResult DeliveryModuleImpl::addMixPeer(const std::string& recordJson) {
    if (!node) return {false, {}, "Context not initialized"};
    return toLogos(node->waku_mix_add_peer(recordJson));
}
