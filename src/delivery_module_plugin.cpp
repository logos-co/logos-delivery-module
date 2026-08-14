#include "delivery_module_plugin.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

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

#ifdef _WIN32
// liblogosdelivery dlopens optional dependencies by BARE NAME -- libpq.dll for
// the Postgres archive driver is the one that bites today:
//
//     DeliveryModuleImpl::createNode called
//     could not load: libpq.dll          <- 4ms later
//     ...createNode then never completes, and the caller sees a 20s timeout
//
// The Windows loader resolves a bare name against the EXECUTABLE's directory,
// the system directories and PATH -- never against the directory of the DLL
// doing the loading. Our libpq.dll ships INSIDE the package, next to this
// plugin, so none of those find it.
//
// Copying it beside the host executable would work and is wrong: a module is
// self-contained, and its dependencies must not have to be scattered into a
// directory the module does not own (nor collide there with another module's
// copy). Instead, add our own directory to the search order.
//
// SetDllDirectoryW rather than the alternatives:
//   - AddDllDirectory needs SetDefaultDllDirectories(...USER_DIRS), which drops
//     PATH from the search order process-wide -- a much larger blast radius for
//     no gain here.
//   - Preloading libpq.dll by full path (so a later bare-name load matches the
//     already-loaded module) fixes exactly one library; this fixes every
//     bare-name dependency the node may reach for.
// It is process-global, but each module runs in its own logos_host process, so
// the effect is scoped to this module. It also drops the CWD from the search
// order, which is a small hardening win.
void addOwnDirectoryToDllSearchPath()
{
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&addOwnDirectoryToDllSearchPath),
                &self)) {
            fprintf(stderr, "DeliveryModuleImpl: GetModuleHandleExW failed (%lu); "
                            "bare-name dependencies may not resolve\n", GetLastError());
            return;
        }

        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
            if (n == 0) {
                fprintf(stderr, "DeliveryModuleImpl: GetModuleFileNameW failed (%lu)\n", GetLastError());
                return;
            }
            if (n < path.size()) {
                path.resize(n);
                break;
            }
            path.resize(path.size() * 2);  // truncated, retry with room
        }

        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return;
        }
        path.resize(slash);

        if (!SetDllDirectoryW(path.c_str())) {
            fprintf(stderr, "DeliveryModuleImpl: SetDllDirectoryW failed (%lu)\n", GetLastError());
        }
    });
}
#else
void addOwnDirectoryToDllSearchPath() {}
#endif

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
    // std::chrono rather than clock_gettime(CLOCK_REALTIME): the POSIX call is
    // not available on mingw (neither the function nor CLOCK_REALTIME is
    // declared), which broke the Windows cross-build. system_clock is the
    // portable spelling of the same wall-clock reading.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
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

// Signature is dictated by LogosDeliveryScalarRawFn, whose payload parameter is
// `char*` (non-const) -- start/stop keep the pointer+length convention while the
// argument-taking entry points moved to the split reply/err_msg one.
void DeliveryModuleImpl::start_callback(int callerRet, char* msg, size_t len, void* userData)
{
    auto* impl = static_cast<DeliveryModuleImpl*>(userData);
    if (!impl) return;
    impl->nodeStarted(callerRet == RET_OK,
                      (msg && len > 0) ? std::string(msg, len) : std::string(),
                      currentTimestampNs());
}

void DeliveryModuleImpl::stop_callback(int callerRet, char* msg, size_t len, void* userData)
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
        logosdelivery_destroy(deliveryCtx);
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

    // Before the node dlopens anything: liblogosdelivery reaches for optional
    // dependencies (libpq.dll) by bare name, which the loader will not find in
    // our own package directory without this.
    addOwnDirectoryToDllSearchPath();

    auto cfgWithDefaults = applyConfigDefaults(cfg, instancePersistencePath());
    if (!cfgWithDefaults) {
        return {false, {}, "Invalid JSON config"};
    }
    const std::string& cfgWithPorts = *cfgWithDefaults;

    struct CallbackContext {
        std::binary_semaphore sem{0};
        int callerRet{RET_ERR};
        std::string message;
        void* ctx{nullptr};
    };

    static std::mutex pendingMutex;
    static std::unordered_map<void*, std::shared_ptr<CallbackContext>> pendingContexts;

    auto callbackCtx = std::make_shared<CallbackContext>();
    void* callbackKey = static_cast<void*>(callbackCtx.get());

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts[callbackKey] = callbackCtx;
    }

    // create_node no longer RETURNS the context. It arrives here as `ctx_addr`,
    // a decimal string holding the pointer value, and only on success -- see the
    // generated logosdelivery_create_trampoline, which parses it the same way.
    auto callback = +[](int callerRet, const char* ctxAddr, const char* errMsg, void* userData) {
        fprintf(stderr, "DeliveryModuleImpl::createNode callback called with ret: %d\n", callerRet);

        // A progress notification, not a completion. Releasing the semaphore on
        // it would hand createNode a context that does not exist yet; a terminal
        // RET_OK/RET_ERR always follows.
        if (callerRet == NIMFFI_RET_STALE_WARN) {
            return;
        }

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
        if (callerRet == RET_OK) {
            char* endp = nullptr;
            unsigned long long addr = ctxAddr ? std::strtoull(ctxAddr, &endp, 10) : 0ULL;
            if (ctxAddr && *ctxAddr && endp && *endp == '\0') {
                callbackCtx->ctx = reinterpret_cast<void*>(static_cast<uintptr_t>(addr));
            } else {
                callbackCtx->callerRet = RET_ERR;
                callbackCtx->message = "create returned a non-numeric context address";
            }
        } else if (errMsg && *errMsg) {
            callbackCtx->message = errMsg;
            fprintf(stderr, "DeliveryModuleImpl::createNode callback message: %s\n", callbackCtx->message.c_str());
        }

        callbackCtx->sem.release();
    };

    LogosdeliveryCreateNodeCtorReq createReq{};
    createReq.configJson = cfgWithPorts.c_str();
    (void)logosdelivery_create_node(&createReq, callback, callbackKey);

    fprintf(stderr, "DeliveryModuleImpl: Waiting for createNode callback...\n");

    if (!callbackCtx->sem.try_acquire_for(CALLBACK_TIMEOUT)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);

        deliveryCtx = nullptr;

        fprintf(stderr, "DeliveryModuleImpl: Timeout waiting for createNode callback\n");
        return {false, {}, "Timeout waiting for createNode callback"};
    }

    deliveryCtx = callbackCtx->ctx;

    if (callbackCtx->callerRet != RET_OK || deliveryCtx == nullptr) {
        if (!callbackCtx->message.empty()) {
            fprintf(stderr, "DeliveryModuleImpl: createNode callback error: %s\n", callbackCtx->message.c_str());
        }

        deliveryCtx = nullptr;

        fprintf(stderr, "DeliveryModuleImpl: Failed to create Delivery context\n");
        return {false, {}, "Failed to create Delivery context"};
    }

    fprintf(stderr, "DeliveryModuleImpl: Delivery context created successfully\n");

    // The single global event callback is gone; events are delivered through a
    // per-event-name listener registry instead. Register the same dispatcher for
    // every name this module handles -- the payload still carries its own
    // "eventType" field (emitEvent sends `newJsonEvent("message_received", …)`),
    // so event_callback's existing parsing is unchanged.
    for (const char* eventName : {
             "onMessageSent",
             "onMessageError",
             "onMessagePropagated",
             "onMessageReceived",
             "onConnectionStatusChange",
             "onChannelMessageReceived",
             "onChannelMessageSent",
             "onChannelMessageError",
         }) {
        if (logosdelivery_add_event_listener(deliveryCtx, eventName, event_callback, this) == 0) {
            // A zero id means the registration did not take, which would leave
            // this event silently undelivered for the life of the node.
            fprintf(stderr, "DeliveryModuleImpl: failed to register listener for %s\n", eventName);
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
