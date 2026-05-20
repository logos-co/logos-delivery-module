#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <unordered_map>
#include <utility>

#include <logos_result.h>

extern "C" {
#include <liblogosdelivery.h>
}

namespace {
using DeliveryCallback = void (*)(int, const char*, size_t, void*);

struct CallbackPayload {
    int callerRet{RET_ERR};
    std::string message;
};

template <typename Func, typename... BoundArgs>
auto bindApiCall(Func func, void* callbackCtx, BoundArgs&&... boundArgs)
{
    return [func, callbackCtx, ... args = std::forward<BoundArgs>(boundArgs)](DeliveryCallback callback, void* userData) {
        return func(callbackCtx, callback, userData, args...);
    };
}

template <typename BoundInvoke>
StdLogosResult callApiRetVoid(const std::string& operationName, std::chrono::seconds timeout, BoundInvoke&& invoke)
{
    struct CallbackContext {
        std::binary_semaphore sem{0};
        CallbackPayload payload;
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

        callbackCtx->payload.callerRet = callerRet;
        if (msg && len > 0) {
            callbackCtx->payload.message = std::string(msg, len);
        }
        callbackCtx->sem.release();
    };

    int startResult = invoke(callback, callbackKey);
    if (startResult != RET_OK) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return {false, {}, "failed to initiate " + operationName};
    }

    if (!callbackCtx->sem.try_acquire_for(timeout)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return {false, {}, operationName + " callback timeout"};
    }

    if (callbackCtx->payload.callerRet != RET_OK) {
        std::string message = callbackCtx->payload.message.empty()
            ? operationName + " failed"
            : callbackCtx->payload.message;
        return {false, {}, message};
    }

    return {true, {}};
}

template <typename BoundInvoke>
StdLogosResult callApiRetValue(
    const std::string& operationName,
    std::chrono::seconds timeout,
    BoundInvoke&& invoke)
{
    struct CallbackContext {
        std::binary_semaphore sem{0};
        CallbackPayload payload;
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

        callbackCtx->payload.callerRet = callerRet;
        if (msg && len > 0) {
            callbackCtx->payload.message = std::string(msg, len);
        }
        callbackCtx->sem.release();
    };

    int startResult = invoke(callback, callbackKey);
    if (startResult != RET_OK) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return {false, {}, "failed to initiate " + operationName};
    }

    if (!callbackCtx->sem.try_acquire_for(timeout)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return {false, {}, operationName + " callback timeout"};
    }

    if (callbackCtx->payload.callerRet != RET_OK) {
        std::string message = callbackCtx->payload.message.empty()
            ? operationName + " failed"
            : callbackCtx->payload.message;
        return {false, {}, message};
    }

    return {true, callbackCtx->payload.message};
}
} // namespace
