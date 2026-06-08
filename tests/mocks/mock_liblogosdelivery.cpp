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
// Return values and callback messages are controlled via LogosCMockStore.
// For the int-returning dispatch functions, the return value is the *dispatch*
// code (0 / RET_OK by default); set a non-zero value to simulate a dispatch
// failure, in which case no completion callback is fired:
//   t.mockCFunction("logosdelivery_start_node").returns(1);  // dispatch fails

#include <logos_clib_mock.h>
#include <cstring>

#define RET_OK  0
#define RET_ERR 1

typedef void (*logosdelivery_callback)(int callerRet, const char* msg, size_t len, void* userData);

// Sentinel address used as a fake non-null delivery context.
static char s_fakeCtx = 0;

// Helper: invoke callback with RET_OK and the string configured in the mock store.
static void invokeOk(const char* funcName, logosdelivery_callback cb, void* userData) {
    if (!cb) return;
    const char* msg = LogosCMockStore::instance().getReturnString(funcName);
    cb(RET_OK, msg ? msg : "", msg ? strlen(msg) : 0, userData);
}

extern "C" {

void* logosdelivery_create_node(const char* /*cfg*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_create_node");
    int ok = LOGOS_CMOCK_RETURN(int, "logosdelivery_create_node");
    if (ok && cb) {
        cb(RET_OK, "", 0, userData);
    } else if (!ok && cb) {
        cb(RET_ERR, "mock: create_node fail", 22, userData);
    }
    return ok ? static_cast<void*>(&s_fakeCtx) : nullptr;
}

void logosdelivery_set_event_callback(void* /*ctx*/, logosdelivery_callback /*cb*/, void* /*userData*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_set_event_callback");
}

int logosdelivery_destroy(void* /*ctx*/, logosdelivery_callback /*cb*/, void* /*userData*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_destroy");
    return RET_OK;
}

int logosdelivery_start_node(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_start_node");
    // Return value is the dispatch code (default 0 = RET_OK). Only fire the
    // completion callback when dispatch "succeeds", mirroring the real FFI.
    int dispatch = LOGOS_CMOCK_RETURN(int, "logosdelivery_start_node");
    if (dispatch == RET_OK) {
        invokeOk("logosdelivery_start_node", cb, userData);
    }
    return dispatch;
}

int logosdelivery_stop_node(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_stop_node");
    int dispatch = LOGOS_CMOCK_RETURN(int, "logosdelivery_stop_node");
    if (dispatch == RET_OK) {
        invokeOk("logosdelivery_stop_node", cb, userData);
    }
    return dispatch;
}

int logosdelivery_send(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*msg*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_send");
    invokeOk("logosdelivery_send", cb, userData);
    return RET_OK;
}

int logosdelivery_subscribe(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*topic*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_subscribe");
    invokeOk("logosdelivery_subscribe", cb, userData);
    return RET_OK;
}

int logosdelivery_unsubscribe(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*topic*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_unsubscribe");
    invokeOk("logosdelivery_unsubscribe", cb, userData);
    return RET_OK;
}

int logosdelivery_get_node_info(void* /*ctx*/, logosdelivery_callback cb, void* userData, const char* /*attributeName*/) {
    LOGOS_CMOCK_RECORD("logosdelivery_get_node_info");
    invokeOk("logosdelivery_get_node_info", cb, userData);
    return RET_OK;
}

int logosdelivery_get_available_node_info_ids(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_get_available_node_info_ids");
    invokeOk("logosdelivery_get_available_node_info_ids", cb, userData);
    return RET_OK;
}

int logosdelivery_get_available_configs(void* /*ctx*/, logosdelivery_callback cb, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_get_available_configs");
    invokeOk("logosdelivery_get_available_configs", cb, userData);
    return RET_OK;
}

} // extern "C"
