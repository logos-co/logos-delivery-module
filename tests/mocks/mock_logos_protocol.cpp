// lp_* C ABI stubs for the unit tests: no logos host is running.
//
// The RLN bridge's client never comes up — enable still works and every op that
// would reach the module synthesizes a transport failure instead. The channel
// cipher relay's client does come up, against the programmable responder in
// mock_channel_state.h, so the relay can be driven end to end.

#include <cstdlib>
#include <cstring>
#include <string>

#include <logos_protocol.h>

#include "mock_channel_state.h"

namespace delivery_test_cipher {
uint64_t g_encryptFn = 0;
uint64_t g_decryptFn = 0;
uint64_t g_userData = 0;
LpHandler g_lpHandler;
std::string g_methodsJson;
bool g_clientCreateFails = false;
std::string g_lastTarget;
std::string g_lastOrigin;

void reset()
{
    g_encryptFn = 0;
    g_decryptFn = 0;
    g_userData = 0;
    g_lpHandler = nullptr;
    g_methodsJson.clear();
    g_clientCreateFails = false;
    g_lastTarget.clear();
    g_lastOrigin.clear();
}
} // namespace delivery_test_cipher

namespace {
// The RLN bridge asks for this target and must keep seeing a dead client, so
// its ops stay transport failures. Everything else gets a live stub.
constexpr const char* kRlnTarget = "liblogos_rln_module";

char* dup(const std::string& s)
{
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out) {
        std::memcpy(out, s.c_str(), s.size() + 1);
    }
    return out;
}

// A non-null handle the mock hands back; never dereferenced.
char g_handle = 0;
} // namespace

extern "C" {

lp_client* lp_client_create(const char* target, const char* origin, const char*, const char*)
{
    if (!target || std::string(target) == kRlnTarget) {
        return nullptr;
    }
    delivery_test_cipher::g_lastTarget = target;
    delivery_test_cipher::g_lastOrigin = origin ? origin : "";
    if (delivery_test_cipher::g_clientCreateFails) {
        return nullptr;
    }
    return reinterpret_cast<lp_client*>(&g_handle);
}

void lp_client_destroy(lp_client*) {}

int lp_invoke_async(lp_client* client, const char* method, const char* args, int,
                    lp_result_cb cb, void* userData)
{
    if (!client || !cb) {
        return LP_ERR_INVALID_ARG;
    }
    // Synchronous, as every mock here is: the waiter's semaphore is released
    // before it starts waiting.
    std::string out;
    bool ok = false;
    if (delivery_test_cipher::g_lpHandler) {
        ok = delivery_test_cipher::g_lpHandler(method ? method : "", args ? args : "", out);
    }
    if (!ok && out.empty()) {
        out = R"({"code":"mock","message":"no handler"})";
    }
    cb(ok ? 1 : 0, out.c_str(), userData);
    return LP_OK;
}

int lp_invoke(lp_client*, const char*, const char*, int, char**, char**)
{
    return LP_ERR_UNAVAILABLE;
}

char* lp_get_methods(lp_client* client)
{
    if (!client || delivery_test_cipher::g_methodsJson.empty()) {
        return nullptr;
    }
    return dup(delivery_test_cipher::g_methodsJson);
}

void lp_string_free(char* s)
{
    std::free(s);
}

lp_subscription* lp_subscribe(lp_client*, const char*, lp_event_cb, void*)
{
    return nullptr;
}

void lp_unsubscribe(lp_subscription*) {}

} // extern "C"
