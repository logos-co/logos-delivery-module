// Unit tests for DeliveryModuleImpl.
// All liblogosdelivery C functions are mocked at link time via mock_liblogosdelivery.cpp.
// Mocks invoke callbacks synchronously so the semaphore inside api_call_handler.h
// is released before try_acquire_for starts waiting.

#include <logos_test.h>
#include "delivery_module_plugin.h"

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
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_set_event_callback"));
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
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_set_event_callback"));
}

// start

LOGOS_TEST(start_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.start().success);
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

// module name

LOGOS_TEST(name_returns_delivery_module) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_EQ(impl.name(), std::string("delivery_module"));
}
