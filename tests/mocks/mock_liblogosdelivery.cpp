// Mock implementation of liblogosdelivery C functions.
// Replaces the real Nim library at link time during unit tests.
//
// Callback-taking functions invoke the callback synchronously so the result is
// observable before the wrapping call returns - matching the storage module
// mock pattern. For the blocking wrappers (send/subscribe/...) this releases the
// api_call_handler semaphore before try_acquire_for waits; for the fire-and-
// forget start()/stop() it means the nodeStarted/nodeStopped event is emitted
// synchronously during the dispatch call.
//
// Only the symbols are shared with the plugin (extern "C", so no mangling), not
// the declarations: request structs are taken as `const void*` since the mock
// ignores their contents.
//
// Return values and callback messages are controlled via LogosCMockStore.
// For the int-returning dispatch functions, the return value is the *dispatch*
// code (0 / RET_OK by default); set a non-zero value to simulate a dispatch
// failure, in which case no completion callback is fired:
//   t.mockCFunction("logosdelivery_start_node").returns(1);  // dispatch fails

#include <logos_clib_mock.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mock_rln_state.h"

namespace delivery_test_rln {
LogosDeliveryRlnCallbacks g_callbacks{};
void* g_userData = nullptr;
bool g_callbacksSet = false;
int g_setCallbacksCalls = 0;
uint64_t g_lastResponseReqId = 0;
std::string g_lastResponseJson;
bool g_responseFired = false;
} // namespace delivery_test_rln

#define RET_OK  0
#define RET_ERR 1

// Argument-taking exports: NUL-terminated reply and error strings.
typedef void (*logosdelivery_reply)(int errCode, const char* reply, const char* errMsg, void* userData);
// No-argument "scalar fast path" exports: raw byte run.
typedef void (*logosdelivery_scalar)(int callerRet, char* msg, size_t len, void* userData);
// Constructor: the context address as decimal text.
typedef void (*logosdelivery_create)(int errCode, const char* ctxAddr, const char* errMsg, void* userData);
// Event listeners.
typedef void (*logosdelivery_event)(int callerRet, const char* msg, size_t len, void* userData);

// Sentinel address used as a fake non-null delivery context.
static char s_fakeCtx = 0;

// Helper: reply RET_OK with the string configured in the mock store.
static void replyOk(const char* funcName, logosdelivery_reply onReply, void* userData) {
    if (!onReply) return;
    const char* msg = LogosCMockStore::instance().getReturnString(funcName);
    onReply(RET_OK, msg ? msg : "", "", userData);
}

static void scalarOk(const char* funcName, logosdelivery_scalar callback, void* userData) {
    if (!callback) return;
    const char* msg = LogosCMockStore::instance().getReturnString(funcName);
    // The real ABI hands out a mutable, non-NUL-terminated buffer; copy so the
    // callback cannot observe the store's storage.
    char buffer[4096];
    size_t len = msg ? strnlen(msg, sizeof(buffer)) : 0;
    if (len > 0) memcpy(buffer, msg, len);
    callback(RET_OK, buffer, len, userData);
}

extern "C" {

void* logosdelivery_create_node(const void* /*req*/, logosdelivery_create onCreated, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_create_node");
    int ok = LOGOS_CMOCK_RETURN(int, "logosdelivery_create_node");
    if (onCreated) {
        if (ok) {
            // The real constructor reports the context as decimal text.
            char addr[32];
            snprintf(addr, sizeof(addr), "%llu",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&s_fakeCtx)));
            onCreated(RET_OK, addr, "", userData);
        } else {
            onCreated(RET_ERR, "", "mock: create_node fail", userData);
        }
    }
    // Upstream reports the context through the callback, not the return value.
    return nullptr;
}

uint64_t logosdelivery_add_event_listener(void* /*ctx*/, const char* /*eventName*/,
                                          logosdelivery_event /*cb*/, void* /*userData*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_add_event_listener");
    // Non-zero: a valid listener id.
    return 1;
}

int logosdelivery_remove_event_listener(void* /*ctx*/, uint64_t /*listenerId*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_remove_event_listener");
    return RET_OK;
}

int logosdelivery_destroy(void* /*ctx*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_destroy");
    return RET_OK;
}

int logosdelivery_start_node(void* /*ctx*/, logosdelivery_scalar cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_start_node");
    // Return value is the dispatch code (default 0 = RET_OK). Only fire the
    // completion callback when dispatch "succeeds", mirroring the real FFI.
    int dispatch = LOGOS_CMOCK_RETURN(int, "logosdelivery_start_node");
    if (dispatch == RET_OK) {
        scalarOk("logosdelivery_start_node", cb, userData);
    }
    return dispatch;
}

int logosdelivery_stop_node(void* /*ctx*/, logosdelivery_scalar cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_stop_node");
    int dispatch = LOGOS_CMOCK_RETURN(int, "logosdelivery_stop_node");
    if (dispatch == RET_OK) {
        scalarOk("logosdelivery_stop_node", cb, userData);
    }
    return dispatch;
}

int logosdelivery_send(void* /*ctx*/, logosdelivery_reply onReply, void* userData, const void* /*req*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_send");
    replyOk("logosdelivery_send", onReply, userData);
    return RET_OK;
}

int logosdelivery_subscribe(void* /*ctx*/, logosdelivery_reply onReply, void* userData, const void* /*req*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_subscribe");
    replyOk("logosdelivery_subscribe", onReply, userData);
    return RET_OK;
}

int logosdelivery_unsubscribe(void* /*ctx*/, logosdelivery_reply onReply, void* userData, const void* /*req*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_unsubscribe");
    replyOk("logosdelivery_unsubscribe", onReply, userData);
    return RET_OK;
}

int logosdelivery_channel_create(void* /*ctx*/, logosdelivery_reply onReply, void* userData, const void* /*req*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_channel_create");
    replyOk("logosdelivery_channel_create", onReply, userData);
    return RET_OK;
}

int logosdelivery_channel_exists(void* /*ctx*/, logosdelivery_reply onReply, void* userData, const void* /*req*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_channel_exists");
    replyOk("logosdelivery_channel_exists", onReply, userData);
    return RET_OK;
}

int logosdelivery_channel_send(void* /*ctx*/, logosdelivery_reply onReply, void* userData, const void* /*req*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_channel_send");
    replyOk("logosdelivery_channel_send", onReply, userData);
    return RET_OK;
}

int logosdelivery_channel_close(void* /*ctx*/, logosdelivery_reply onReply, void* userData, const void* /*req*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_channel_close");
    replyOk("logosdelivery_channel_close", onReply, userData);
    return RET_OK;
}

int waku_store_query(void* /*ctx*/, logosdelivery_reply onReply, void* userData, const void* /*req*/) {
    LOGOS_CMOCK_RECORD("waku_store_query");
    replyOk("waku_store_query", onReply, userData);
    return RET_OK;
}

int logosdelivery_get_node_info(void* /*ctx*/, logosdelivery_reply onReply, void* userData, const void* /*req*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_get_node_info");
    replyOk("logosdelivery_get_node_info", onReply, userData);
    return RET_OK;
}

int logosdelivery_get_available_node_info_ids(void* /*ctx*/, logosdelivery_scalar cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_get_available_node_info_ids");
    scalarOk("logosdelivery_get_available_node_info_ids", cb, userData);
    return RET_OK;
}

int logosdelivery_get_available_configs(void* /*ctx*/, logosdelivery_scalar cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_get_available_configs");
    scalarOk("logosdelivery_get_available_configs", cb, userData);
    return RET_OK;
}

// RLN surface (liblogosdelivery_rln.h). Registration is recorded so tests can
// fire the stored callback slots, simulating the library requesting an RLN op.
int logosdelivery_rln_set_callbacks(const LogosDeliveryRlnCallbacks* cbs, void* user_data) {
    LOGOS_CMOCK_RECORD("logosdelivery_rln_set_callbacks");
    delivery_test_rln::g_setCallbacksCalls++;
    if (cbs) {
        delivery_test_rln::g_callbacks = *cbs;
        delivery_test_rln::g_userData = user_data;
        delivery_test_rln::g_callbacksSet = true;
    } else {
        // NULL clears the surface (and, in the real library, fails all
        // in-flight requests).
        delivery_test_rln::g_callbacks = LogosDeliveryRlnCallbacks{};
        delivery_test_rln::g_userData = nullptr;
        delivery_test_rln::g_callbacksSet = false;
    }
    return 0;
}

// Return value is controllable (default 0 = accepted); set non-zero to
// simulate an unknown / already-completed reqId:
//   t.mockCFunction("logosdelivery_rln_response").returns(1);
int logosdelivery_rln_response(uint64_t req_id, const char* result_json) {
    LOGOS_CMOCK_RECORD("logosdelivery_rln_response");
    delivery_test_rln::g_lastResponseReqId = req_id;
    delivery_test_rln::g_lastResponseJson = result_json ? result_json : "";
    delivery_test_rln::g_responseFired = true;
    return LOGOS_CMOCK_RETURN(int, "logosdelivery_rln_response");
}

} // extern "C"
