#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include <logos_result.h>

extern "C" {
#include <liblogosdelivery.h>
}

namespace {
struct CallbackPayload {
    int callerRet{RET_ERR};
    std::string message;
};

// liblogosdelivery's generated C ABI has TWO reply conventions, and which one an
// entry point uses is not a style choice we can paper over:
//
//   LogosDeliveryScalarRawFn      (int caller_ret, char* msg, size_t len, void*)
//   LogosDelivery<Op>ReplyFn      (int err_code, const char* reply,
//                                  const char* err_msg, void*)
//
// The first is used by the no-argument calls (start_node, stop_node,
// get_available_*), where the payload is a pointer+length. The second is used by
// everything that takes arguments, where success and failure text arrive in
// SEPARATE parameters. The third parameter therefore means different things in
// the two, which is why a single callback typedef cannot serve both.
//
// Argument-taking entry points also no longer take their arguments positionally:
// each has a generated request struct (`const Logosdelivery<Op>Req*`) whose
// fields are declared in the old positional order.
//
// These traits read both facts off the function pointer type, so every call site
// keeps passing the same scalars it always did and the struct is assembled here.
template <typename Func>
struct ApiTraits;

// Argument-taking: ...(void* ctx, Cb, void* user_data, const Req* req)
template <typename Ret, typename Cb, typename Req>
struct ApiTraits<Ret (*)(void*, Cb, void*, const Req*)> {
    using callback_type = Cb;
    using request_type = Req;
    static constexpr bool has_request = true;
};

// No-argument: ...(void* ctx, Cb, void* user_data)
template <typename Ret, typename Cb>
struct ApiTraits<Ret (*)(void*, Cb, void*)> {
    using callback_type = Cb;
    static constexpr bool has_request = false;
};

template <typename Func>
using ApiCallbackT = typename ApiTraits<Func>::callback_type;

// Owns the request struct for the duration of the call. The generated header is
// explicit that its strings are "borrowed, NUL-terminated const char* valid only
// for the duration of the call they cross" -- so the struct must outlive the
// blocking wait in callApi*, not just the invoke() that starts it. Holding it in
// the bound object does that: the temporary lives to the end of the full
// expression, which encloses the wait.
template <typename Func, typename... BoundArgs>
class BoundApiCall
{
public:
    using traits = ApiTraits<Func>;
    using callback_type = typename traits::callback_type;

    BoundApiCall(Func func, void* ctx, BoundArgs&&... args)
        : m_func(func)
        , m_ctx(ctx)
    {
        if constexpr (traits::has_request) {
            m_req = typename traits::request_type{std::forward<BoundArgs>(args)...};
        }
    }

    int operator()(callback_type callback, void* userData)
    {
        if constexpr (traits::has_request) {
            return m_func(m_ctx, callback, userData, &m_req);
        } else {
            return m_func(m_ctx, callback, userData);
        }
    }

private:
    // std::conditional_t would name `traits::request_type` even in the false
    // branch, and the no-argument specialisation does not define it. Select the
    // member type with a partial specialisation instead, so the lookup only
    // happens when there IS a request struct.
    template <typename T, bool HasRequest>
    struct RequestMember {
        using type = std::monostate;
    };
    template <typename T>
    struct RequestMember<T, true> {
        using type = typename T::request_type;
    };

    Func m_func;
    void* m_ctx;
    [[no_unique_address]] typename RequestMember<traits, traits::has_request>::type m_req{};
};

template <typename Func, typename... BoundArgs>
auto bindApiCall(Func func, void* callbackCtx, BoundArgs&&... boundArgs)
{
    return BoundApiCall<Func, BoundArgs...>(
        func, callbackCtx, std::forward<BoundArgs>(boundArgs)...);
}

// One pending-call registry per result flavour. The callback must be a plain
// C function pointer, so it cannot capture -- it recovers its context through
// the user_data key instead.
template <typename Tag>
struct PendingRegistry {
    struct CallbackContext {
        std::binary_semaphore sem{0};
        CallbackPayload payload;
    };

    static inline std::mutex mutex;
    static inline std::unordered_map<void*, std::shared_ptr<CallbackContext>> contexts;

    static void put(void* key, std::shared_ptr<CallbackContext> ctx)
    {
        std::lock_guard<std::mutex> lock(mutex);
        contexts[key] = std::move(ctx);
    }

    // Take-and-erase, so a late second callback for the same key is a no-op
    // rather than a second sem.release().
    static std::shared_ptr<CallbackContext> take(void* key)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = contexts.find(key);
        if (it == contexts.end()) {
            return nullptr;
        }
        auto ctx = it->second;
        contexts.erase(it);
        return ctx;
    }

    static void drop(void* key)
    {
        std::lock_guard<std::mutex> lock(mutex);
        contexts.erase(key);
    }
};

// Builds the reply callback for whichever convention `Cb` follows.
//
// ScalarRawFn carries the payload as pointer+length and uses it for both success
// and failure text. The per-op ReplyFn splits them: `reply` is meaningful on
// success, `err_msg` on failure -- reading the wrong one yields an empty message
// and turns a real error into "<op> failed" with no reason, so select on
// err_code rather than on whichever pointer happens to be non-null.
template <typename Reg, typename Cb>
constexpr Cb makeReplyCallback()
{
    if constexpr (std::is_same_v<Cb, LogosDeliveryScalarRawFn>) {
        return +[](int callerRet, char* msg, size_t len, void* userData) {
            auto ctx = Reg::take(userData);
            if (!ctx) {
                return;
            }
            ctx->payload.callerRet = callerRet;
            if (msg && len > 0) {
                ctx->payload.message.assign(msg, len);
            }
            ctx->sem.release();
        };
    } else {
        return +[](int errCode, const char* reply, const char* errMsg, void* userData) {
            auto ctx = Reg::take(userData);
            if (!ctx) {
                return;
            }
            ctx->payload.callerRet = errCode;
            const char* text = (errCode == RET_OK) ? reply : errMsg;
            if (text) {
                ctx->payload.message.assign(text);
            }
            ctx->sem.release();
        };
    }
}

// Shared body for both flavours: register, fire, wait, translate. `wantValue`
// only decides whether the payload text is returned or discarded.
template <typename Tag, bool wantValue, typename BoundInvoke>
StdLogosResult callApi(const std::string& operationName, std::chrono::seconds timeout, BoundInvoke&& invoke)
{
    using Reg = PendingRegistry<Tag>;
    using Cb = typename std::decay_t<BoundInvoke>::callback_type;

    auto callbackCtx = std::make_shared<typename Reg::CallbackContext>();
    void* callbackKey = static_cast<void*>(callbackCtx.get());
    Reg::put(callbackKey, callbackCtx);

    int startResult = invoke(makeReplyCallback<Reg, Cb>(), callbackKey);
    if (startResult != RET_OK) {
        Reg::drop(callbackKey);
        return {false, {}, "failed to initiate " + operationName};
    }

    if (!callbackCtx->sem.try_acquire_for(timeout)) {
        Reg::drop(callbackKey);
        return {false, {}, operationName + " callback timeout"};
    }

    if (callbackCtx->payload.callerRet != RET_OK) {
        std::string message = callbackCtx->payload.message.empty()
            ? operationName + " failed"
            : callbackCtx->payload.message;
        return {false, {}, message};
    }

    if constexpr (wantValue) {
        return {true, callbackCtx->payload.message};
    } else {
        return {true, {}};
    }
}

struct RetVoidTag {};
struct RetValueTag {};

template <typename BoundInvoke>
StdLogosResult callApiRetVoid(const std::string& operationName, std::chrono::seconds timeout, BoundInvoke&& invoke)
{
    return callApi<RetVoidTag, false>(operationName, timeout, std::forward<BoundInvoke>(invoke));
}

template <typename BoundInvoke>
StdLogosResult callApiRetValue(const std::string& operationName, std::chrono::seconds timeout, BoundInvoke&& invoke)
{
    return callApi<RetValueTag, true>(operationName, timeout, std::forward<BoundInvoke>(invoke));
}
} // namespace
