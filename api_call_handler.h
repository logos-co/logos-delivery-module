#pragma once

#include <QString>
#include <chrono>
#include <memory>
#include <mutex>
#include <semaphore>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "QExpected.h"

extern "C" {
#include <liblogosdelivery.h>
}

namespace {
using DeliveryCallback = void (*)(int, const char*, size_t, void*);

struct CallbackPayload {
    int callerRet{RET_ERR};
    QString message;
};

template <typename Func, typename... BoundArgs>
auto bindApiCall(Func func, void* ctx, BoundArgs&&... boundArgs)
{
    return [func, ctx, ... args = std::forward<BoundArgs>(boundArgs)](DeliveryCallback callback, void* userData) {
        return func(ctx, callback, userData, args...);
    };
}

template <typename BoundInvoke>
QExpected<void> callApiRetVoid(const QString& operationName, std::chrono::seconds timeout, BoundInvoke&& invoke)
{
    struct CallbackContext {
        std::binary_semaphore sem{0};
        CallbackPayload payload;
    };

    static std::mutex pendingMutex;
    static std::unordered_map<void*, std::shared_ptr<CallbackContext>> pendingContexts;

    auto ctx = std::make_shared<CallbackContext>();
    void* callbackKey = static_cast<void*>(ctx.get());

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts[callbackKey] = ctx;
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
            callbackCtx->payload.message = QString::fromUtf8(msg, len);
        }
        callbackCtx->sem.release();
    };

    int startResult = invoke(callback, callbackKey);
    if (startResult != RET_OK) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return QExpected<void>::err(operationName + " failed to initiate");
    }

    if (!ctx->sem.try_acquire_for(timeout)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return QExpected<void>::err(operationName + " callback timeout");
    }

    if (ctx->payload.callerRet != RET_OK) {
        const QString message = ctx->payload.message.isEmpty()
            ? operationName + " failed"
            : ctx->payload.message;
        return QExpected<void>::err(message);
    }

    return QExpected<void>::ok();
}

template <typename TResult, typename BoundInvoke>
QExpected<TResult> callApiRetValue(
    const QString& operationName,
    std::chrono::seconds timeout,
    BoundInvoke&& invoke)
{
    static_assert(std::is_same_v<TResult, QString>, "callApiRetValue only supports QString payload; perform conversions at call site");

    struct CallbackContext {
        std::binary_semaphore sem{0};
        CallbackPayload payload;
    };

    static std::mutex pendingMutex;
    static std::unordered_map<void*, std::shared_ptr<CallbackContext>> pendingContexts;

    auto ctx = std::make_shared<CallbackContext>();
    void* callbackKey = static_cast<void*>(ctx.get());

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts[callbackKey] = ctx;
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
            callbackCtx->payload.message = QString::fromUtf8(msg, len);
        }
        callbackCtx->sem.release();
    };

    int startResult = invoke(callback, callbackKey);
    if (startResult != RET_OK) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return QExpected<TResult>::err(operationName + " failed to initiate");
    }

    if (!ctx->sem.try_acquire_for(timeout)) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingContexts.erase(callbackKey);
        return QExpected<TResult>::err(operationName + " callback timeout");
    }

    if (ctx->payload.callerRet != RET_OK) {
        const QString message = ctx->payload.message.isEmpty()
            ? operationName + " failed"
            : ctx->payload.message;
        return QExpected<TResult>::err(message);
    }

    return QExpected<TResult>::ok(ctx->payload.message);
}
} // namespace
