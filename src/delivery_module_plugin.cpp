#include "delivery_module_plugin.h"
#include <cstdio>
#include <ctime>
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
} // namespace

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

        nlohmann::json jsonObj;
        try {
            jsonObj = nlohmann::json::parse(message);
        } catch (const nlohmann::json::parse_error&) {
            fprintf(stderr, "DeliveryModuleImpl::event_callback: Invalid JSON\n");
            return;
        }

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
                auto& payloadValue = msgObj["payload"];
                if (payloadValue.is_array()) {
                    payloadBytes.reserve(payloadValue.size());
                    for (const auto& val : payloadValue) {
                        payloadBytes.push_back(static_cast<uint8_t>(val.get<int>()));
                    }
                } else if (payloadValue.is_string()) {
                    payloadBytes = base64Decode(payloadValue.get<std::string>());
                }
            }

            int64_t msgTimestamp = static_cast<int64_t>(msgObj.value("timestamp", 0.0));
            impl->messageReceived(hash, topic, payloadBytes, msgTimestamp);

        } else if (eventType == "connection_status_change") {
            impl->connectionStateChanged(
                jsonObj.value("connectionStatus", ""),
                timestamp);

        } else {
            fprintf(stderr, "DeliveryModuleImpl::event_callback: Unknown event type: %s\n", eventType.c_str());
        }
    }
}

// Default every listening port (tcpPort, discv5UdpPort, restPort,
// metricsServerPort, websocketPort) to 0 so the OS assigns an ephemeral port
// when the caller did not pin a specific value. Caller-supplied ports are
// preserved so fleet configs that pin ports keep working. logos-delivery now
// accepts port 0 (status-im/nim-confutils#146), which makes this work.
// See logos-delivery-module#18.
static std::optional<std::string> applyPortDefaults(const std::string& cfg)
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

    auto cfgWithDefaults = applyPortDefaults(cfg);
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
        fprintf(stderr, "DeliveryModuleImpl: Cannot start Delivery - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "start",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_start_node, deliveryCtx));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Start failed: %s\n", outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Delivery start completed with success\n");
    return outcome;
}

StdLogosResult DeliveryModuleImpl::stop()
{
    fprintf(stderr, "DeliveryModuleImpl::stop called\n");

    if (!deliveryCtx) {
        fprintf(stderr, "DeliveryModuleImpl: Cannot stop Delivery - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "stop",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_stop_node, deliveryCtx));

    if (!outcome.success) {
        fprintf(stderr, "DeliveryModuleImpl: Stop failed: %s\n", outcome.error.c_str());
    }

    fprintf(stderr, "DeliveryModuleImpl: Delivery stop completed with success\n");
    return outcome;
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
