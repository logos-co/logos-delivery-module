#include "delivery_module_plugin.h"
#include <algorithm>
#include <cctype>
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
} // namespace

void DeliveryModuleImpl::start_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) return;
    impl->nodeStarted(callerRet == RET_OK,
                      (msg && len > 0) ? std::string(msg, len) : std::string(),
                      currentTimestampNs());
}

void DeliveryModuleImpl::stop_callback(int callerRet, const char* msg, size_t len, void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) return;
    impl->nodeStopped(callerRet == RET_OK,
                      (msg && len > 0) ? std::string(msg, len) : std::string(),
                      currentTimestampNs());
}

DeliveryModuleImpl::DeliveryModuleImpl() : deliveryCtx(nullptr)
{
    fprintf(stderr, "DeliveryModuleImpl: Initializing...\n");
    fprintf(stderr, "DeliveryModuleImpl: Initialized successfully\n");
}

DeliveryModuleImpl::~DeliveryModuleImpl()
{
    if (deliveryCtx) {
        logosdelivery_destroy(deliveryCtx, nullptr, nullptr);
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

// True when cfgObj already carries one of `names`. The upstream JSON conf
// parser keys fields case-insensitively and matches either the Nim field name
// or its CLI `name:` pragma, so a caller may legitimately spell a key several
// ways; matching the same way keeps us from overriding their value.
static bool containsAnyKey(const nlohmann::json& cfgObj,
                           std::initializer_list<const char*> names)
{
    for (const auto& entry : cfgObj.items()) {
        std::string key = entry.key();
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* name : names) {
            if (key == name) return true;
        }
    }
    return false;
}

// Default every listening port (tcpPort, discv5UdpPort, restPort,
// metricsServerPort, websocketPort) to 0 so the OS assigns an ephemeral port
// when the caller did not pin a specific value. Caller-supplied ports are
// preserved so fleet configs that pin ports keep working. logos-delivery now
// accepts port 0 (status-im/nim-confutils#146), which makes this work.
// See logos-delivery-module#18.
//
// Also default the node's storage directory to the per-instance path the host
// provisions for this module. logos-delivery otherwise falls back to "./data"
// (persistency.nim DefaultStoragePath), which is relative to the process
// working directory and therefore identical for every instance launched from
// it — side-by-side instances would share one SQLite file. The path is empty
// when the module runs outside a host that provisions persistence (unit tests
// constructing the impl directly), in which case upstream's default stands.
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

    for (const char* portKey : {
             "tcpPort",
             "discv5UdpPort",
             "restPort",
             "metricsServerPort",
             "websocketPort",
         }) {
        if (!cfgObj.contains(portKey)) {
            cfgObj[portKey] = 0;
        }
    }

    if (!persistencePath.empty()
        && !containsAnyKey(cfgObj, {"localstoragepath", "local-storage-path"})) {
        cfgObj["localStoragePath"] = persistencePath + "/data";
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

    struct CallbackContext {
        std::binary_semaphore sem{0};
        int callerRet{RET_ERR};
        std::string message;
    };

    static std::mutex pendingMutex;
    static std::unordered_map<void*, std::shared_ptr<CallbackContext>> pendingContexts;

    auto callbackCtx = std::make_shared<CallbackContext>();
    void* callbackKey = static_cast<void*>(callbackCtx.get());

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts[callbackKey] = callbackCtx;
    }

    auto callback = +[](int callerRet, const char* msg, size_t len, void* userData) {
        fprintf(stderr, "DeliveryModuleImpl::createNode callback called with ret: %d\n", callerRet);

        std::shared_ptr<CallbackContext> callbackCtx;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            auto it = pendingContexts.find(userData);
            if (it == pendingContexts.end()) {
                return;
            }
            callbackCtx = it->second;
            pendingContexts.erase(it);
        }

        if (!callbackCtx) {
            return;
        }

        callbackCtx->callerRet = callerRet;
        if (msg && len > 0) {
            callbackCtx->message = std::string(msg, len);
            fprintf(stderr, "DeliveryModuleImpl::createNode callback message: %s\n", callbackCtx->message.c_str());
        }

        callbackCtx->sem.release();
    };

    deliveryCtx = logosdelivery_create_node(cfgWithPorts.c_str(), callback, callbackKey);

    fprintf(stderr, "DeliveryModuleImpl: Waiting for createNode callback...\n");

    if (!callbackCtx->sem.try_acquire_for(CALLBACK_TIMEOUT)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);

        deliveryCtx = nullptr;

        fprintf(stderr, "DeliveryModuleImpl: Timeout waiting for createNode callback\n");
        return {false, {}, "Timeout waiting for createNode callback"};
    }

    if (callbackCtx->callerRet != RET_OK || deliveryCtx == nullptr) {
        if (!callbackCtx->message.empty()) {
            fprintf(stderr, "DeliveryModuleImpl: createNode callback error: %s\n", callbackCtx->message.c_str());
        }

        deliveryCtx = nullptr;

        fprintf(stderr, "DeliveryModuleImpl: Failed to create Delivery context\n");
        return {false, {}, "Failed to create Delivery context"};
    }

    fprintf(stderr, "DeliveryModuleImpl: Delivery context created successfully\n");

    logosdelivery_set_event_callback(deliveryCtx, event_callback, this);
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
        bindApiCall(logosdelivery_send, deliveryCtx, messageJson.c_str()));

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
        bindApiCall(logosdelivery_subscribe, deliveryCtx, contentTopic.c_str()));

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
        bindApiCall(logosdelivery_unsubscribe, deliveryCtx, contentTopic.c_str()));

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
                    jsonQuery.c_str(), peerAddr.c_str(), static_cast<int>(timeoutMs)));

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
                    channelId.c_str(), contentTopic.c_str(), senderId.c_str()));

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
        bindApiCall(logosdelivery_channel_exists, deliveryCtx, channelId.c_str()));

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
        bindApiCall(logosdelivery_channel_close, deliveryCtx, channelId.c_str()));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Channel close failed for id: %s, reason: %s\n",
                channelId.c_str(), outcome.error.c_str());
    }
    return outcome;
}

std::string DeliveryModuleImpl::version() const {
    std::string moduleVersion = "1.1.0";
    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot get version - context not initialized. Call createNode first.\n");
        return moduleVersion + " (liblogosdelivery version unknown, context not initialized)";
    }

    auto liblogosDeliveryVersion = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, "Version"));

    if (!liblogosDeliveryVersion.success) {
        fprintf(stderr, "DeliveryModuleImpl: Get node info failed getting version, reason: %s\n",
                liblogosDeliveryVersion.error.c_str());
        return moduleVersion + " (liblogosdelivery version unknown)";
    }

    std::string ver = liblogosDeliveryVersion.value.get<std::string>();
    fprintf(stderr, "DeliveryModuleImpl: Get node info completed for attribute: Version, with success: %s\n", ver.c_str());

    return moduleVersion + " (liblogosdelivery version: " + ver + ")";
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
        bindApiCall(logosdelivery_get_available_node_info_ids, deliveryCtx));

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
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, nodeInfoId.c_str()));

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
        bindApiCall(logosdelivery_get_available_configs, deliveryCtx));

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
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, "Metrics"));

    if (!outcome.success || !outcome.value.is_string()) {
        fprintf(stderr, "DeliveryModuleImpl: collectOpenMetricsText failed to read Metrics node info: %s\n",
                outcome.error.c_str());
        return "";
    }

    // Hand the exposition text back verbatim; the openmetrics module parses it,
    // injects the module="delivery_module" label, and merges it with others.
    return outcome.value.get<std::string>();
}
