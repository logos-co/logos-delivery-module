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
// The two reply shapes of the generated C ABI. Argument-taking exports deliver
// a typed (errCode, reply, errMsg) triple of NUL-terminated strings; the
// no-argument "scalar fast path" exports keep the raw (callerRet, msg, len)
// byte run. Every generated per-call typedef is an alias of one of these.
using DeliveryReplyFn = void (*)(int, const char*, const char*, void*);
using DeliveryScalarFn = void (*)(int, char*, size_t, void*);

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

// RET_STALE_WARN is a progress tick that fires every ~5s on a long call and is
// always followed by a terminal code, so both trampolines ignore it instead of
// completing the call.
void replyTrampoline(int errCode, const char* reply, const char* errMsg, void* userData)
{
    if (errCode == RET_STALE_WARN) {
        return;
    }

    auto context = claimCall(userData);
    if (!context) {
        return;
    }

    context->callerRet = errCode;
    const char* text = (errCode == RET_OK) ? reply : errMsg;
    if (text) {
        context->message = text;
    }
    context->sem.release();
}

void scalarTrampoline(int callerRet, char* msg, size_t len, void* userData)
{
    if (callerRet == RET_STALE_WARN) {
        return;
    }

    auto context = claimCall(userData);
    if (!context) {
        return;
    }

    context->callerRet = callerRet;
    if (msg && len > 0) {
        context->message.assign(msg, len);
    }
    context->sem.release();
}

// Binds an argument-taking export to its request struct; the generated
// signature is func(ctx, onReply, userData, &req). `req` borrows the caller's
// strings, so those must outlive the bound call - they do, since it runs to
// completion inside callApiRetValue below.
template <typename Func, typename Req>
auto bindApiCall(Func func, void* deliveryCtx, Req req)
{
    return [func, deliveryCtx, req](void* ticket) {
        return func(deliveryCtx, static_cast<DeliveryReplyFn>(replyTrampoline), ticket, &req);
    };
}

// Binds a no-argument export: func(ctx, callback, userData).
template <typename Func>
auto bindScalarApiCall(Func func, void* deliveryCtx)
{
    return [func, deliveryCtx](void* ticket) {
        return func(deliveryCtx, static_cast<DeliveryScalarFn>(scalarTrampoline), ticket);
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
        return {false, {}, "failed to initiate " + operationName};
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
