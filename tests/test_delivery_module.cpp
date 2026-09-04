// Unit tests for DeliveryModuleImpl.
// All liblogosdelivery C functions are mocked at link time via mock_liblogosdelivery.cpp.
// Mocks invoke callbacks synchronously so the semaphore inside api_call_handler.h
// is released before try_acquire_for starts waiting.

#include <logos_test.h>
#include "delivery_module_plugin.h"
#include "mocks/delivery_module_events_stub.h"
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
    LOGOS_ASSERT_FALSE(impl.channelCreate("chan-1", "/test/1/delivery/proto", "sender-1").success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_channel_create"));
}

LOGOS_TEST(channelCreate_returns_channel_id) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_channel_create").returns("chan-1");
    StdLogosResult result = impl->channelCreate("chan-1", "/test/1/delivery/proto", "sender-1");

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("chan-1"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_channel_create"), 1);

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

    t.mockCFunction("logosdelivery_get_available_node_info_ids").returns("@[Version,PeerID]");
    StdLogosResult result = impl->getAvailableNodeInfoIDs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_available_node_info_ids"));
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("@[Version,PeerID]"));

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

LOGOS_TEST(createNode_registers_rln_callbacks) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_rln_set_callbacks"));
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_callbacksSet);
    // userData must be the module instance so the trampolines can emit events.
    LOGOS_ASSERT(delivery_test_rln::g_userData == static_cast<void*>(impl));
    // All seven slots populated.
    LOGOS_ASSERT(delivery_test_rln::g_callbacks.start != nullptr);
    LOGOS_ASSERT(delivery_test_rln::g_callbacks.stop != nullptr);
    LOGOS_ASSERT(delivery_test_rln::g_callbacks.register_membership != nullptr);
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
    auto* impl = createInitializedImpl(t);

    delivery_test_rln::g_callbacks.generate_proof(7, "eip155:59144:0xb9cd", "0xdead", "ab01",
                                                  1700000000, delivery_test_rln::g_userData);

    const auto& e = delivery_test_events::g_lastRlnRequest;
    LOGOS_ASSERT_EQ(e.op, std::string("generate_proof"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(7));
    LOGOS_ASSERT_EQ(e.registryId, std::string("eip155:59144:0xb9cd"));
    LOGOS_ASSERT_EQ(e.rlnIdentifier, std::string("0xdead"));
    LOGOS_ASSERT_EQ(e.signalHex, std::string("ab01"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000000));

    delete impl;
}

LOGOS_TEST(rln_callback_slots_route_to_their_events) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);
    void* ud = delivery_test_rln::g_userData;

    // Slot/arg mix-ups within a signature are invisible to the compiler for
    // same-typed string params, so each slot is fired once and its routing
    // asserted. The opaque JSON args (options/proof) must pass through
    // untouched.
    const auto& e = delivery_test_events::g_lastRlnRequest;

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.start(1, R"({"epoch_size_sec":600})", ud);
    LOGOS_ASSERT_EQ(e.op, std::string("start"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(1));
    LOGOS_ASSERT_EQ(e.configJson, std::string(R"({"epoch_size_sec":600})"));

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.stop(2, ud);
    LOGOS_ASSERT_EQ(e.op, std::string("stop"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(2));

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.register_membership(
        3, "reg", "rln-id", R"([{"key":"rate_limit","value":"10"}])", ud);
    LOGOS_ASSERT_EQ(e.op, std::string("register_membership"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(3));
    LOGOS_ASSERT_EQ(e.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(e.rlnIdentifier, std::string("rln-id"));
    LOGOS_ASSERT_EQ(e.optionsJson, std::string(R"([{"key":"rate_limit","value":"10"}])"));

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.get_membership_state(4, "reg", "rln-id", ud);
    LOGOS_ASSERT_EQ(e.op, std::string("get_membership_state"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(4));
    LOGOS_ASSERT_EQ(e.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(e.rlnIdentifier, std::string("rln-id"));

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.get_epoch_quota(5, "reg", "rln-id", 1700000001, ud);
    LOGOS_ASSERT_EQ(e.op, std::string("get_epoch_quota"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(5));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000001));

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.generate_proof(6, "reg", "rln-id", "ab01", 1700000002, ud);
    LOGOS_ASSERT_EQ(e.op, std::string("generate_proof"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(6));
    LOGOS_ASSERT_EQ(e.signalHex, std::string("ab01"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000002));

    delivery_test_events::resetRlnRequestEvent();
    delivery_test_rln::g_callbacks.validate_proof(8, "reg", "rln-id", "ab01", 1700000003,
                                                  R"({"proof":"00ff"})", ud);
    LOGOS_ASSERT_EQ(e.op, std::string("validate_proof"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(8));
    LOGOS_ASSERT_EQ(e.signalHex, std::string("ab01"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000003));
    LOGOS_ASSERT_EQ(e.proofJson, std::string(R"({"proof":"00ff"})"));

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
    auto* impl = createInitializedImpl(t);
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
