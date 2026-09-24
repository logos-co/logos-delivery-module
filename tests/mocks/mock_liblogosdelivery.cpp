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
// Upstream generates the logosdelivery_ctx_* calls inline over a CBOR wire; the
// test stub header declares them instead, and this file implements them
// directly (extern "C", so no mangling), so no CBOR is involved.
//
// Return values and callback messages are controlled via LogosCMockStore.
// For the int-returning dispatch functions, the return value is the *dispatch*
// code (0 / RET_OK by default); set a non-zero value to simulate a dispatch
// failure, in which case no completion callback is fired:
//   t.mockCFunction("logosdelivery_ctx_start_node").returns(1);  // dispatch fails

#include <logos_clib_mock.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "mock_rln_state.h"

namespace delivery_test_rln {
LogosDeliveryRlnPlugin g_callbacks{};
void* g_userData = nullptr;
bool g_callbacksSet = false;
int g_setCallbacksCalls = 0;
uint64_t g_lastResponseReqId = 0;
std::string g_lastResponseJson;
bool g_responseFired = false;
std::string g_lastCreateConfigJson;
std::string g_startNodeReplyError;
} // namespace delivery_test_rln

#define RET_OK  0
#define RET_ERR 1

typedef struct {
    void* ptr;
} LogosDeliveryCtx;

typedef void (*logosdelivery_create)(int errCode, LogosDeliveryCtx* ctx, const char* errMsg, void* userData);
typedef void (*logosdelivery_reply)(int errCode, const char* const* reply, const char* errMsg, void* userData);
// Event listeners.
typedef void (*logosdelivery_event)(int callerRet, const char* msg, size_t len, void* userData);

// Sentinel address used as a fake non-null delivery context.
static char s_fakeCtx = 0;

// Helper: reply RET_OK with the string configured in the mock store.
static void replyOk(const char* funcName, logosdelivery_reply onReply, void* userData) {
    if (!onReply) return;
    const char* msg = LogosCMockStore::instance().getReturnString(funcName);
    const char* text = msg ? msg : "";
    onReply(RET_OK, &text, nullptr, userData);
}

// A call that takes only a context: record it, reply unless dispatch "fails".
static int dispatchCall(const char* funcName, logosdelivery_reply onReply, void* userData) {
    int dispatch = LogosCMockStore::instance().getReturn<int>(funcName);
    if (dispatch == RET_OK) {
        replyOk(funcName, onReply, userData);
    }
    return dispatch;
}

extern "C" {

int logosdelivery_ctx_create(const char* configJson, logosdelivery_create onCreated, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_create");
    delivery_test_rln::g_lastCreateConfigJson = configJson ? configJson : "";
    int ok = LOGOS_CMOCK_RETURN(int, "logosdelivery_ctx_create");
    if (onCreated) {
        if (ok) {
            auto* ctx = static_cast<LogosDeliveryCtx*>(calloc(1, sizeof(LogosDeliveryCtx)));
            ctx->ptr = &s_fakeCtx;
            onCreated(RET_OK, ctx, nullptr, userData);
        } else {
            onCreated(RET_ERR, nullptr, "mock: create_node fail", userData);
        }
    }
    return RET_OK;
}

const char* logosdelivery_version(void) {
    LOGOS_CMOCK_RECORD("logosdelivery_version");
    return "mock-version";
}

int logosdelivery_ctx_destroy(LogosDeliveryCtx* ctx) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_destroy");
    free(ctx);
    return RET_OK;
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

int logosdelivery_ctx_start_node(const LogosDeliveryCtx* /*ctx*/, logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_start_node");
    const std::string& failure = delivery_test_rln::g_startNodeReplyError;
    if (!failure.empty()) {
        if (onReply) {
            onReply(RET_ERR, nullptr, failure.c_str(), userData);
        }
        return RET_OK;
    }
    return dispatchCall("logosdelivery_ctx_start_node", onReply, userData);
}

int logosdelivery_ctx_stop_node(const LogosDeliveryCtx* /*ctx*/, logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_stop_node");
    return dispatchCall("logosdelivery_ctx_stop_node", onReply, userData);
}

int logosdelivery_ctx_send(const LogosDeliveryCtx* /*ctx*/, const char* /*messageJson*/,
                           logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_send");
    replyOk("logosdelivery_ctx_send", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_subscribe(const LogosDeliveryCtx* /*ctx*/, const char* /*contentTopic*/,
                                logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_subscribe");
    replyOk("logosdelivery_ctx_subscribe", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_unsubscribe(const LogosDeliveryCtx* /*ctx*/, const char* /*contentTopic*/,
                                  logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_unsubscribe");
    replyOk("logosdelivery_ctx_unsubscribe", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_channel_create(const LogosDeliveryCtx* /*ctx*/, const char* /*channelId*/,
                                     const char* /*contentTopic*/, const char* /*senderId*/,
                                     uint64_t /*encryptFn*/, uint64_t /*decryptFn*/,
                                     uint64_t /*cipherUserData*/, logosdelivery_reply onReply,
                                     void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_channel_create");
    replyOk("logosdelivery_ctx_channel_create", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_channel_exists(const LogosDeliveryCtx* /*ctx*/, const char* /*channelId*/,
                                     logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_channel_exists");
    replyOk("logosdelivery_ctx_channel_exists", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_channel_send(const LogosDeliveryCtx* /*ctx*/, const char* /*channelId*/,
                                   const char* /*messageJson*/, logosdelivery_reply onReply,
                                   void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_channel_send");
    replyOk("logosdelivery_ctx_channel_send", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_channel_close(const LogosDeliveryCtx* /*ctx*/, const char* /*channelId*/,
                                    logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_channel_close");
    replyOk("logosdelivery_ctx_channel_close", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_waku_store_query(const LogosDeliveryCtx* /*ctx*/, const char* /*jsonQuery*/,
                                       const char* /*peerAddr*/, int32_t /*timeoutMs*/,
                                       logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_waku_store_query");
    replyOk("logosdelivery_ctx_waku_store_query", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_get_node_info(const LogosDeliveryCtx* /*ctx*/, const char* /*nodeInfoId*/,
                                    logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_get_node_info");
    replyOk("logosdelivery_ctx_get_node_info", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_get_available_node_info_ids(const LogosDeliveryCtx* /*ctx*/,
                                                  logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_get_available_node_info_ids");
    replyOk("logosdelivery_ctx_get_available_node_info_ids", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_get_available_configs(const LogosDeliveryCtx* /*ctx*/,
                                            logosdelivery_reply onReply, void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_get_available_configs");
    replyOk("logosdelivery_ctx_get_available_configs", onReply, userData);
    return RET_OK;
}

// RLN surface (liblogosdelivery_rln.h). The install is recorded so tests can
// fire the stored callback slots, simulating the library requesting an RLN op.
int logosdelivery_rln_set_plugin(const LogosDeliveryRlnPlugin* cbs, void* user_data) {
    LOGOS_CMOCK_RECORD("logosdelivery_rln_set_plugin");
    delivery_test_rln::g_setCallbacksCalls++;
    if (cbs) {
        delivery_test_rln::g_callbacks = *cbs;
        delivery_test_rln::g_userData = user_data;
        delivery_test_rln::g_callbacksSet = true;
    } else {
        // NULL clears the surface (and, in the real library, fails all
        // in-flight requests).
        delivery_test_rln::g_callbacks = LogosDeliveryRlnPlugin{};
        delivery_test_rln::g_userData = nullptr;
        delivery_test_rln::g_callbacksSet = false;
    }
    return 0;
}

// Return value is controllable (default 0 = accepted); set non-zero to
// simulate an unknown / already-completed reqId:
//   t.mockCFunction("logosdelivery_rln_response").returns(1);
// --- service discovery -------------------------------------------------
//
// The module asks the node what discovery it needs, then installs a plugin
// when the answer says so. Both go through the ctx wrappers, so the mock
// implements those directly.

int logosdelivery_ctx_get_discovery_requirements(const void* /*ctx*/,
                                                 logosdelivery_reply onReply,
                                                 void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_get_discovery_requirements");
    // Unconfigured means a node that wants no plugin, so the many tests that
    // only need a context keep working without setting a reply.
    const char* configured = LogosCMockStore::instance().getReturnString(
        "logosdelivery_ctx_get_discovery_requirements");
    if (!configured || !*configured) {
        if (onReply) {
            const char* reply = R"({"externalServiceDiscovery":false,"bootstrapNodes":[]})";
            onReply(RET_OK, &reply, nullptr, userData);
        }
        return RET_OK;
    }
    replyOk("logosdelivery_ctx_get_discovery_requirements", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_set_service_discovery_plugin(const void* /*ctx*/,
                                                   uint64_t /*pluginPtr*/,
                                                   logosdelivery_reply onReply,
                                                   void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_set_service_discovery_plugin");
    replyOk("logosdelivery_ctx_set_service_discovery_plugin", onReply, userData);
    return RET_OK;
}

int logosdelivery_ctx_clear_service_discovery_plugin(const void* /*ctx*/,
                                                     logosdelivery_reply onReply,
                                                     void* userData) {
    LOGOS_CMOCK_RECORD("logosdelivery_ctx_clear_service_discovery_plugin");
    replyOk("logosdelivery_ctx_clear_service_discovery_plugin", onReply, userData);
    return RET_OK;
}

int logosdelivery_rln_response(uint64_t req_id, const char* result_json) {
    LOGOS_CMOCK_RECORD("logosdelivery_rln_response");
    delivery_test_rln::g_lastResponseReqId = req_id;
    delivery_test_rln::g_lastResponseJson = result_json ? result_json : "";
    delivery_test_rln::g_responseFired = true;
    return LOGOS_CMOCK_RETURN(int, "logosdelivery_rln_response");
}

} // extern "C"
