#include "delivery_module_plugin.h"
#include <algorithm>
#include <atomic>
#include <cctype>
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
#include <boost/beast/core/detail/base64.hpp>

#include "api_call_handler.h"
extern "C" {
#include <liblogosdelivery.h>
// Kernel tier: unstable, may change without a deprecation cycle. Only
// waku_store_query is consumed from it; everything else goes through the
// stable surface above.
#include <liblogosdelivery_kernel.h>
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
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + static_cast<int64_t>(ts.tv_nsec);
}

// message_received: JSON array of byte values.
std::vector<uint8_t> decodeByteArrayPayload(const nlohmann::json& payloadValue) {
    if (!payloadValue.is_array()) {
        return {};
    }
    std::vector<uint8_t> payloadBytes;
    payloadBytes.reserve(payloadValue.size());
    for (const auto& val : payloadValue) {
        if (!val.is_number_integer()) {
            return {};
        }
        auto byte = val.get<int64_t>();
        if (byte < 0 || byte > 255) {
            return {};
        }
        payloadBytes.push_back(static_cast<uint8_t>(byte));
    }
    return payloadBytes;
}

// channel_message_received: base64 string.
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
} // namespace

void DeliveryModuleImpl::start_callback(int callerRet, char* msg, size_t len, void* userData)
{
    if (callerRet == RET_STALE_WARN) {
        return;
    }

    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) return;
    impl->nodeStarted(callerRet == RET_OK,
                      (msg && len > 0) ? std::string(msg, len) : std::string(),
                      currentTimestampNs());
}

void DeliveryModuleImpl::stop_callback(int callerRet, char* msg, size_t len, void* userData)
{
    if (callerRet == RET_STALE_WARN) {
        return;
    }

    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) return;
    impl->nodeStopped(callerRet == RET_OK,
                      (msg && len > 0) ? std::string(msg, len) : std::string(),
                      currentTimestampNs());
}

DeliveryModuleImpl::DeliveryModuleImpl() : deliveryCtx(nullptr), deliveryCtxHandle(nullptr)
{
    fprintf(stderr, "DeliveryModuleImpl: Initializing...\n");
    fprintf(stderr, "DeliveryModuleImpl: Initialized successfully\n");
}

DeliveryModuleImpl::~DeliveryModuleImpl()
{
    if (deliveryCtxHandle) {
        // Frees the handle and stops the node, tearing down the event
        // listeners registered against it along the way.
        logosdelivery_ctx_destroy(static_cast<LogosDeliveryCtx*>(deliveryCtxHandle));
        deliveryCtxHandle = nullptr;
        deliveryCtx = nullptr;
    }
}

void DeliveryModuleImpl::event_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    fprintf(stderr, "DeliveryModuleImpl::event_callback called with ret: %d\n", callerRet);

    DeliveryModuleImpl* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid userData\n");
        return;
    }

    if (msg && len > 0) {
        std::string message(msg, len);
        fprintf(stderr, "DeliveryModuleImpl::event_callback message: %s\n", message.c_str());

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

                std::vector<uint8_t> payloadBytes;
                if (msgObj.contains("payload")) {
                    payloadBytes = decodeByteArrayPayload(msgObj["payload"]);
                }

                int64_t msgTimestamp = static_cast<int64_t>(msgObj.value("timestamp", 0.0));
                impl->messageReceived(hash, topic, payloadBytes, msgTimestamp);

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

// Defaults the node's storage directory to the host's per-instance path, so
// side-by-side instances don't share upstream's cwd-relative "./data". The
// path goes where each config shape accepts it: kernelConf when present,
// messagingOverrides (created if needed) for the layered shapes, top level
// for the legacy flat shape.
static std::optional<std::string> applyConfigDefaults(const std::string& cfg,
                                                      const std::string& persistencePath)
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

    if (!persistencePath.empty()) {
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
        if (target && !findKey(*target, {"localstoragepath", "local-storage-path"})) {
            (*target)["localStoragePath"] = persistencePath + "/data";
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

    auto cfgWithDefaults = applyConfigDefaults(cfg, instancePersistencePath());
    if (!cfgWithDefaults) {
        return {false, {}, "Invalid JSON config"};
    }
    const std::string& cfgWithPorts = *cfgWithDefaults;

    // logosdelivery_ctx_create packs the request struct and turns the decimal
    // context address the FFI reports back into a LogosDeliveryCtx handle.
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
        return {false, {}, "Failed to initiate createNode"};
    }

    fprintf(stderr, "DeliveryModuleImpl: Waiting for createNode callback...\n");

    if (!callbackCtx->sem.try_acquire_for(CALLBACK_TIMEOUT)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);

        fprintf(stderr, "DeliveryModuleImpl: Timeout waiting for createNode callback\n");
        return {false, {}, "Timeout waiting for createNode callback"};
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
        return {false, {}, "Failed to create Delivery context"};
    }

    deliveryCtxHandle = callbackCtx->ctx;
    deliveryCtx = callbackCtx->ctx->ptr;

    fprintf(stderr, "DeliveryModuleImpl: Delivery context created successfully\n");

    for (const char* eventName : kEventNames) {
        if (logosdelivery_add_event_listener(deliveryCtx, eventName, event_callback, this) == 0) {
            fprintf(stderr, "DeliveryModuleImpl: Failed to register listener for event %s\n", eventName);
        }
    }
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::start()
{
    fprintf(stderr, "DeliveryModuleImpl::start called\n");

    if (!deliveryCtx) {
        return {false, {}, "Context not initialized"};
    }

    // Node start can block for a long time (relay reconnect backoff), so return
    // once dispatched. Completion arrives via nodeStarted.
    if (logosdelivery_start_node(deliveryCtx, start_callback, this) != RET_OK) {
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

    if (logosdelivery_stop_node(deliveryCtx, stop_callback, this) != RET_OK) {
        return {false, {}, "failed to initiate stop"};
    }
    return {true, {}};
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
    messageObj["payload"] = base64Encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = callApiRetValue(
        "send",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_send, deliveryCtx,
                    LogosdeliverySendReq{.messageJson = messageJson.c_str()}));

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
        bindApiCall(logosdelivery_subscribe, deliveryCtx,
                    LogosdeliverySubscribeReq{.contentTopicStr = contentTopic.c_str()}));

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
        bindApiCall(logosdelivery_unsubscribe, deliveryCtx,
                    LogosdeliveryUnsubscribeReq{.contentTopicStr = contentTopic.c_str()}));

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
        bindApiCall(waku_store_query, deliveryCtx,
                    WakuStoreQueryReq{.jsonQuery = jsonQuery.c_str(),
                                      .peerAddr = peerAddr.c_str(),
                                      .timeoutMs = static_cast<int32_t>(timeoutMs)}));

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
        bindApiCall(logosdelivery_channel_create, deliveryCtx,
                    LogosdeliveryChannelCreateReq{.channelIdStr = channelId.c_str(),
                                                  .contentTopicStr = contentTopic.c_str(),
                                                  .senderIdStr = senderId.c_str()}));

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
        bindApiCall(logosdelivery_channel_exists, deliveryCtx,
                    LogosdeliveryChannelExistsReq{.channelIdStr = channelId.c_str()}));

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
    messageObj["payload"] = base64Encode(payload);
    messageObj["ephemeral"] = false;

    std::string messageJson = messageObj.dump();

    auto outcome = callApiRetValue(
        "channel_send",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_channel_send, deliveryCtx,
                    LogosdeliveryChannelSendReq{.channelIdStr = channelId.c_str(),
                                                .messageJson = messageJson.c_str()}));

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
        bindApiCall(logosdelivery_channel_close, deliveryCtx,
                    LogosdeliveryChannelCloseReq{.channelIdStr = channelId.c_str()}));

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
        bindScalarApiCall(logosdelivery_get_available_node_info_ids, deliveryCtx));

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
        bindApiCall(logosdelivery_get_node_info, deliveryCtx,
                    LogosdeliveryGetNodeInfoReq{.nodeInfoId = nodeInfoId.c_str()}));

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
        bindScalarApiCall(logosdelivery_get_available_configs, deliveryCtx));

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
        bindApiCall(logosdelivery_get_node_info, deliveryCtx,
                    LogosdeliveryGetNodeInfoReq{.nodeInfoId = "Metrics"}));

    if (!outcome.success || !outcome.value.is_string()) {
        fprintf(stderr, "DeliveryModuleImpl: collectOpenMetricsText failed to read Metrics node info: %s\n",
                outcome.error.c_str());
        return "";
    }

    // Hand the exposition text back verbatim; the openmetrics module parses it,
    // injects the module="delivery_module" label, and merges it with others.
    return outcome.value.get<std::string>();
}
