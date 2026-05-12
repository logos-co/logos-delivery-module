// Unit tests for DeliveryModulePlugin.
// All liblogosdelivery C functions are mocked at link time via mock_liblogosdelivery.cpp.
// Mocks invoke callbacks synchronously so the semaphore inside api_call_handler.h
// is released before try_acquire_for starts waiting.

#include <logos_test.h>
#include "delivery_module_plugin.h"

// ---------------------------------------------------------------------------
// Helper: create a plugin that has a valid delivery context (createNode called).
// ---------------------------------------------------------------------------
static DeliveryModulePlugin* createInitializedPlugin(LogosTestContext& t) {
    t.mockCFunction("logosdelivery_create_node").returns(1);
    auto* plugin = new DeliveryModulePlugin();
    LOGOS_ASSERT_TRUE(plugin->createNode(R"({"logLevel":"INFO"})").success);
    return plugin;
}

// createNode

LOGOS_TEST(createNode_succeeds_when_ffi_returns_non_null_context) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_TRUE(plugin.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_set_event_callback"));
}

LOGOS_TEST(createNode_fails_when_ffi_returns_null) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(0);

    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_FALSE(plugin.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
}

LOGOS_TEST(createNode_tracks_call_count) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModulePlugin plugin;
    plugin.createNode(R"({"logLevel":"INFO"})");
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_create_node"), 1);
}

LOGOS_TEST(createNode_succeeds_with_logos_dev_preset_config) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_TRUE(plugin.createNode(R"({"logLevel":"DEBUG","mode":"Core","preset":"logos.dev"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_set_event_callback"));
}

// start

LOGOS_TEST(start_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_FALSE(plugin.start().success);
}

LOGOS_TEST(start_succeeds_after_createNode) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    LOGOS_ASSERT_TRUE(plugin->start().success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_start_node"));

    delete plugin;
}

LOGOS_TEST(start_calls_ffi_start_node) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    plugin->start();
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_start_node"), 1);

    delete plugin;
}

// stop

LOGOS_TEST(stop_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_FALSE(plugin.stop().success);
}

LOGOS_TEST(stop_succeeds_after_createNode) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    LOGOS_ASSERT_TRUE(plugin->stop().success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_stop_node"));

    delete plugin;
}

// send

LOGOS_TEST(send_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModulePlugin plugin;

    LogosResult result = plugin.send("/test/1/delivery/proto", QByteArray("hello"));
    LOGOS_ASSERT_FALSE(result.success);
}

LOGOS_TEST(send_succeeds_and_returns_request_id) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    t.mockCFunction("logosdelivery_send").returns("req-id-abc123");
    LogosResult result = plugin->send("/test/1/delivery/proto", QByteArray("hello world"));

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.getString().toStdString(), std::string("req-id-abc123"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_send"));

    delete plugin;
}

LOGOS_TEST(send_calls_ffi_with_byte_array_payload) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    t.mockCFunction("logosdelivery_send").returns("req-id-xyz");
    LogosResult result = plugin->send("/test/1/delivery/proto", QByteArray("test-payload"));

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_send"), 1);

    delete plugin;
}

LOGOS_TEST(send_returns_error_on_ffi_failure) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    DeliveryModulePlugin pluginNoCtx;
    LogosResult failResult = pluginNoCtx.send("/topic", QByteArray("payload"));
    LOGOS_ASSERT_FALSE(failResult.success);
    LOGOS_ASSERT_FALSE(failResult.getError<QString>().isEmpty());

    delete plugin;
}

// subscribe

LOGOS_TEST(subscribe_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_FALSE(plugin.subscribe("/test/1/delivery/proto").success);
}

LOGOS_TEST(subscribe_succeeds_with_context) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    LOGOS_ASSERT_TRUE(plugin->subscribe("/test/1/delivery/proto").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_subscribe"));

    delete plugin;
}

// unsubscribe

LOGOS_TEST(unsubscribe_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_FALSE(plugin.unsubscribe("/test/1/delivery/proto").success);
}

LOGOS_TEST(unsubscribe_succeeds_with_context) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    LOGOS_ASSERT_TRUE(plugin->unsubscribe("/test/1/delivery/proto").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_unsubscribe"));

    delete plugin;
}

// getAvailableNodeInfoIDs

LOGOS_TEST(getAvailableNodeInfoIDs_returns_mocked_string) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    t.mockCFunction("logosdelivery_get_available_node_info_ids").returns("@[Version,PeerID]");
    LogosResult result = plugin->getAvailableNodeInfoIDs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_available_node_info_ids"));
    LOGOS_ASSERT_EQ(result.getString().toStdString(), std::string("@[Version,PeerID]"));

    delete plugin;
}

LOGOS_TEST(getAvailableNodeInfoIDs_returns_empty_on_ffi_failure) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModulePlugin plugin;
    LogosResult result = plugin.getAvailableNodeInfoIDs();
    LOGOS_ASSERT_FALSE(result.success);
}

// getNodeInfo

LOGOS_TEST(getNodeInfo_returns_mocked_value_for_attribute) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    t.mockCFunction("logosdelivery_get_node_info").returns("v1.2.3");
    LogosResult result = plugin->getNodeInfo("Version");

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.getString().toStdString(), std::string("v1.2.3"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_node_info"));

    delete plugin;
}

// getAvailableConfigs

LOGOS_TEST(getAvailableConfigs_returns_mocked_json) {
    auto t = LogosTestContext("delivery_module");
    auto* plugin = createInitializedPlugin(t);

    t.mockCFunction("logosdelivery_get_available_configs").returns(R"([{"key":"mode","type":"string"}])");
    LogosResult result = plugin->getAvailableConfigs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_available_configs"));

    delete plugin;
}

LOGOS_TEST(getAvailableConfigs_returns_empty_on_ffi_failure) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModulePlugin plugin;
    LogosResult result = plugin.getAvailableConfigs();
    LOGOS_ASSERT_FALSE(result.success);
}

// module name

LOGOS_TEST(name_returns_delivery_module) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModulePlugin plugin;
    LOGOS_ASSERT_EQ(plugin.name().toStdString(), std::string("delivery_module"));
}
