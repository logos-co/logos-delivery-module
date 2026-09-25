// Mock implementation of liblogosdelivery's poll-mode C ABI.
// Replaces the real Nim library at link time during unit tests.
//
// The library's side of the poll model is a queue of messages and a file
// descriptor that is readable while the queue is not empty. This mock keeps
// that queue in memory: every method export answers at once by queueing its
// REPLY (so the module's pump finds it on the first poll), and tests queue
// events and reverse calls through mock_rln_state.h to play the library.
//
// Return values and reply texts are controlled via LogosCMockStore. For the
// int-returning exports the return value is the *dispatch* code (0 / NIMFFI_RET_OK by
// default); set a non-zero value to simulate a refused submit, in which case
// no reply is queued:
//   t.mockCFunction("logosdelivery_start_node").returns(1);  // refused
#include <logos_clib_mock.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include <liblogosdelivery_poll.h>
#include "mock_rln_state.h"

using nlohmann::json;

namespace {

struct Owned {
    NimFfiMsg msg{};
    std::vector<uint8_t> bytes;
};

std::deque<Owned> s_queue;
Owned s_current;              // the message handed out by the last poll
uint64_t s_nextId = 1;
uint64_t s_seq = 0;
int s_pipe[2] = {-1, -1};     // readable while the queue is not empty
char s_fakeCtx = 0;
bool s_interpose = false;
std::string s_interposeWire;
json s_interposeArgs;

void ensurePipe()
{
    if (s_pipe[0] >= 0) return;
    if (pipe(s_pipe) != 0) return;
    fcntl(s_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(s_pipe[1], F_SETFL, O_NONBLOCK);
}

void push(uint32_t kind, uint64_t id, uint64_t nameId, int ret, std::vector<uint8_t> bytes)
{
    ensurePipe();
    Owned o;
    o.bytes = std::move(bytes);
    o.msg.struct_size = sizeof(NimFfiMsg);
    o.msg.kind = kind;
    o.msg.seq = ++s_seq;
    o.msg.id = id;
    o.msg.name_id = nameId;
    o.msg.ret_code = ret;
    o.msg.payload = o.bytes.empty() ? reinterpret_cast<const uint8_t*>("") : o.bytes.data();
    o.msg.len = o.bytes.size();
    s_queue.push_back(std::move(o));
    char b = 1;
    (void)!write(s_pipe[1], &b, 1);
}

// A method export: records the call and queues an OK reply carrying the
// store's string for this name. `dispatchable` exports (start/stop) also
// honour a mocked int as the submit code, as the real FFI would refuse.
int method(const char* name, void* ctx, const uint8_t* req, size_t len, uint64_t* idOut,
           bool dispatchable = false)
{
    if (ctx != &s_fakeCtx || !idOut) return NIMFFI_RET_INVALID_CTX;
    delivery_test_rln::g_lastRequestMethod = name;
    delivery_test_rln::g_lastRequest =
        len ? json::from_cbor(std::vector<uint8_t>(req, req + len), true, false) : json::object();
    if (dispatchable && LogosCMockStore::instance().getReturn<int>(name) != 0) return NIMFFI_RET_ERR;
    if (s_interpose) {
        s_interpose = false;
        delivery_test_rln::pushReverseCall(s_interposeWire.c_str(), s_interposeArgs);
    }
    const uint64_t id = s_nextId++;
    const char* text = LogosCMockStore::instance().getReturnString(name);
    push(NIMFFI_MSG_REPLY, id, 0, NIMFFI_RET_OK, json::to_cbor(json(text ? text : "")));
    *idOut = id;
    return NIMFFI_RET_OK;
}

} // namespace

namespace delivery_test_rln {

nlohmann::json g_lastRequest;
std::string g_lastRequestMethod;
std::string g_lastCreateConfigJson;
bool g_lastCreateRlnPlugin = false;
uint64_t g_lastReplyCallId = 0;
int g_lastReplyRet = -1;
std::string g_lastReplyJson;
int g_replyCount = 0;

uint64_t pushReverseCall(const char* wire, const json& args)
{
    const uint64_t id = s_nextId++;
    push(NIMFFI_MSG_REVERSE_CALL, id, nimffi_name_id(wire), 0, json::to_cbor(args));
    return id;
}

void pushEvent(const char* wire, const json& payload)
{
    const std::string text = payload.dump();
    push(NIMFFI_MSG_EVENT, 0, nimffi_name_id(wire), 0,
         std::vector<uint8_t>(text.begin(), text.end()));
}

void interposeReverseCall(const char* wire, const json& args)
{
    s_interpose = true;
    s_interposeWire = wire;
    s_interposeArgs = args;
}

void resetRlnMockState()
{
    s_queue.clear();
    s_interpose = false;
    g_lastRequest = json();
    g_lastRequestMethod.clear();
    g_lastCreateConfigJson.clear();
    g_lastCreateRlnPlugin = false;
    g_lastReplyCallId = 0;
    g_lastReplyRet = -1;
    g_lastReplyJson.clear();
    g_replyCount = 0;
    if (s_pipe[0] >= 0) {
        char drain[64];
        while (read(s_pipe[0], drain, sizeof drain) > 0) {}
    }
}

} // namespace delivery_test_rln

extern "C" {

int logosdelivery_create_node(const uint8_t* req, size_t len, void** ctxOut, uint64_t* idOut)
{
    LOGOS_CMOCK_RECORD("logosdelivery_create_node");
    delivery_test_rln::g_lastRequestMethod = "logosdelivery_create_node";
    delivery_test_rln::g_lastRequest =
        len ? json::from_cbor(std::vector<uint8_t>(req, req + len), true, false) : json::object();
    delivery_test_rln::g_lastCreateConfigJson = delivery_test_rln::g_lastRequest.value("configJson", "");
    delivery_test_rln::g_lastCreateRlnPlugin = delivery_test_rln::g_lastRequest.value("rlnPlugin", false);
    const int ok = LOGOS_CMOCK_RETURN(int, "logosdelivery_create_node");
    if (!ok || !ctxOut || !idOut) {
        return NIMFFI_RET_ERR;
    }
    *ctxOut = &s_fakeCtx;
    *idOut = s_nextId++;
    push(NIMFFI_MSG_REPLY, *idOut, 0, NIMFFI_RET_OK, json::to_cbor(json(true)));
    return NIMFFI_RET_OK;
}

int logosdelivery_destroy(void* ctx)
{
    LOGOS_CMOCK_RECORD("logosdelivery_destroy");
    if (ctx != &s_fakeCtx) return NIMFFI_RET_INVALID_CTX;
    s_queue.clear();
    return NIMFFI_RET_OK;
}

int logosdelivery_shutdown(void)
{
    LOGOS_CMOCK_RECORD("logosdelivery_shutdown");
    return NIMFFI_RET_OK;
}

int logosdelivery_poll(void* ctx, int32_t /*timeoutMs*/, const NimFfiMsg** msg)
{
    if (ctx != &s_fakeCtx || !msg) return NIMFFI_RET_INVALID_CTX;
    if (s_queue.empty()) {
        *msg = nullptr;
        return NIMFFI_RET_TIMEOUT;
    }
    s_current = std::move(s_queue.front());
    s_queue.pop_front();
    s_current.msg.payload = s_current.bytes.empty() ? reinterpret_cast<const uint8_t*>("")
                                                    : s_current.bytes.data();
    char b;
    (void)!read(s_pipe[0], &b, 1);
    *msg = &s_current.msg;
    return NIMFFI_RET_OK;
}

int logosdelivery_poll_fd(void* ctx)
{
    if (ctx != &s_fakeCtx) return -1;
    ensurePipe();
    return s_pipe[0];
}

int logosdelivery_reverse_reply(void* ctx, uint64_t callId, int ret, const uint8_t* payload, size_t len)
{
    LOGOS_CMOCK_RECORD("logosdelivery_reverse_reply");
    if (ctx != &s_fakeCtx) return NIMFFI_RET_INVALID_CTX;
    delivery_test_rln::g_lastReplyCallId = callId;
    delivery_test_rln::g_lastReplyRet = ret;
    ++delivery_test_rln::g_replyCount;
    if (ret == NIMFFI_RET_OK) {
        json v = json::from_cbor(std::vector<uint8_t>(payload, payload + len), true, false);
        delivery_test_rln::g_lastReplyJson = v.is_string() ? v.get<std::string>() : v.dump();
    } else {
        delivery_test_rln::g_lastReplyJson.assign(reinterpret_cast<const char*>(payload), len);
    }
    return NIMFFI_RET_OK;
}

#define MOCK_METHOD(name, dispatchable)                                                \
    int name(void* ctx, const uint8_t* req, size_t len, uint64_t* idOut)                  \
    {                                                                                  \
        LOGOS_CMOCK_RECORD(#name);                                                     \
        return method(#name, ctx, req, len, idOut, dispatchable);                      \
    }
MOCK_METHOD(logosdelivery_start_node, true)
MOCK_METHOD(logosdelivery_stop_node, true)
MOCK_METHOD(logosdelivery_send, false)
MOCK_METHOD(logosdelivery_subscribe, false)
MOCK_METHOD(logosdelivery_unsubscribe, false)
MOCK_METHOD(logosdelivery_channel_create, false)
MOCK_METHOD(logosdelivery_channel_exists, false)
MOCK_METHOD(logosdelivery_channel_send, false)
MOCK_METHOD(logosdelivery_channel_close, false)
MOCK_METHOD(logosdelivery_get_available_node_info_ids, false)
MOCK_METHOD(logosdelivery_get_node_info, false)
MOCK_METHOD(logosdelivery_get_available_configs, false)
MOCK_METHOD(waku_store_query, false)
#undef MOCK_METHOD

} // extern "C"
