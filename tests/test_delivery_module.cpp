// Unit tests for DeliveryModuleImpl.
// All liblogosdelivery C functions are mocked at link time via mock_liblogosdelivery.cpp.
// The mock plays the library's side of the poll model: every export queues its
// reply at once, so the module's pump finds it on the first poll; tests queue
// events and RLN questions through mock_rln_state.h.

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
#include "rln_presets.h"
#include "mocks/delivery_module_events_stub.h"
#include "mocks/mock_rln_state.h"
#include "rln_bridge.h"
#include "liblogos_rln_module_api.h"

// ---------------------------------------------------------------------------
// Helper: create an impl that has a valid delivery context (createNode called).
// ---------------------------------------------------------------------------
static DeliveryModuleImpl* createInitializedImpl(LogosTestContext& t) {
    t.mockCFunction("logosdelivery_create_node").returns(1);
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
    t.mockCFunction("logosdelivery_create_node").returns(1);
    auto* impl = new DeliveryModuleImpl();
    LOGOS_ASSERT_TRUE(impl->createNode(kRlnNodeCfg).success);
    return impl;
}

// createNode

LOGOS_TEST(createNode_succeeds_when_ffi_returns_non_null_context) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
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

// RLN: the library asks through reverse calls; the module answers by calling
// the RLN module and replying through logosdelivery_reverse_reply. The library
// is registry-agnostic: the registry and identifier on each request event are
// this module's own (from the preset).
LOGOS_TEST(createNode_declares_the_rln_plugin_in_the_create_request) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastRequestMethod, std::string("logosdelivery_create_node"));
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_lastCreateRlnPlugin);
    delete impl;
}

LOGOS_TEST(rln_question_is_answered_and_reported_as_event) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnRequestEvent();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    const uint64_t call = delivery_test_rln::pushReverseCall(
        "rln_generate_proof", nlohmann::json{{"signalHex", "ab01"}, {"timestamp", 1700000000}});
    // Any call drains the queue; the question is served before this call's reply.
    LOGOS_ASSERT_TRUE(impl->getAvailableConfigs().success);
    const auto& e = delivery_test_events::g_lastRlnRequest;
    LOGOS_ASSERT_EQ(e.op, std::string("generate_proof"));
    LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(call));
    LOGOS_ASSERT_EQ(e.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(e.rlnIdentifier, std::string("rln-id"));
    LOGOS_ASSERT_EQ(e.signalHex, std::string("ab01"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000000));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_reverse_reply"));
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastReplyCallId, call);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastReplyRet, 0);
    // Without a module context the bridge is disabled: the answer is the
    // op's own error shape, so the library is never left waiting.
    auto reply = nlohmann::json::parse(delivery_test_rln::g_lastReplyJson);
    LOGOS_ASSERT_FALSE(reply.value("success", true));
    delete impl;
}

LOGOS_TEST(rln_question_arriving_mid_call_is_served_before_the_call_returns) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnRequestEvent();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    // The library asks before it can answer `send`: the pump must serve the
    // question from inside the wait, or `send` could never complete.
    delivery_test_rln::interposeReverseCall(
        "rln_validate_proof",
        nlohmann::json{{"signalHex", "ab01"}, {"timestamp", 1700000003}, {"proofJson", R"({"proof":"00ff"})"}});
    LOGOS_ASSERT_TRUE(impl->send("/app/1/t/proto", {1, 2, 3}).success);
    const auto& e = delivery_test_events::g_lastRlnRequest;
    LOGOS_ASSERT_EQ(e.op, std::string("validate_proof"));
    LOGOS_ASSERT_EQ(e.proofJson, std::string(R"({"proof":"00ff"})"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000003));
    LOGOS_ASSERT_EQ(delivery_test_rln::g_replyCount, 1);
    delete impl;
}

LOGOS_TEST(rln_questions_route_to_their_events) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    const auto& e = delivery_test_events::g_lastRlnRequest;
    auto ask = [&](const char* wire, const nlohmann::json& args) {
        delivery_test_events::resetRlnRequestEvent();
        const uint64_t call = delivery_test_rln::pushReverseCall(wire, args);
        LOGOS_ASSERT_TRUE(impl->getAvailableConfigs().success);
        LOGOS_ASSERT_EQ(e.reqId, static_cast<int64_t>(call));
        LOGOS_ASSERT_EQ(e.registryId, std::string("reg"));
        LOGOS_ASSERT_EQ(e.rlnIdentifier, std::string("rln-id"));
        LOGOS_ASSERT_EQ(delivery_test_rln::g_lastReplyCallId, call);
    };
    ask("rln_get_membership_state", nlohmann::json::object());
    LOGOS_ASSERT_EQ(e.op, std::string("get_membership_state"));
    ask("rln_get_epoch_quota", {{"timestamp", 1700000001}});
    LOGOS_ASSERT_EQ(e.op, std::string("get_epoch_quota"));
    LOGOS_ASSERT_EQ(e.epochTimestamp, static_cast<int64_t>(1700000001));
    ask("rln_generate_proof", {{"signalHex", "ab01"}, {"timestamp", 1700000002}});
    LOGOS_ASSERT_EQ(e.op, std::string("generate_proof"));
    LOGOS_ASSERT_EQ(e.signalHex, std::string("ab01"));
    delete impl;
}

LOGOS_TEST(unknown_rln_question_is_refused_not_dropped) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    const uint64_t call = delivery_test_rln::pushReverseCall("rln_frobnicate", nlohmann::json::object());
    LOGOS_ASSERT_TRUE(impl->getAvailableConfigs().success);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastReplyCallId, call);
    LOGOS_ASSERT(delivery_test_rln::g_lastReplyRet != 0);
    delete impl;
}

LOGOS_TEST(send_request_carries_the_message_json) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);
    LOGOS_ASSERT_TRUE(impl->send("/app/1/t/proto", {1, 2, 3}).success);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastRequestMethod, std::string("logosdelivery_send"));
    auto msg = nlohmann::json::parse(delivery_test_rln::g_lastRequest.at("messageJson").get<std::string>());
    LOGOS_ASSERT_EQ(msg.value("contentTopic", ""), std::string("/app/1/t/proto"));
    LOGOS_ASSERT_EQ(msg.value("payload", ""), std::string("AQID"));
    delete impl;
}

LOGOS_TEST(createNode_without_an_rln_preset_installs_no_plugin) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    auto* impl = createInitializedImpl(t);

    LOGOS_ASSERT_FALSE(delivery_test_rln::g_lastCreateRlnPlugin);
    LOGOS_ASSERT_EQ(impl->rlnState().value.value("state", ""), std::string("Disabled"));

    delete impl;
}

// The shipped presets all carry RLN off, so naming one must not turn it on.
LOGOS_TEST(builtin_presets_leave_rln_off) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO","preset":"logos.test"})").success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_lastCreateRlnPlugin);
    LOGOS_ASSERT_EQ(impl.rlnState().value.value("state", ""), std::string("Disabled"));
}

LOGOS_TEST(an_rln_preset_installs_the_plugin_and_reports_bring_up) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnStateEvent();
    RlnPresetsFile presets(kRlnPresetTable);
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(kRlnNodeCfg).success);
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_lastCreateRlnPlugin);

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
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(kRlnNodeCfg).success);
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_lastCreateRlnPlugin);

    auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
    LOGOS_ASSERT_TRUE(libCfg.value("rln-disable-validation", false));
}

LOGOS_TEST(a_preset_without_the_flag_leaves_library_validation_on) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTable);
    t.mockCFunction("logosdelivery_create_node").returns(1);

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
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    StdLogosResult r = impl.createNode(kRlnNodeCfg);
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_lastCreateRlnPlugin);
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
    t.mockCFunction("logosdelivery_create_node").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.createNode(R"({"logLevel":"INFO","preset":"logostest"})").success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_lastCreateRlnPlugin);
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

LOGOS_TEST(name_returns_delivery_module) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_EQ(impl.name(), std::string("delivery_module"));
}

// ---------------------------------------------------------------------------
// RlnBridge, driven directly.
//
// The bridge is reachable here for the first time: it now calls the typed
// client, whose stub answers from the calling thread with whatever stubError()
// says, so a request op's whole reply path runs inside the test with no host,
// no chain and no waiting. What these pin is the SHAPE the delivery library
// receives — the two error dialects, and the classification that decides
// whether the library retries.
// ---------------------------------------------------------------------------

// A bridge wired to a stub client and enabled, which is all an op entry point
// needs — bringUpRlnBridge's context gate is a DeliveryModuleImpl concern.
struct BridgeFixture {
    LiblogosRlnModule client{"delivery_module"};
    RlnBridge bridge;

    BridgeFixture() {
        liblogos_rln_module_stub::resetStubError();
        delivery_test_rln::resetRlnMockState();
        RlnBridge::setResponder([](uint64_t reqId, const std::string& out) {
            delivery_test_rln::g_lastReplyCallId = reqId;
            delivery_test_rln::g_lastReplyJson = out;
            ++delivery_test_rln::g_replyCount;
        });
        bridge.init(&client);
    }
    ~BridgeFixture() { liblogos_rln_module_stub::resetStubError(); }
};

LOGOS_TEST(rln_bridge_tstr_transport_failure_answers_the_bare_error_object) {
    auto t = LogosTestContext("delivery_module");
    BridgeFixture f;
    LOGOS_ASSERT_EQ(f.bridge.enable(), std::string());

    f.bridge.getMembershipState(7, "reg", "rln-id");

    LOGOS_ASSERT_TRUE((delivery_test_rln::g_replyCount > 0));
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastReplyCallId, static_cast<uint64_t>(7));
    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastReplyJson);
    // tstr dialect: the error object sits at the top level, with no envelope.
    LOGOS_ASSERT_FALSE(reply.contains("success"));
    LOGOS_ASSERT_EQ(reply["error"]["class"].get<std::string>(),
                    std::string("transient"));
    LOGOS_ASSERT_EQ(reply["error"]["kind"].get<std::string>(),
                    std::string("rln_bridge_transport"));
}

LOGOS_TEST(rln_bridge_result_transport_failure_answers_the_envelope) {
    auto t = LogosTestContext("delivery_module");
    BridgeFixture f;
    LOGOS_ASSERT_EQ(f.bridge.enable(), std::string());

    f.bridge.getEpochQuota(8, "reg", "rln-id", 1700000000);

    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastReplyCallId, static_cast<uint64_t>(8));
    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastReplyJson);
    // result dialect: success=false, and the error arm is a JSON-ENCODED object.
    LOGOS_ASSERT_FALSE(reply["success"].get<bool>());
    const auto inner = nlohmann::json::parse(reply["error"].get<std::string>());
    LOGOS_ASSERT_EQ(inner["class"].get<std::string>(), std::string("transient"));
}

LOGOS_TEST(rln_bridge_provider_refusal_is_permanent) {
    auto t = LogosTestContext("delivery_module");
    BridgeFixture f;
    LOGOS_ASSERT_EQ(f.bridge.enable(), std::string());
    // A refusal is the module declining the call itself — a contract mismatch
    // no retry fixes, unlike every other CallError the bridge sees.
    liblogos_rln_module_stub::stubError() = logos::CallError{
        "invalid_args", "rate_limit is not a string", "liblogos_rln_module"};

    f.bridge.generateProof(9, "reg", "rln-id", "deadbeef", 1700000000);

    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastReplyJson);
    const auto inner = nlohmann::json::parse(reply["error"].get<std::string>());
    LOGOS_ASSERT_EQ(inner["class"].get<std::string>(), std::string("permanent"));
    LOGOS_ASSERT_EQ(inner["kind"].get<std::string>(),
                    std::string("rln_bridge_dispatch"));
}

LOGOS_TEST(rln_bridge_disabled_still_answers_the_request) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnBridge bridge; // never init()ed, so never enabled

    bridge.validateProof(10, "reg", "rln-id", "deadbeef", 1700000000, "{}");

    // The library is owed an answer for every reqId it raises, including the
    // ones this bridge cannot serve — an unserved request only goes quiet and
    // expires against the library's own budget.
    LOGOS_ASSERT_TRUE((delivery_test_rln::g_replyCount > 0));
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastReplyCallId, static_cast<uint64_t>(10));
    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastReplyJson);
    const auto inner = nlohmann::json::parse(reply["error"].get<std::string>());
    // Permanent: enable() happens once, at createNode. Retrying cannot help.
    LOGOS_ASSERT_EQ(inner["class"].get<std::string>(), std::string("permanent"));
    LOGOS_ASSERT_EQ(inner["kind"].get<std::string>(),
                    std::string("rln_bridge_disabled"));
}

LOGOS_TEST(rln_bridge_enable_refuses_without_a_client) {
    auto t = LogosTestContext("delivery_module");
    RlnBridge bridge; // no init(): nothing to call

    // The op entry points dereference the client without checking it, which is
    // safe only because this refusal holds: an un-enabled bridge is never
    // asked, since the plugin gates every callback on enabled().
    LOGOS_ASSERT_FALSE(bridge.enable().empty());
    LOGOS_ASSERT_FALSE(bridge.enabled());
}

LOGOS_TEST(rln_bridge_start_backend_reports_the_call_error) {
    auto t = LogosTestContext("delivery_module");
    BridgeFixture f;
    LOGOS_ASSERT_EQ(f.bridge.enable(), std::string());

    // Lifecycle answers the caller, not the library: text on failure, empty on
    // success.
    const std::string err = f.bridge.startBackend(R"({"registries":[]})");
    LOGOS_ASSERT_FALSE(err.empty());
    LOGOS_ASSERT_FALSE((delivery_test_rln::g_replyCount > 0));
}

LOGOS_TEST(rln_bridge_start_backend_refuses_before_enable) {
    auto t = LogosTestContext("delivery_module");
    liblogos_rln_module_stub::resetStubError();
    LiblogosRlnModule client{"delivery_module"};
    RlnBridge bridge;
    bridge.init(&client);

    LOGOS_ASSERT_EQ(bridge.startBackend("{}"),
                    std::string("rln bridge is not enabled"));
}
