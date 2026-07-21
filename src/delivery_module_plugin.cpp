#include "delivery_module_plugin.h"
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
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

// Verbosity of the plugin's own stderr diagnostics, declared by decreasing
// severity: a line prints when its level does not exceed the current one.
enum class LogLevel { Error, Warn, Info, Debug, Trace };

std::atomic<LogLevel> currentLogLevel{LogLevel::Info};

// Set once DELIVERY_MODULE_LOG_LEVEL has supplied a level, which the createNode
// `logLevel` key must then not overwrite.
std::atomic<bool> logLevelPinnedByEnv{false};

std::optional<LogLevel> parseLogLevel(const std::string& name) {
    std::string upper;
    upper.reserve(name.size());
    for (unsigned char ch : name) {
        upper.push_back(static_cast<char>(std::toupper(ch)));
    }

    if (upper == "ERROR") return LogLevel::Error;
    if (upper == "WARN") return LogLevel::Warn;
    if (upper == "INFO") return LogLevel::Info;
    if (upper == "DEBUG") return LogLevel::Debug;
    if (upper == "TRACE") return LogLevel::Trace;
    return std::nullopt;
}

// The host classifies a forwarded stderr line by looking for these tokens
// anywhere in it (logos-liblogos subprocess_container.cpp); an unprefixed line
// is logged at info.
const char* logLevelPrefix(LogLevel level) {
    switch (level) {
    case LogLevel::Error: return "ERROR: ";
    case LogLevel::Warn:  return "Warning: ";
    case LogLevel::Info:  return "";
    case LogLevel::Debug: return "Debug: ";
    case LogLevel::Trace: return "Trace: ";
    }
    return "";
}

// Composes the whole line before writing it: liblogosdelivery callbacks log
// from their own threads, and a separate write for the prefix could end up on
// stderr apart from the message it belongs to.
[[gnu::format(printf, 2, 3)]]
void logAt(LogLevel level, const char* fmt, ...) {
    if (level > currentLogLevel.load(std::memory_order_relaxed)) return;

    va_list args;
    va_start(args, fmt);
    va_list sizeArgs;
    va_copy(sizeArgs, args);
    const int size = std::vsnprintf(nullptr, 0, fmt, sizeArgs);
    va_end(sizeArgs);

    std::string message;
    if (size > 0) {
        message.resize(static_cast<size_t>(size));
        std::vsnprintf(message.data(), static_cast<size_t>(size) + 1, fmt, args);
    }
    va_end(args);

    fprintf(stderr, "%s%s", logLevelPrefix(level), message.c_str());
}

void initLogLevelFromEnv() {
    std::optional<LogLevel> level;
    if (const char* value = std::getenv("DELIVERY_MODULE_LOG_LEVEL")) {
        level = parseLogLevel(value);
    }

    logLevelPinnedByEnv.store(level.has_value(), std::memory_order_relaxed);
    if (level) {
        currentLogLevel.store(*level, std::memory_order_relaxed);
    }
}

// Takes the plugin's verbosity from the createNode `logLevel` key, shared with
// the node's own logger. An absent or unrecognized value leaves the current
// level in place; a malformed config is reported by applyPortDefaults.
void applyConfigLogLevel(const std::string& cfg) {
    if (logLevelPinnedByEnv.load(std::memory_order_relaxed)) return;

    nlohmann::json cfgObj;
    try {
        cfgObj = nlohmann::json::parse(cfg);
    } catch (const nlohmann::json::parse_error&) {
        return;
    }

    if (!cfgObj.is_object()) return;

    const auto logLevelIt = cfgObj.find("logLevel");
    if (logLevelIt == cfgObj.end() || !logLevelIt->is_string()) return;

    if (auto level = parseLogLevel(logLevelIt->get<std::string>())) {
        currentLogLevel.store(*level, std::memory_order_relaxed);
    }
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
    initLogLevelFromEnv();

    logAt(LogLevel::Info, "DeliveryModuleImpl: Initializing...\n");
    logAt(LogLevel::Info, "DeliveryModuleImpl: Initialized successfully\n");
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
    logAt(LogLevel::Debug, "DeliveryModuleImpl::event_callback called with ret: %d\n", callerRet);

    DeliveryModuleImpl* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) {
        logAt(LogLevel::Error, "DeliveryModuleImpl::event_callback: Invalid userData\n");
        return;
    }

    if (msg && len > 0) {
        std::string message(msg, len);
        logAt(LogLevel::Trace, "DeliveryModuleImpl::event_callback message: %s\n", message.c_str());

        nlohmann::json jsonObj;
        try {
            jsonObj = nlohmann::json::parse(message);
        } catch (const nlohmann::json::parse_error&) {
            logAt(LogLevel::Error, "DeliveryModuleImpl::event_callback: Invalid JSON\n");
            return;
        }

        if (!jsonObj.is_object()) {
            logAt(LogLevel::Error, "DeliveryModuleImpl::event_callback: Invalid JSON\n");
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
            logAt(LogLevel::Error, "DeliveryModuleImpl::event_callback: Unknown event type: %s\n", eventType.c_str());
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
        logAt(LogLevel::Error, "DeliveryModuleImpl: createNode cfg is not valid JSON\n");
        return std::nullopt;
    }

    if (!cfgObj.is_object()) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: createNode cfg is not a JSON object\n");
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
        logAt(LogLevel::Error, "DeliveryModuleImpl: createNode rejected - context already initialized\n");
        return {false, {}, "Context already initialized"};
    }

    applyConfigLogLevel(cfg);

    // Don't log cfg: it can carry sensitive config.
    logAt(LogLevel::Info, "DeliveryModuleImpl::createNode called\n");

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
        logAt(LogLevel::Info, "DeliveryModuleImpl::createNode callback called with ret: %d\n", callerRet);

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
            logAt(LogLevel::Debug, "DeliveryModuleImpl::createNode callback message: %s\n", callbackCtx->message.c_str());
        }

        callbackCtx->sem.release();
    };

    deliveryCtx = logosdelivery_create_node(cfgWithPorts.c_str(), callback, callbackKey);

    logAt(LogLevel::Info, "DeliveryModuleImpl: Waiting for createNode callback...\n");

    if (!callbackCtx->sem.try_acquire_for(CALLBACK_TIMEOUT)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);

        deliveryCtx = nullptr;

        logAt(LogLevel::Error, "DeliveryModuleImpl: Timeout waiting for createNode callback\n");
        return {false, {}, "Timeout waiting for createNode callback"};
    }

    if (callbackCtx->callerRet != RET_OK || deliveryCtx == nullptr) {
        if (!callbackCtx->message.empty()) {
            logAt(LogLevel::Error, "DeliveryModuleImpl: createNode callback error: %s\n", callbackCtx->message.c_str());
        }

        deliveryCtx = nullptr;

        logAt(LogLevel::Error, "DeliveryModuleImpl: Failed to create Delivery context\n");
        return {false, {}, "Failed to create Delivery context"};
    }

    logAt(LogLevel::Info, "DeliveryModuleImpl: Delivery context created successfully\n");

    logosdelivery_set_event_callback(deliveryCtx, event_callback, this);
    return {true, {}};
}

StdLogosResult DeliveryModuleImpl::start()
{
    logAt(LogLevel::Info, "DeliveryModuleImpl::start called\n");

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
    logAt(LogLevel::Info, "DeliveryModuleImpl::stop called\n");

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
    logAt(LogLevel::Debug, "DeliveryModuleImpl::send called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Cannot send message - context not initialized. Call createNode first.\n");
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
        logAt(LogLevel::Error, "DeliveryModuleImpl: Send failed for topic: %s, reason: %s\n",
              contentTopic.c_str(), outcome.error.c_str());
    }

    if (outcome.success && outcome.value.is_string()) {
        logAt(LogLevel::Debug, "DeliveryModuleImpl: Send initiated for topic: %s, with success, requestId: %s\n",
              contentTopic.c_str(), outcome.value.get<std::string>().c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::subscribe(const std::string& contentTopic)
{
    logAt(LogLevel::Debug, "DeliveryModuleImpl::subscribe called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Cannot subscribe - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "subscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_subscribe, deliveryCtx, contentTopic.c_str()));

    if (!outcome.success) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Subscribe failed for topic: %s, reason: %s\n",
              contentTopic.c_str(), outcome.error.c_str());
    }

    logAt(LogLevel::Debug, "DeliveryModuleImpl: Subscribe completed for topic: %s with success\n", contentTopic.c_str());
    return outcome;
}

StdLogosResult DeliveryModuleImpl::unsubscribe(const std::string& contentTopic)
{
    logAt(LogLevel::Debug, "DeliveryModuleImpl::unsubscribe called with contentTopic: %s\n", contentTopic.c_str());

    if (!deliveryCtx) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Cannot unsubscribe - context not initialized.\n");
        return {false, {}, "Context not initialized"};
    }

    auto outcome = callApiRetVoid(
        "unsubscribe",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_unsubscribe, deliveryCtx, contentTopic.c_str()));

    if (!outcome.success) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Unsubscribe failed for topic: %s, reason: %s\n",
              contentTopic.c_str(), outcome.error.c_str());
    }

    logAt(LogLevel::Debug, "DeliveryModuleImpl: Unsubscribe completed for topic: %s with success\n", contentTopic.c_str());
    return outcome;
}

std::string DeliveryModuleImpl::version() const {
    std::string moduleVersion = "1.1.0";
    if (!deliveryCtx) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Cannot get version - context not initialized. Call createNode first.\n");
        return moduleVersion + " (liblogosdelivery version unknown, context not initialized)";
    }

    auto liblogosDeliveryVersion = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, "Version"));

    if (!liblogosDeliveryVersion.success) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Get node info failed getting version, reason: %s\n",
              liblogosDeliveryVersion.error.c_str());
        return moduleVersion + " (liblogosdelivery version unknown)";
    }

    std::string ver = liblogosDeliveryVersion.value.get<std::string>();
    logAt(LogLevel::Info, "DeliveryModuleImpl: Get node info completed for attribute: Version, with success: %s\n", ver.c_str());

    return moduleVersion + " (liblogosdelivery version: " + ver + ")";
}

StdLogosResult DeliveryModuleImpl::getAvailableNodeInfoIDs() {
    logAt(LogLevel::Info, "DeliveryModuleImpl::getAvailableNodeInfoIDs called\n");

    if (!deliveryCtx) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Cannot get available node info IDs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_available_node_info_ids",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_node_info_ids, deliveryCtx));

    if (!outcome.success) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Get available node info IDs failed, reason: %s\n", outcome.error.c_str());
    }
    return outcome;
}

StdLogosResult DeliveryModuleImpl::getNodeInfo(const std::string& nodeInfoId) {
    logAt(LogLevel::Info, "DeliveryModuleImpl::getNodeInfo called with nodeInfoId: %s\n", nodeInfoId.c_str());

    if (!deliveryCtx) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Cannot get node info - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_node_info",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_node_info, deliveryCtx, nodeInfoId.c_str()));

    if (!outcome.success) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Get node info failed for ID: %s, reason: %s\n",
              nodeInfoId.c_str(), outcome.error.c_str());
    }

    return outcome;
}

StdLogosResult DeliveryModuleImpl::getAvailableConfigs() {
    logAt(LogLevel::Info, "DeliveryModuleImpl::getAvailableConfigs called\n");

    if (!deliveryCtx) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Cannot get available configs - context not initialized. Call createNode first.\n");
        return {false, {}, "Context not initialized"};
    }
    auto outcome = callApiRetValue(
        "get_available_configs",
        CALLBACK_TIMEOUT,
        bindApiCall(logosdelivery_get_available_configs, deliveryCtx));

    if (!outcome.success) {
        logAt(LogLevel::Error, "DeliveryModuleImpl: Get available configs failed, reason: %s\n", outcome.error.c_str());
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
        logAt(LogLevel::Error, "DeliveryModuleImpl: collectOpenMetricsText failed to read Metrics node info: %s\n",
              outcome.error.c_str());
        return "";
    }

    // Hand the exposition text back verbatim; the openmetrics module parses it,
    // injects the module="delivery_module" label, and merges it with others.
    return outcome.value.get<std::string>();
}
