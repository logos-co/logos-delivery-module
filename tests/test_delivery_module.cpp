// Unit tests for DeliveryModuleImpl.
// All liblogosdelivery C functions are mocked at link time via mock_liblogosdelivery.cpp.
// Mocks invoke callbacks synchronously so the semaphore inside api_call_handler.h
// is released before try_acquire_for starts waiting.

#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>

#include <logos_test.h>
#include <nlohmann/json.hpp>
#include "delivery_module_plugin.h"
#include "base64.h"
#include "discovery_config.h"
#include "service_discovery_plugin.h"
#include "libp2p_module_api.h"
#include "rln_presets.h"
#include "mocks/delivery_module_events_stub.h"
#include "mocks/mock_rln_state.h"

// ---------------------------------------------------------------------------
// Helper: create an impl that has a valid delivery context (createNode called).
// ---------------------------------------------------------------------------
static DeliveryModuleImpl* createInitializedImpl(LogosTestContext& t) {
    t.mockCFunction("logosdelivery_ctx_create").returns(1);
    auto* impl = new DeliveryModuleImpl();
    LOGOS_ASSERT_TRUE(impl->createNode(R"({"logLevel":"INFO"})").success);
    return impl;
}

// RLN is never in the node config and has no method of its own: it comes from
// the network preset. This is what a deployment's own presets file looks like,
// staged and pointed at by LOGOS_DELIVERY_RLN_PRESETS.
static constexpr const char* kRlnPresetTable = R"({
  "logos.test": {
    "enabled": true,
    "registry-id": "reg",
    "rln-identifier": "rln-id",
    "epoch-size-sec": 600
  }
})";

static constexpr const char* kRlnNodeCfg = R"({"logLevel":"INFO","preset":"logos.test"})";

// Stages a presets file and points the env var at it for this scope.
class RlnPresetsFile {
public:
    explicit RlnPresetsFile(const char* table) {
        path_ = std::filesystem::temp_directory_path()
                / ("delivery-rln-presets-" + std::to_string(::getpid()) + ".json");
        std::ofstream(path_) << table;
        ::setenv(kRlnPresetsEnvVar, path_.c_str(), 1);
    }
    ~RlnPresetsFile() {
        ::unsetenv(kRlnPresetsEnvVar);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

private:
    std::filesystem::path path_;
};

// Bring-up runs off the createNode thread, so settle before asserting on it.
static std::string settledRlnState(DeliveryModuleImpl& impl) {
    for (int i = 0; i < 500; ++i) {
        const std::string state = impl.rlnState().value.value("state", "");
        if (state != "Initializing") {
            return state;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return "Initializing";
}

// rlnState() changes before rlnStateChanged fires, so a settled state does not
// mean the event has been recorded yet: wait for the event itself.
static delivery_test_events::RlnStateEvent awaitRlnStateEvents(int transitions) {
    for (int i = 0; i < 500; ++i) {
        const auto event = delivery_test_events::lastRlnState();
        if (event.transitions >= transitions) {
            return event;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return delivery_test_events::lastRlnState();
}

// Without a framework context the bridge cannot come up, so bring-up settles
// on Failed. The plugin is installed synchronously either way, which is what
// the RLN callback tests below need.
static DeliveryModuleImpl* createRlnImpl(LogosTestContext& t) {
    t.mockCFunction("logosdelivery_ctx_create").returns(1);
    auto* impl = new DeliveryModuleImpl();
    LOGOS_ASSERT_TRUE(impl->createNode(kRlnNodeCfg).success);
    return impl;
}

// createNode

LOGOS_TEST(createNode_succeeds_when_ffi_returns_non_null_context) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_create"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_add_event_listener"));
}

LOGOS_TEST(createNode_fails_when_ffi_returns_null) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(0);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_create"));
}

LOGOS_TEST(createNode_tracks_call_count) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    impl.createNode(R"({"logLevel":"INFO"})");
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_create"), 1);
}

LOGOS_TEST(createNode_succeeds_with_logos_dev_preset_config) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"DEBUG","mode":"Core","preset":"logos.dev"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_create"));
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
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_start_node"));

    delete impl;
}

LOGOS_TEST(start_calls_ffi_start_node) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    impl->start();
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_start_node"), 1);

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
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_stop_node"));

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
    t.mockCFunction("logosdelivery_ctx_start_node").returns(1);
    LOGOS_ASSERT_FALSE(impl->start().success);
    LOGOS_ASSERT_FALSE(delivery_test_events::g_lastNodeStarted.fired);

    delete impl;
}

LOGOS_TEST(stop_returns_false_when_dispatch_fails) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_ctx_stop_node").returns(1);
    LOGOS_ASSERT_FALSE(impl->stop().success);
    LOGOS_ASSERT_FALSE(delivery_test_events::g_lastNodeStopped.fired);

    delete impl;
}

// The library marks a node started before external service discovery comes
// up, so a discovery failure leaves it half up. The module stops it again.
LOGOS_TEST(failed_start_stops_the_half_started_node) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    delivery_test_rln::g_startNodeReplyError = "failed to start external service discovery";
    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.fired);
    LOGOS_ASSERT_FALSE(delivery_test_events::g_lastNodeStarted.success);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.message ==
                      "failed to start external service discovery");

    delete impl; // joins the stop, which runs on a thread of the module's own
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_stop_node"), 1);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStopped.fired);
    delivery_test_rln::resetRlnMockState();
}

LOGOS_TEST(successful_start_does_not_stop_the_node) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_events::resetNodeLifecycleEvents();
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.success);

    delete impl;
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_stop_node"), 0);
}

// libp2p_module is optional. A node configured for external service discovery
// without it must fail its discovery start saying so, not with a timeout.
LOGOS_TEST(discovery_start_reports_absent_libp2p_module) {
    Libp2pModule libp2p("delivery_module");
    Libp2pModule::createNodeErrorCode = "object_unavailable";
    DeliveryServiceDiscoveryPlugin plugin(&libp2p, "{}");

    char err[1024] = {};
    const LdServiceDiscoveryPlugin* vt = plugin.vtable();
    const int rc = vt->start(vt->pluginCtx, err, sizeof(err));
    Libp2pModule::createNodeErrorCode.clear();

    LOGOS_ASSERT_EQ(rc, LD_DISCO_ERROR);
    const std::string reason(err);
    LOGOS_ASSERT_TRUE(reason.find("libp2p_module is not available") != std::string::npos);
    LOGOS_ASSERT_TRUE(reason.find("internal discovery") != std::string::npos);
}

LOGOS_TEST(discovery_start_without_libp2p_client_fails_cleanly) {
    DeliveryServiceDiscoveryPlugin plugin(nullptr, "{}");

    char err[256] = {};
    const LdServiceDiscoveryPlugin* vt = plugin.vtable();
    LOGOS_ASSERT_EQ(vt->start(vt->pluginCtx, err, sizeof(err)), LD_DISCO_ERROR);
    LOGOS_ASSERT_TRUE(std::string(err).find("no libp2p_module client") != std::string::npos);
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

    t.mockCFunction("logosdelivery_ctx_send").returns("req-id-abc123");
    std::vector<uint8_t> payload{'h','e','l','l','o',' ','w','o','r','l','d'};
    StdLogosResult result = impl->send("/test/1/delivery/proto", payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("req-id-abc123"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_send"));

    delete impl;
}

LOGOS_TEST(send_calls_ffi_with_byte_array_payload) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_ctx_send").returns("req-id-xyz");
    std::vector<uint8_t> payload{'t','e','s','t','-','p','a','y','l','o','a','d'};
    StdLogosResult result = impl->send("/test/1/delivery/proto", payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_send"), 1);

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
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_subscribe"));

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
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_unsubscribe"));

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
    t.mockCFunction("logosdelivery_ctx_waku_store_query").returns(responseJson);

    StdLogosResult result = impl->storeQuery(
        R"({"requestId":"req-1","includeData":true,"paginationForward":true})",
        "/ip4/127.0.0.1/tcp/60000/p2p/16Uiu2peer", 5000);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string(responseJson));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_waku_store_query"), 1);

    delete impl;
}

// channelCreate

LOGOS_TEST(channelCreate_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.channelCreate("chan-1", "/test/1/delivery/proto", "sender-1").success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_ctx_channel_create"));
}

LOGOS_TEST(channelCreate_returns_channel_id) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_ctx_channel_create").returns("chan-1");
    StdLogosResult result = impl->channelCreate("chan-1", "/test/1/delivery/proto", "sender-1");

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("chan-1"));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_channel_create"), 1);

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
    t.mockCFunction("logosdelivery_ctx_channel_exists").returns("true");
    StdLogosResult existing = impl->channelExists("chan-1");
    LOGOS_ASSERT_TRUE(existing.success);
    LOGOS_ASSERT_EQ(existing.value.get<std::string>(), std::string("true"));

    t.mockCFunction("logosdelivery_ctx_channel_exists").returns("false");
    StdLogosResult missing = impl->channelExists("no-such-chan");
    LOGOS_ASSERT_TRUE(missing.success);
    LOGOS_ASSERT_EQ(missing.value.get<std::string>(), std::string("false"));

    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_channel_exists"), 2);

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

    t.mockCFunction("logosdelivery_ctx_channel_send").returns("req-id-chan-42");
    std::vector<uint8_t> payload{'h','e','l','l','o',' ','c','h','a','n'};
    StdLogosResult result = impl->channelSend("chan-1", payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("req-id-chan-42"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_channel_send"));

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
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_channel_close"));

    delete impl;
}

// getAvailableNodeInfoIDs

LOGOS_TEST(getAvailableNodeInfoIDs_returns_mocked_string) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_ctx_get_available_node_info_ids").returns(R"(["Version","MyPeerId"])");
    StdLogosResult result = impl->getAvailableNodeInfoIDs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_get_available_node_info_ids"));
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

    t.mockCFunction("logosdelivery_ctx_get_node_info").returns("v1.2.3");
    StdLogosResult result = impl->getNodeInfo("Version");

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("v1.2.3"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_get_node_info"));

    delete impl;
}

// getAvailableConfigs

LOGOS_TEST(getAvailableConfigs_returns_mocked_json) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_ctx_get_available_configs").returns(R"([{"key":"mode","type":"string"}])");
    StdLogosResult result = impl->getAvailableConfigs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_get_available_configs"));

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
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_ctx_get_node_info"));
}

LOGOS_TEST(collectOpenMetricsText_returns_metrics_text_verbatim) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    const char* promText =
        "# HELP waku_node_messages_total number of messages\n"
        "# TYPE waku_node_messages_total counter\n"
        "waku_node_messages_total{shard=\"0\"} 42\n";
    t.mockCFunction("logosdelivery_ctx_get_node_info").returns(promText);

    // The module is a pure passthrough: the openmetrics scraper does the parsing.
    LOGOS_ASSERT_EQ(impl->collectOpenMetricsText(), std::string(promText));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_get_node_info"));

    delete impl;
}

// RLN bridge (liblogosdelivery_rln.h)

LOGOS_TEST(createNode_installs_rln_plugin) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTable);
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

// The stop after a failed start and an explicit stop() both stop the RLN
// backend, and both join the bring-up thread on the way -- from different
// threads, possibly at once. They must not race that join.
LOGOS_TEST(failed_start_and_explicit_stop_share_the_rln_teardown) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetNodeLifecycleEvents();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);

    delivery_test_rln::g_startNodeReplyError = "failed to start external service discovery";
    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT_TRUE(impl->stop().success);

    delete impl;
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_stop_node"), 2);
    delivery_test_rln::resetRlnMockState();
}

LOGOS_TEST(rln_generate_proof_callback_emits_typed_event_with_verbatim_args) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnRequestEvent();
    RlnPresetsFile presets(kRlnPresetTable);
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
    RlnPresetsFile presets(kRlnPresetTable);
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

LOGOS_TEST(createNode_without_an_rln_preset_installs_no_plugin) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_rln_set_plugin"));
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_callbacksSet);
    LOGOS_ASSERT_EQ(impl->rlnState().value.value("state", ""), std::string("Disabled"));

    delete impl;
}

// The shipped presets all carry RLN off, so naming one must not turn it on.
LOGOS_TEST(builtin_presets_leave_rln_off) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO","preset":"logos.test"})").success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_callbacksSet);
    LOGOS_ASSERT_EQ(impl.rlnState().value.value("state", ""), std::string("Disabled"));
}

LOGOS_TEST(an_rln_preset_installs_the_plugin_and_reports_bring_up) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnStateEvent();
    RlnPresetsFile presets(kRlnPresetTable);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(kRlnNodeCfg).success);
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_callbacksSet);

    // No framework context in a unit test, so the bridge cannot come up.
    LOGOS_ASSERT_EQ(settledRlnState(impl), std::string("Failed"));
    // Initializing, then Failed.
    const auto event = awaitRlnStateEvents(2);
    LOGOS_ASSERT_EQ(event.state, std::string("Failed"));
    LOGOS_ASSERT_EQ(event.transitions, 2);
}

// kRlnPresetTable with validation disabled.
static constexpr const char* kRlnPresetTableNoValidation = R"({
  "logos.test": {
    "enabled": true,
    "registry-id": "reg",
    "rln-identifier": "rln-id",
    "epoch-size-sec": 600,
    "enable-validation": false
  }
})";

// The switch travels to the library inside the createNode config, spelled as
// the library spells it; the client config never carries it.
LOGOS_TEST(a_preset_with_validation_off_disables_it_in_the_library_config) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTableNoValidation);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(kRlnNodeCfg).success);
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_callbacksSet);

    auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
    LOGOS_ASSERT_TRUE(libCfg.value("rln-disable-validation", false));
}

LOGOS_TEST(a_preset_without_the_flag_leaves_library_validation_on) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTable);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(kRlnNodeCfg).success);

    auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
    LOGOS_ASSERT_FALSE(libCfg.contains("rln-disable-validation"));
}

// A presets file that cannot be used fails node creation rather than quietly
// producing a node without the rate limiting its deployment expects.
LOGOS_TEST(a_broken_presets_file_fails_createNode) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(R"({"logos.test": {"enabled": true, "registry-id": "reg"}})");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    StdLogosResult r = impl.createNode(kRlnNodeCfg);
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_callbacksSet);
}

LOGOS_TEST(preset_names_are_matched_exactly) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTable, table).empty());
    LOGOS_ASSERT_EQ(table.count("logos.test"), static_cast<size_t>(1));
    LOGOS_ASSERT_TRUE(table["logos.test"].enabled);
    LOGOS_ASSERT_EQ(table["logos.test"].epochSizeSec, static_cast<uint64_t>(600));

    // A variant spelling is an error where it is written, not a silent miss.
    LOGOS_ASSERT_FALSE(parseRlnPresetTable(R"({"logostest":{"enabled":false}})", table).empty());
    LOGOS_ASSERT_FALSE(parseRlnPresetTable(R"({"LogosTest":{"enabled":false}})", table).empty());
    LOGOS_ASSERT_FALSE(parseRlnPresetTable(R"({"nosuchnet":{"enabled":false}})", table).empty());
}

// Same rule on the lookup side: a node asking for a spelling this module does
// not carry fails rather than coming up quietly without rate limiting.
LOGOS_TEST(createNode_rejects_a_preset_spelling_it_does_not_know) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.createNode(R"({"logLevel":"INFO","preset":"logostest"})").success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_callbacksSet);
}

// The RLN module rejects a start config without a positive epoch size and has
// no default, so an enabled preset missing one is caught here instead.
LOGOS_TEST(preset_table_rejects_incomplete_enabled_entries) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_FALSE(parseRlnPresetTable("not json", table).empty());
    LOGOS_ASSERT_FALSE(
        parseRlnPresetTable(R"({"logos.test":{"enabled":true,"rln-identifier":"x","epoch-size-sec":1}})", table)
            .empty());
    LOGOS_ASSERT_FALSE(
        parseRlnPresetTable(R"({"logos.test":{"enabled":true,"registry-id":"r","rln-identifier":"x"}})", table)
            .empty());
    // A disabled entry needs none of them.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(R"({"logos.test":{"enabled":false}})", table).empty());
}

// The validation switch is opt-out: a table that does not mention it validates.
LOGOS_TEST(an_omitted_enable_validation_defaults_to_on) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTable, table).empty());
    LOGOS_ASSERT_TRUE(table["logos.test"].enableValidation);

    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTableNoValidation, table).empty());
    LOGOS_ASSERT_FALSE(table["logos.test"].enableValidation);
}

// The identifier scopes the application, so a deployment that does not name
// one still has to agree with every other Logos Delivery node.
LOGOS_TEST(an_omitted_rln_identifier_defaults_to_this_application) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(
        parseRlnPresetTable(R"({"logos.test":{"enabled":true,"registry-id":"r","epoch-size-sec":600}})",
                            table)
            .empty());
    LOGOS_ASSERT_EQ(table["logos.test"].rlnIdentifier, std::string(kLogosDeliveryRlnIdentifier));

    // sha256("rln/logos-delivery/v0.0.1"), as 32 bytes of hex.
    LOGOS_ASSERT_EQ(std::string(kLogosDeliveryRlnIdentifier).size(), static_cast<size_t>(64));
    LOGOS_ASSERT_EQ(
        std::string(kLogosDeliveryRlnIdentifier),
        std::string("5e269b6a19fce081f5808b13442dcbc3522197638dd38df5a28bc4e55236b977"));
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
    RlnPresetsFile presets(kRlnPresetTable);
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

// One base64 for both plugins (payloads over the FFI, the signed record over
// libp2p's JSON transport). RFC 4648 vectors, including the padding cases.
LOGOS_TEST(base64_encode_matches_rfc4648_vectors) {
    const auto enc = [](const char* s) {
        return delivery_base64::encode(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
    };
    LOGOS_ASSERT(enc("") == "");
    LOGOS_ASSERT(enc("f") == "Zg==");
    LOGOS_ASSERT(enc("fo") == "Zm8=");
    LOGOS_ASSERT(enc("foo") == "Zm9v");
    LOGOS_ASSERT(enc("foob") == "Zm9vYg==");
    LOGOS_ASSERT(enc("fooba") == "Zm9vYmE=");
    LOGOS_ASSERT(enc("foobar") == "Zm9vYmFy");
    const uint8_t binary[] = {0x00, 0xff, 0x10};
    LOGOS_ASSERT(delivery_base64::encode(binary, 3) == "AP8Q");
    LOGOS_ASSERT(delivery_base64::encode(nullptr, 0).empty());
    LOGOS_ASSERT(delivery_base64::decode("Zm9vYmFy") == std::vector<uint8_t>({'f', 'o', 'o', 'b', 'a', 'r'}));
}

// discovery config (discovery_config.h): the node's requirements reply

static nlohmann::json parseJson(const char* text) { return nlohmann::json::parse(text); }

static const char* kEnabledReply =
    R"({"externalServiceDiscovery":true,"bootstrapNodes":[
        "/dns4/a.example/tcp/30303/p2p/16Uiu2HAmA",
        "/ip4/10.0.0.2/tcp/30303/p2p/16Uiu2HAmB"]})";
static const char* kDisabledReply = R"({"externalServiceDiscovery":false,"bootstrapNodes":[]})";

// Sets LIBP2P_MODULE_CONFIG for a test body and restores it afterwards.
struct ScopedLibp2pEnv {
    std::string saved;
    bool had;
    explicit ScopedLibp2pEnv(const char* value)
    {
        const char* old = getenv("LIBP2P_MODULE_CONFIG");
        had = old != nullptr;
        if (had) saved = old;
        setenv("LIBP2P_MODULE_CONFIG", value, 1);
    }
    ~ScopedLibp2pEnv()
    {
        if (had) setenv("LIBP2P_MODULE_CONFIG", saved.c_str(), 1);
        else unsetenv("LIBP2P_MODULE_CONFIG");
    }
};

LOGOS_TEST(discovery_split_bootstrap_address) {
    nlohmann::json node;
    LOGOS_ASSERT_TRUE(delivery_discovery::splitBootstrapAddress(
        "/dns4/a.example/tcp/30303/p2p/16Uiu2HAmA", node));
    LOGOS_ASSERT_EQ(node["peerId"].get<std::string>(), std::string("16Uiu2HAmA"));
    LOGOS_ASSERT_EQ(node["addrs"][0].get<std::string>(), std::string("/dns4/a.example/tcp/30303"));
    LOGOS_ASSERT_FALSE(delivery_discovery::splitBootstrapAddress("/ip4/10.0.0.2/tcp/1", node));
    LOGOS_ASSERT_FALSE(delivery_discovery::splitBootstrapAddress("/p2p/16Uiu2HAmA", node));
}

LOGOS_TEST(discovery_from_requirements_disabled_means_no_plugin) {
    delivery_discovery::PluginRequest req;
    LOGOS_ASSERT_TRUE(delivery_discovery::fromRequirements(kDisabledReply, nlohmann::json::object(), req).empty());
    LOGOS_ASSERT_FALSE(req.enabled);
    LOGOS_ASSERT_TRUE(req.libp2pConfig.empty());
}

LOGOS_TEST(discovery_from_requirements_builds_the_libp2p_config) {
    delivery_discovery::PluginRequest req;
    LOGOS_ASSERT_TRUE(delivery_discovery::fromRequirements(kEnabledReply, nlohmann::json::object(), req).empty());
    LOGOS_ASSERT_TRUE(req.enabled);
    const auto libp2p = nlohmann::json::parse(req.libp2pConfig);
    LOGOS_ASSERT_TRUE(libp2p["mountKad"].get<bool>());
    LOGOS_ASSERT_TRUE(libp2p["mountServiceDiscovery"].get<bool>());
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"].size(), size_t{2});
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"][1]["peerId"].get<std::string>(), std::string("16Uiu2HAmB"));
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"][1]["addrs"][0].get<std::string>(), std::string("/ip4/10.0.0.2/tcp/30303"));
}

LOGOS_TEST(discovery_from_requirements_keeps_libp2p_own_config_underneath) {
    // The node decides the DHT peers and the mounts; everything else in
    // libp2p's own config survives.
    delivery_discovery::PluginRequest req;
    const auto base = parseJson(R"({"addrs":["/ip4/0.0.0.0/tcp/9000"],"transport":"tcp",
        "mountKad":false,"bootstrapNodes":[{"peerId":"stale","addrs":["/ip4/1.1.1.1/tcp/1"]}]})");
    LOGOS_ASSERT_TRUE(delivery_discovery::fromRequirements(kEnabledReply, base, req).empty());
    const auto libp2p = nlohmann::json::parse(req.libp2pConfig);
    LOGOS_ASSERT_EQ(libp2p["addrs"][0].get<std::string>(), std::string("/ip4/0.0.0.0/tcp/9000"));
    LOGOS_ASSERT_EQ(libp2p["transport"].get<std::string>(), std::string("tcp"));
    LOGOS_ASSERT_TRUE(libp2p["mountKad"].get<bool>());
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"].size(), size_t{2});
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"][0]["peerId"].get<std::string>(), std::string("16Uiu2HAmA"));
}

LOGOS_TEST(discovery_libp2p_env_config_is_read_like_libp2p_module_does) {
    {
        ScopedLibp2pEnv env(R"({"addrs":["/ip4/127.0.0.1/tcp/7"]})");
        const auto cfg = delivery_discovery::libp2pEnvConfig();
        LOGOS_ASSERT_EQ(cfg["addrs"][0].get<std::string>(), std::string("/ip4/127.0.0.1/tcp/7"));
    }
    {
        ScopedLibp2pEnv env("not json");
        LOGOS_ASSERT_TRUE(delivery_discovery::libp2pEnvConfig().empty());
    }
    {
        ScopedLibp2pEnv env("");
        LOGOS_ASSERT_TRUE(delivery_discovery::libp2pEnvConfig().empty());
    }
}

LOGOS_TEST(discovery_from_requirements_rejects_bad_input) {
    for (const char* reply : {
             "", "not json", "[]", R"({"bootstrapNodes":[]})",
             R"({"externalServiceDiscovery":"yes"})",
             R"({"externalServiceDiscovery":true,"bootstrapNodes":"x"})",
             R"({"externalServiceDiscovery":true,"bootstrapNodes":["/ip4/10.0.0.2/tcp/1"]})",
         }) {
        delivery_discovery::PluginRequest req;
        LOGOS_ASSERT_FALSE(delivery_discovery::fromRequirements(reply, nlohmann::json::object(), req).empty());
        LOGOS_ASSERT_FALSE(req.enabled);
    }
}

// createNode: plugin path, driven by the node's answer

LOGOS_TEST(createNode_installs_plugin_when_the_node_asks_for_it) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);
    t.mockCFunction("logosdelivery_ctx_get_discovery_requirements").returns(kEnabledReply);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"preset":"logos.dev","messagingOverrides":{"pluginKadDiscovery":true}})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_get_discovery_requirements"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_set_service_discovery_plugin"));
}

LOGOS_TEST(createNode_skips_plugin_when_the_node_wants_none) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);
    t.mockCFunction("logosdelivery_ctx_get_discovery_requirements").returns(kDisabledReply);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"preset":"logos.test"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_get_discovery_requirements"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_ctx_set_service_discovery_plugin"));
}

LOGOS_TEST(createNode_fails_on_a_malformed_requirements_reply) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);
    t.mockCFunction("logosdelivery_ctx_get_discovery_requirements").returns("nonsense");

    DeliveryModuleImpl impl;
    const auto r = impl.createNode(R"({"preset":"logos.test"})");
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_TRUE(r.error.find("discovery") != std::string::npos);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_ctx_set_service_discovery_plugin"));
}
