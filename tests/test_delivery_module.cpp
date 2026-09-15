// Unit tests for DeliveryModuleImpl.
// All liblogosdelivery C functions are mocked at link time via mock_liblogosdelivery.cpp.
// Mocks invoke callbacks synchronously so the semaphore inside api_call_handler.h
// is released before try_acquire_for starts waiting.

#include <logos_test.h>

#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "delivery_module_plugin.h"
#include "mocks/delivery_module_events_stub.h"
#include "mocks/mock_channel_state.h"
#include "mocks/mock_rln_state.h"

// ---------------------------------------------------------------------------
// Helper: create an impl that has a valid delivery context (createNode called).
// ---------------------------------------------------------------------------
static DeliveryModuleImpl* createInitializedImpl(LogosTestContext& t) {
    t.mockCFunction("logosdelivery_create_node").returns(1);
    auto* impl = new DeliveryModuleImpl();
    LOGOS_ASSERT_TRUE(impl->createNode(R"({"logLevel":"INFO"})").success);
    return impl;
}

// RLN lives behind its own method, never in the node config. Without a
// framework context the bridge cannot come up, which is not fatal — the plugin
// is still installed.
static constexpr const char* kRlnCfg =
    R"({"registry-id":"reg","rln-identifier":"rln-id","epoch-size-sec":600})";

static DeliveryModuleImpl* createRlnImpl(LogosTestContext& t) {
    t.mockCFunction("logosdelivery_create_node").returns(1);
    auto* impl = new DeliveryModuleImpl();
    LOGOS_ASSERT_TRUE(impl->configureRln(kRlnCfg).success);
    LOGOS_ASSERT_TRUE(impl->createNode(R"({"logLevel":"INFO"})").success);
    return impl;
}

// createNode

LOGOS_TEST(createNode_succeeds_when_ffi_returns_non_null_context) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_add_event_listener"));
}

LOGOS_TEST(createNode_fails_when_ffi_returns_null) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(0);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
}

LOGOS_TEST(createNode_tracks_call_count) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    impl.createNode(R"({"logLevel":"INFO"})");
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_create_node"), 1);
}

LOGOS_TEST(createNode_succeeds_with_logos_dev_preset_config) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"DEBUG","mode":"Core","preset":"logos.dev"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_add_event_listener"));
}

// start

LOGOS_TEST(start_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.start().success);
}

// Without a framework the context is never handed over, so enabling must
// refuse before touching modules() rather than serve blind.
// (The success path runs in tests/e2e/run.sh against the real daemon.)
LOGOS_TEST(rlnBridgeEnable_fails_without_module_context) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    DeliveryModuleImpl impl;

    StdLogosResult r = impl.rlnBridgeEnable();
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_EQ(r.error, std::string("module context not ready"));
}


LOGOS_TEST(start_succeeds_after_createNode) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_start_node"));

    delete impl;
}

LOGOS_TEST(start_calls_ffi_start_node) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    impl->start();
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_start_node"), 1);

    delete impl;
}

// stop

LOGOS_TEST(stop_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.stop().success);
}

LOGOS_TEST(stop_succeeds_after_createNode) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->stop().success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_stop_node"));

    delete impl;
}

// start()/stop() report completion via events

LOGOS_TEST(start_emits_node_started_event) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    auto* impl = createInitializedImpl(t);

    // Dispatch succeeds; the mock fires the completion callback synchronously,
    // so the nodeStarted event is observable right after start() returns.
    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.fired);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.success);

    delete impl;
}

LOGOS_TEST(stop_emits_node_stopped_event) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->stop().success);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStopped.fired);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStopped.success);

    delete impl;
}

LOGOS_TEST(start_returns_false_when_dispatch_fails) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    auto* impl = createInitializedImpl(t);

    // A non-zero dispatch code means the library refused to start; start()
    // reports failure immediately and NO completion event is emitted.
    t.mockCFunction("logosdelivery_start_node").returns(1);
    LOGOS_ASSERT_FALSE(impl->start().success);
    LOGOS_ASSERT_FALSE(delivery_test_events::g_lastNodeStarted.fired);

    delete impl;
}

LOGOS_TEST(stop_returns_false_when_dispatch_fails) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_stop_node").returns(1);
    LOGOS_ASSERT_FALSE(impl->stop().success);
    LOGOS_ASSERT_FALSE(delivery_test_events::g_lastNodeStopped.fired);

    delete impl;
}

// send

LOGOS_TEST(send_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;

    std::vector<uint8_t> payload{'h','e','l','l','o'};
    StdLogosResult result = impl.send("/test/1/delivery/proto", payload);
    LOGOS_ASSERT_FALSE(result.success);
}

LOGOS_TEST(send_succeeds_and_returns_request_id) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_send").returns("req-id-abc123");
    std::vector<uint8_t> payload{'h','e','l','l','o',' ','w','o','r','l','d'};
    StdLogosResult result = impl->send("/test/1/delivery/proto", payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("req-id-abc123"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_send"));

    delete impl;
}

LOGOS_TEST(send_calls_ffi_with_byte_array_payload) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_send").returns("req-id-xyz");
    std::vector<uint8_t> payload{'t','e','s','t','-','p','a','y','l','o','a','d'};
    StdLogosResult result = impl->send("/test/1/delivery/proto", payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_send"), 1);

    delete impl;
}

LOGOS_TEST(send_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    DeliveryModuleImpl implNoCtx;
    std::vector<uint8_t> payload{'p','a','y','l','o','a','d'};
    StdLogosResult failResult = implNoCtx.send("/topic", payload);
    LOGOS_ASSERT_FALSE(failResult.success);
    LOGOS_ASSERT_FALSE(failResult.error.empty());

    delete impl;
}

// subscribe

LOGOS_TEST(subscribe_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.subscribe("/test/1/delivery/proto").success);
}

LOGOS_TEST(subscribe_succeeds_with_context) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->subscribe("/test/1/delivery/proto").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_subscribe"));

    delete impl;
}

// unsubscribe

LOGOS_TEST(unsubscribe_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.unsubscribe("/test/1/delivery/proto").success);
}

LOGOS_TEST(unsubscribe_succeeds_with_context) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->unsubscribe("/test/1/delivery/proto").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_unsubscribe"));

    delete impl;
}

// storeQuery

LOGOS_TEST(storeQuery_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    StdLogosResult result = impl.storeQuery(
        R"({"requestId":"req-1","includeData":true,"paginationForward":true})",
        "/ip4/127.0.0.1/tcp/60000/p2p/16Uiu2peer", 5000);
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_FALSE(result.error.empty());
}

LOGOS_TEST(storeQuery_returns_response_json) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    const char* responseJson =
        R"({"requestId":"req-1","statusCode":200,"statusDesc":"OK","messages":[]})";
    t.mockCFunction("waku_store_query").returns(responseJson);

    StdLogosResult result = impl->storeQuery(
        R"({"requestId":"req-1","includeData":true,"paginationForward":true})",
        "/ip4/127.0.0.1/tcp/60000/p2p/16Uiu2peer", 5000);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string(responseJson));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("waku_store_query"), 1);

    delete impl;
}

// channelCreate

LOGOS_TEST(channelCreate_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.channelCreate("chan-1", "/test/1/delivery/proto", "sender-1", "").success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_channel_create"));
}

LOGOS_TEST(channelCreate_returns_channel_id) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_channel_create").returns("chan-1");
    StdLogosResult result = impl->channelCreate("chan-1", "/test/1/delivery/proto", "sender-1", "");

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("chan-1"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_channel_create"), 1);

    delete impl;
}

// channelCreate — per-channel cipher relay
//
// The relay's outbound calls go through the programmable lp stub in
// mocks/mock_logos_protocol.cpp, so a test stands in for the module that owns
// the channel. `logos::currentCaller()` is Unknown without a framework, which
// is why every spec here names its `module` explicitly.

namespace {

// Base64 over the alphabet the module uses, so a test can speak the cipher
// wire without pulling boost into this TU.
const char* kB64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64Encode(const std::vector<uint8_t>& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        const size_t left = in.size() - i;
        uint32_t chunk = static_cast<uint32_t>(in[i]) << 16;
        if (left > 1) chunk |= static_cast<uint32_t>(in[i + 1]) << 8;
        if (left > 2) chunk |= in[i + 2];
        out += kB64Alphabet[(chunk >> 18) & 0x3f];
        out += kB64Alphabet[(chunk >> 12) & 0x3f];
        out += left > 1 ? kB64Alphabet[(chunk >> 6) & 0x3f] : '=';
        out += left > 2 ? kB64Alphabet[chunk & 0x3f] : '=';
    }
    return out;
}

std::vector<uint8_t> b64Decode(const std::string& in) {
    std::vector<uint8_t> out;
    uint32_t chunk = 0;
    int bits = 0;
    for (char c : in) {
        const char* at = c == '=' ? nullptr : strchr(kB64Alphabet, c);
        if (!at) break;
        chunk = (chunk << 6) | static_cast<uint32_t>(at - kB64Alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((chunk >> bits) & 0xff));
        }
    }
    return out;
}

// The cipher a test module implements: flip every byte. Its own inverse, so
// one body serves encrypt and decrypt.
std::vector<uint8_t> flip(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out = in;
    for (auto& b : out) b = static_cast<uint8_t>(~b);
    return out;
}

std::string hex(const std::vector<uint8_t>& in) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : in) { out += digits[b >> 4]; out += digits[b & 0x0f]; }
    return out;
}

using CryptoFn = int (*)(void*, const uint8_t*, size_t, const uint8_t**, size_t*);

CryptoFn asCryptoFn(uint64_t handle) {
    return reinterpret_cast<CryptoFn>(static_cast<uintptr_t>(handle));
}

const char* kCipherSpec = R"({"module":"demo_module","encrypt":"chEnc","decrypt":"chDec"})";

// A methods document that satisfies the arity check for both methods.
const char* kMethods =
    R"([{"name":"chEnc","parameters":[{"name":"channelId"},{"name":"payload"}]},)"
    R"({"name":"chDec","parameters":[{"name":"channelId"},{"name":"payload"}]}])";

// Installs a responder that applies `flip` to whatever payload it is handed and
// records the calls it saw.
struct CipherModule {
    std::vector<std::string> calls;
    std::string lastChannelId;

    void install() {
        delivery_test_cipher::g_lpHandler =
            [this](const std::string& method, const std::string& argsJson, std::string& out) {
                calls.push_back(method);
                const auto args = nlohmann::json::parse(argsJson, nullptr, false);
                if (!args.is_array() || args.size() != 2) return false;
                lastChannelId = args[0].get<std::string>();
                out = nlohmann::json(b64Encode(flip(b64Decode(args[1].get<std::string>())))).dump();
                return true;
            };
    }
};

} // namespace

LOGOS_TEST(channelCreate_leaves_the_cipher_uninstalled_without_a_spec) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_cipher::reset();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->channelCreate("chan-1", "/test/1/delivery/proto", "s", "").success);
    LOGOS_ASSERT_EQ(delivery_test_cipher::g_encryptFn, static_cast<uint64_t>(0));
    LOGOS_ASSERT_EQ(delivery_test_cipher::g_decryptFn, static_cast<uint64_t>(0));
    LOGOS_ASSERT_EQ(delivery_test_cipher::g_userData, static_cast<uint64_t>(0));

    delete impl;
}

LOGOS_TEST(channelCreate_installs_the_cipher_and_relays_both_directions) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_cipher::reset();
    delivery_test_cipher::g_methodsJson = kMethods;
    CipherModule cipher;
    cipher.install();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(
        impl->channelCreate("chan-1", "/test/1/delivery/proto", "s", kCipherSpec).success);
    LOGOS_ASSERT_EQ(delivery_test_cipher::g_lastTarget, std::string("demo_module"));
    LOGOS_ASSERT_EQ(delivery_test_cipher::g_lastOrigin, std::string("delivery_module"));
    LOGOS_ASSERT_TRUE(delivery_test_cipher::g_encryptFn != 0);
    LOGOS_ASSERT_TRUE(delivery_test_cipher::g_decryptFn != 0);
    LOGOS_ASSERT_TRUE(delivery_test_cipher::g_userData != 0);

    const std::vector<uint8_t> plain = {0x00, 0x01, 0x7f, 0xff};
    void* ud = reinterpret_cast<void*>(static_cast<uintptr_t>(delivery_test_cipher::g_userData));

    const uint8_t* out = nullptr;
    size_t outLen = 0;
    LOGOS_ASSERT_EQ(
        asCryptoFn(delivery_test_cipher::g_encryptFn)(ud, plain.data(), plain.size(), &out, &outLen),
        0);
    const std::vector<uint8_t> sealed(out, out + outLen);
    LOGOS_ASSERT_EQ(hex(sealed), hex(flip(plain)));
    LOGOS_ASSERT_EQ(cipher.lastChannelId, std::string("chan-1"));

    out = nullptr;
    outLen = 0;
    LOGOS_ASSERT_EQ(
        asCryptoFn(delivery_test_cipher::g_decryptFn)(ud, sealed.data(), sealed.size(), &out, &outLen),
        0);
    LOGOS_ASSERT_EQ(hex(std::vector<uint8_t>(out, out + outLen)), hex(plain));

    LOGOS_ASSERT_EQ(cipher.calls.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(cipher.calls[0], std::string("chEnc"));
    LOGOS_ASSERT_EQ(cipher.calls[1], std::string("chDec"));

    delete impl;
}

LOGOS_TEST(cipher_failure_fails_the_message_rather_than_falling_back_to_plaintext) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_cipher::reset();
    delivery_test_cipher::g_methodsJson = kMethods;
    delivery_test_cipher::g_lpHandler = [](const std::string&, const std::string&, std::string&) {
        return false;
    };
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(
        impl->channelCreate("chan-1", "/test/1/delivery/proto", "s", kCipherSpec).success);

    const std::vector<uint8_t> plain = {1, 2, 3};
    void* ud = reinterpret_cast<void*>(static_cast<uintptr_t>(delivery_test_cipher::g_userData));
    const uint8_t* out = nullptr;
    size_t outLen = 0;
    LOGOS_ASSERT_TRUE(
        asCryptoFn(delivery_test_cipher::g_encryptFn)(ud, plain.data(), plain.size(), &out, &outLen)
        != 0);

    delete impl;
}

LOGOS_TEST(channelCreate_rejects_a_cipher_target_that_lacks_the_methods) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_cipher::reset();
    delivery_test_cipher::g_methodsJson = R"([{"name":"chEnc","parameters":[{"name":"only"}]}])";
    auto* impl = createInitializedImpl(t);

    StdLogosResult r = impl->channelCreate("chan-1", "/test/1/delivery/proto", "s", kCipherSpec);
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_channel_create"));

    delete impl;
}

LOGOS_TEST(channelCreate_rejects_a_malformed_cipher_spec) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_cipher::reset();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_FALSE(
        impl->channelCreate("chan-1", "/test/1/delivery/proto", "s", "not json").success);
    LOGOS_ASSERT_FALSE(
        impl->channelCreate("chan-1", "/test/1/delivery/proto", "s", R"({"encrypt":"e"})").success);

    delete impl;
}

LOGOS_TEST(channelCreate_needs_a_named_module_when_the_caller_is_unknown) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_cipher::reset();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_FALSE(
        impl->channelCreate("chan-1", "/test/1/delivery/proto", "s",
                            R"({"encrypt":"chEnc","decrypt":"chDec"})").success);

    delete impl;
}

LOGOS_TEST(channelCreate_fails_when_the_cipher_target_cannot_be_reached) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_cipher::reset();
    delivery_test_cipher::g_clientCreateFails = true;
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_FALSE(
        impl->channelCreate("chan-1", "/test/1/delivery/proto", "s", kCipherSpec).success);

    delete impl;
}

// channelExists

LOGOS_TEST(channelExists_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.channelExists("chan-1").success);
}

LOGOS_TEST(channelExists_passes_through_true_and_false) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    // The FFI returns "true"/"false" verbatim; an unknown id is not an error.
    t.mockCFunction("logosdelivery_channel_exists").returns("true");
    StdLogosResult existing = impl->channelExists("chan-1");
    LOGOS_ASSERT_TRUE(existing.success);
    LOGOS_ASSERT_EQ(existing.value.get<std::string>(), std::string("true"));

    t.mockCFunction("logosdelivery_channel_exists").returns("false");
    StdLogosResult missing = impl->channelExists("no-such-chan");
    LOGOS_ASSERT_TRUE(missing.success);
    LOGOS_ASSERT_EQ(missing.value.get<std::string>(), std::string("false"));

    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_channel_exists"), 2);

    delete impl;
}

// channelSend

LOGOS_TEST(channelSend_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;

    std::vector<uint8_t> payload{'h','e','l','l','o'};
    LOGOS_ASSERT_FALSE(impl.channelSend("chan-1", payload).success);
}

LOGOS_TEST(channelSend_succeeds_and_returns_request_id) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_channel_send").returns("req-id-chan-42");
    std::vector<uint8_t> payload{'h','e','l','l','o',' ','c','h','a','n'};
    StdLogosResult result = impl->channelSend("chan-1", payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("req-id-chan-42"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_channel_send"));

    delete impl;
}

// channelClose

LOGOS_TEST(channelClose_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.channelClose("chan-1").success);
}

LOGOS_TEST(channelClose_succeeds_with_context) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->channelClose("chan-1").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_channel_close"));

    delete impl;
}

// getAvailableNodeInfoIDs

LOGOS_TEST(getAvailableNodeInfoIDs_returns_mocked_string) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_get_available_node_info_ids").returns(R"(["Version","MyPeerId"])");
    StdLogosResult result = impl->getAvailableNodeInfoIDs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_available_node_info_ids"));
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string(R"(["Version","MyPeerId"])"));

    delete impl;
}

LOGOS_TEST(getAvailableNodeInfoIDs_returns_empty_on_ffi_failure) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    StdLogosResult result = impl.getAvailableNodeInfoIDs();
    LOGOS_ASSERT_FALSE(result.success);
}

// getNodeInfo

LOGOS_TEST(getNodeInfo_returns_mocked_value_for_attribute) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_get_node_info").returns("v1.2.3");
    StdLogosResult result = impl->getNodeInfo("Version");

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("v1.2.3"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_node_info"));

    delete impl;
}

// getAvailableConfigs

LOGOS_TEST(getAvailableConfigs_returns_mocked_json) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_get_available_configs").returns(R"([{"key":"mode","type":"string"}])");
    StdLogosResult result = impl->getAvailableConfigs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_available_configs"));

    delete impl;
}

LOGOS_TEST(getAvailableConfigs_returns_empty_on_ffi_failure) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    StdLogosResult result = impl.getAvailableConfigs();
    LOGOS_ASSERT_FALSE(result.success);
}

// collectOpenMetricsText

LOGOS_TEST(collectOpenMetricsText_returns_empty_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;

    LOGOS_ASSERT_EQ(impl.collectOpenMetricsText(), std::string(""));
    // No context -> we must not even attempt the FFI read.
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_get_node_info"));
}

LOGOS_TEST(collectOpenMetricsText_returns_metrics_text_verbatim) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    const char* promText =
        "# HELP waku_node_messages_total number of messages\n"
        "# TYPE waku_node_messages_total counter\n"
        "waku_node_messages_total{shard=\"0\"} 42\n";
    t.mockCFunction("logosdelivery_get_node_info").returns(promText);

    // The module is a pure passthrough: the openmetrics scraper does the parsing.
    LOGOS_ASSERT_EQ(impl->collectOpenMetricsText(), std::string(promText));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_node_info"));

    delete impl;
}

// RLN bridge (liblogosdelivery_rln.h)

LOGOS_TEST(createNode_installs_rln_plugin) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createRlnImpl(t);

    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_rln_set_plugin"));
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_callbacksSet);
    // userData must be the module instance so the trampolines can emit events.
    LOGOS_ASSERT(delivery_test_rln::g_userData == static_cast<void*>(impl));
    // All four slots populated.
    LOGOS_ASSERT(delivery_test_rln::g_callbacks.get_membership_state != nullptr);
    LOGOS_ASSERT(delivery_test_rln::g_callbacks.get_epoch_quota != nullptr);
    LOGOS_ASSERT(delivery_test_rln::g_callbacks.generate_proof != nullptr);
    LOGOS_ASSERT(delivery_test_rln::g_callbacks.validate_proof != nullptr);

    delete impl;
}

LOGOS_TEST(rln_generate_proof_callback_emits_typed_event_with_verbatim_args) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnRequestEvent();
    auto* impl = createRlnImpl(t);

    delivery_test_rln::g_callbacks.generate_proof(7, "ab01", 1700000000,
                                                  delivery_test_rln::g_userData);

    const auto& e = delivery_test_events::g_lastRlnRequest;
    LOGOS_ASSERT_EQ(e.op, std::string("generate_proof"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(7));
    // Supplied by this module, not by the library.
    LOGOS_ASSERT_EQ(e.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(e.rlnIdentifier, std::string("rln-id"));
    LOGOS_ASSERT_EQ(e.signalHex, std::string("ab01"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000000));

    delete impl;
}

LOGOS_TEST(rln_callback_slots_route_to_their_events) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createRlnImpl(t);
    void* ud = delivery_test_rln::g_userData;

    // The library's plugin carries no registry or membership, so each slot is
    // fired with only its own arguments and the module's own configuration is
    // asserted on the emitted event. The opaque proof JSON must pass through
    // untouched.
    const auto& e = delivery_test_events::g_lastRlnRequest;

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.get_membership_state(4, ud);
    LOGOS_ASSERT_EQ(e.op, std::string("get_membership_state"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(4));
    LOGOS_ASSERT_EQ(e.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(e.rlnIdentifier, std::string("rln-id"));

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.get_epoch_quota(5, 1700000001, ud);
    LOGOS_ASSERT_EQ(e.op, std::string("get_epoch_quota"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(5));
    LOGOS_ASSERT_EQ(e.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(e.rlnIdentifier, std::string("rln-id"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000001));

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.generate_proof(6, "ab01", 1700000002, ud);
    LOGOS_ASSERT_EQ(e.op, std::string("generate_proof"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(6));
    LOGOS_ASSERT_EQ(e.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(e.signalHex, std::string("ab01"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000002));

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.validate_proof(8, "ab01", 1700000003,
                                                  R"({"proof":"00ff"})", ud);
    LOGOS_ASSERT_EQ(e.op, std::string("validate_proof"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(8));
    LOGOS_ASSERT_EQ(e.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(e.signalHex, std::string("ab01"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000003));
    LOGOS_ASSERT_EQ(e.proofJson, std::string(R"({"proof":"00ff"})"));

    delete impl;
}

LOGOS_TEST(createNode_without_configureRln_installs_no_plugin) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_rln_set_plugin"));
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_callbacksSet);

    delete impl;
}

LOGOS_TEST(configureRln_rejects_an_incomplete_config) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.configureRln("not json").success);
    LOGOS_ASSERT_FALSE(impl.configureRln(R"({"rln-identifier":"rln-id"})").success);
    LOGOS_ASSERT_FALSE(impl.configureRln(R"({"registry-id":"reg"})").success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_callbacksSet);
}

LOGOS_TEST(configureRln_must_precede_createNode) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_FALSE(impl->configureRln(kRlnCfg).success);

    delete impl;
}

LOGOS_TEST(rlnRespond_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    DeliveryModuleImpl impl;

    LOGOS_ASSERT_FALSE(impl.rlnRespond(1, R"({"success":true,"value":{}})").success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_responseFired);
}

LOGOS_TEST(rlnRespond_forwards_req_id_and_verbatim_json) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    const char* resultJson = R"({"success":true,"value":{"verdict":"valid"}})";
    LOGOS_ASSERT_TRUE(impl->rlnRespond(42, resultJson).success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_rln_response"));
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastResponseReqId, static_cast<uint64_t>(42));
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastResponseJson, std::string(resultJson));

    delete impl;
}

LOGOS_TEST(rlnRespond_fails_on_unknown_req_id) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    // Non-zero response code = reqId unknown (e.g. already timed out
    // library-side).
    t.mockCFunction("logosdelivery_rln_response").returns(1);
    StdLogosResult result = impl->rlnRespond(99, R"({"success":true,"value":{}})");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_FALSE(result.error.empty());

    delete impl;
}

LOGOS_TEST(rlnRespond_passes_negative_req_id_bit_exactly) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    // A negative reqId is the int64 view of a library id >= 2^63; it must
    // round-trip bit-exactly, not be rejected.
    LOGOS_ASSERT_TRUE(impl->rlnRespond(-1, R"({"success":true,"value":{}})").success);
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_responseFired);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastResponseReqId, UINT64_MAX);

    delete impl;
}

LOGOS_TEST(destructor_clears_rln_callbacks) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createRlnImpl(t);
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_callbacksSet);

    delete impl;

    // Destruction must clear the surface (NULL registration) so no in-flight
    // request can fire into a destroyed object.
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_callbacksSet);
    LOGOS_ASSERT(delivery_test_rln::g_callbacks.generate_proof == nullptr);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_setCallbacksCalls, 2);
}

// module name

LOGOS_TEST(name_returns_delivery_module) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_EQ(impl.name(), std::string("delivery_module"));
}
