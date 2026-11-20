// Stub of the codegen-generated rln_module client, for unit tests.
//
// The real header is emitted at build time by logos-cpp-generator from
// metadata.json#optional_dependencies (the std/LIDL variant). The unit tests build the
// module sources without that codegen step, so this stand-in provides the
// subset of `RlnModule` that src/rln_bridge.cpp calls.
//
// The async twins answer SYNCHRONOUSLY, from the calling thread. The real
// client only does that for a refusal lp declined to send; a test wants it
// always, so an assertion can follow the call with no wait and no flake. What
// they answer is stubError(): a CallError, because a unit test has no host and
// every call would fail against one. Set it to drive the bridge's
// classification — an object_unavailable is a transport failure, an
// invalid_args is a provider refusal — and reset it between tests.
//
// Keep the signatures in sync with the generated header when bumping the
// rln_module flake input. Verify with, in logos-rln-modules:
//   nix build .#headers-lp   (logos-rln-module/, emits rln_module_api.h)

#pragma once
#ifndef __rln_module_api_stub__
#define __rln_module_api_stub__

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <logos_async_result.h>
#include <logos_call_error.h>
#include <logos_result.h>

namespace rln_module_stub {

// The error every stubbed call answers with. A unit test links no host, so the
// honest default is "the target is not there".
inline logos::CallError& stubError()
{
    static logos::CallError err =
        logos::callErrorObjectUnavailable("rln_module", "stub: no host");
    return err;
}

// When set, get_membership_state answers this text as a call that succeeded
// (no CallError), instead of stubError(): what a reachable module replied.
inline std::optional<std::string>& membershipStateReply()
{
    static std::optional<std::string> reply;
    return reply;
}

// The same per registry id, looked up before membershipStateReply(), for a
// test that reads two registries.
inline std::map<std::string, std::string>& membershipStateReplies()
{
    static std::map<std::string, std::string> replies;
    return replies;
}

// The registry id of every get_membership_state call, in order.
inline std::vector<std::string>& membershipStateCalls()
{
    static std::vector<std::string> calls;
    return calls;
}

inline void resetStubError()
{
    stubError() =
        logos::callErrorObjectUnavailable("rln_module", "stub: no host");
    membershipStateReply().reset();
    membershipStateReplies().clear();
    membershipStateCalls().clear();
}

} // namespace rln_module_stub

class RlnModule {
public:
    explicit RlnModule(const std::string&) {}

    StdLogosResult start(const std::string&, logos::CallError* err = nullptr,
                         int /*timeout_ms*/ = 0)
    {
        if (err) { *err = rln_module_stub::stubError(); }
        return {};
    }
    StdLogosResult stop(logos::CallError* err = nullptr, int /*timeout_ms*/ = 0)
    {
        if (err) { *err = rln_module_stub::stubError(); }
        return {};
    }

    void get_membership_stateAsyncResult(
        const std::string& registryId, const std::string&,
        std::function<void(logos::AsyncResult<std::string>)> callback,
        int /*timeout_ms*/ = 0)
    {
        rln_module_stub::membershipStateCalls().push_back(registryId);
        std::optional<std::string> reply = rln_module_stub::membershipStateReply();
        const auto& perRegistry = rln_module_stub::membershipStateReplies();
        if (const auto it = perRegistry.find(registryId); it != perRegistry.end()) {
            reply = it->second;
        }
        if (reply && callback) {
            logos::AsyncResult<std::string> res;
            res.value = *reply;
            res.error = logos::CallError{};
            callback(res);
            return;
        }
        answer(std::move(callback), std::string());
    }

    void get_registry_parametersAsyncResult(
        const std::string&, const std::string&,
        std::function<void(logos::AsyncResult<StdLogosResult>)> callback,
        int /*timeout_ms*/ = 0)
    {
        answer(std::move(callback), StdLogosResult{});
    }

    void get_epoch_quotaAsyncResult(
        const std::string&, const std::string&, const std::string&,
        std::function<void(logos::AsyncResult<StdLogosResult>)> callback,
        int /*timeout_ms*/ = 0)
    {
        answer(std::move(callback), StdLogosResult{});
    }

    void generate_proofAsyncResult(
        const std::string&, const std::string&, const std::string&, const std::string&,
        std::function<void(logos::AsyncResult<StdLogosResult>)> callback,
        int /*timeout_ms*/ = 0)
    {
        answer(std::move(callback), StdLogosResult{});
    }

    void validate_proofAsyncResult(
        const std::string&, const std::string&, const std::string&, const std::string&,
        const std::string&,
        std::function<void(logos::AsyncResult<StdLogosResult>)> callback,
        int /*timeout_ms*/ = 0)
    {
        answer(std::move(callback), StdLogosResult{});
    }

private:
    // Mirrors the generated body: the value is whatever the decode yields on a
    // failed call, and the error is what the caller branches on.
    template <typename T>
    static void answer(std::function<void(logos::AsyncResult<T>)> callback, T value)
    {
        if (!callback) { return; }
        logos::AsyncResult<T> res;
        res.value = std::move(value);
        res.error = rln_module_stub::stubError();
        callback(res);
    }
};

#endif /* __rln_module_api_stub__ */
