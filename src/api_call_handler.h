#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
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
// The reply shape every generated logosdelivery_ctx_* call shares: `reply`
// points at the decoded result on success, `errMsg` carries a failure.
using DeliveryReplyFn = void (*)(int, const char* const*, const char*, void*);

struct CallbackContext {
    std::binary_semaphore sem{0};
    int callerRet{RET_ERR};
    std::string message;
};

// Calls waiting for a reply, keyed by the ticket handed to the FFI as userData.
// A counter rather than the context's address: a timed-out call leaves its
// ticket behind, and a recycled address would let that late reply wake an
// unrelated call.
std::mutex& pendingMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<void*, std::shared_ptr<CallbackContext>>& pendingCalls()
{
    static std::unordered_map<void*, std::shared_ptr<CallbackContext>> calls;
    return calls;
}

void* nextTicket()
{
    static std::atomic<uintptr_t> counter{0};
    return reinterpret_cast<void*>(++counter);
}

// Takes the pending call off the map, so a reply that arrives twice - or after
// the call already timed out - is dropped.
std::shared_ptr<CallbackContext> claimCall(void* ticket)
{
    std::lock_guard<std::mutex> lock(pendingMutex());
    auto it = pendingCalls().find(ticket);
    if (it == pendingCalls().end()) {
        return nullptr;
    }
    auto context = it->second;
    pendingCalls().erase(it);
    return context;
}

void forgetCall(void* ticket)
{
    std::lock_guard<std::mutex> lock(pendingMutex());
    pendingCalls().erase(ticket);
}

// The generated trampolines already drop the RET_STALE_WARN progress ticks, so
// every call here is terminal.
void replyTrampoline(int errCode, const char* const* reply, const char* errMsg, void* userData)
{
    auto context = claimCall(userData);
    if (!context) {
        return;
    }

    context->callerRet = errCode;
    const char* text = (errCode == RET_OK) ? (reply ? *reply : nullptr) : errMsg;
    if (text) {
        context->message = text;
    }
    context->sem.release();
}

// Binds a generated wrapper, func(ctx, args..., onReply, userData), to its
// arguments. The wrapper CBOR-encodes them before returning, so borrowed
// strings need only outlive the call - they do, since it runs inside
// callApiRetValue below.
template <typename Func, typename... Args>
auto bindApiCall(Func func, const LogosDeliveryCtx* ctx, Args... args)
{
    return [func, ctx, args...](void* ticket) {
        return func(ctx, args..., static_cast<DeliveryReplyFn>(replyTrampoline), ticket);
    };
}

// Dispatches a bound call and blocks until its reply arrives. The reply text is
// the result value on success and the error message on failure.
template <typename BoundInvoke>
StdLogosResult callApiRetValue(
    const std::string& operationName,
    std::chrono::seconds timeout,
    BoundInvoke&& invoke)
{
    auto context = std::make_shared<CallbackContext>();
    void* ticket = nextTicket();

    {
        std::lock_guard<std::mutex> lock(pendingMutex());
        pendingCalls()[ticket] = context;
    }

    if (invoke(ticket) != RET_OK) {
        forgetCall(ticket);
        // A local failure (encode, allocation) was already reported through
        // the reply callback, before the wrapper returned.
        return {false, {}, context->message.empty() ? "failed to initiate " + operationName
                                                    : context->message};
    }

    if (!context->sem.try_acquire_for(timeout)) {
        forgetCall(ticket);
        return {false, {}, operationName + " callback timeout"};
    }

    if (context->callerRet != RET_OK) {
        return {false, {}, context->message.empty() ? operationName + " failed" : context->message};
    }

    return {true, context->message};
}

template <typename BoundInvoke>
StdLogosResult callApiRetVoid(
    const std::string& operationName,
    std::chrono::seconds timeout,
    BoundInvoke&& invoke)
{
    auto outcome = callApiRetValue(operationName, timeout, std::forward<BoundInvoke>(invoke));
    if (!outcome.success) {
        return outcome;
    }
    return {true, {}};
}
} // namespace
