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
#include "rln_bridge.h"
#include "rln_module_api.h"

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

// libp2p gets every bootstrap peer the node resolved, in order: with only the
// first, an unreachable first entry left kademlia with nobody to join.
LOGOS_TEST(discovery_hands_libp2p_every_bootstrap_peer) {
    Libp2pModule::reset();
    Libp2pModule::bringUpSucceeds = true;
    Libp2pModule libp2p("delivery_module");
    DeliveryServiceDiscoveryPlugin plugin(&libp2p, R"({"bootstrapNodes":[
        {"peerId":"16Uiu2HAmA","addrs":["/ip4/10.0.0.1/tcp/1"]},
        {"peerId":"16Uiu2HAmB","addrs":["/ip4/10.0.0.2/tcp/1"]},
        {"peerId":"16Uiu2HAmC","addrs":["/ip4/10.0.0.3/tcp/1"]}],
        "mountKad":true,"mountServiceDiscovery":true})");

    char err[1024] = {};
    const LdServiceDiscoveryPlugin* vt = plugin.vtable();
    const int rc = vt->start(vt->pluginCtx, err, sizeof(err));
    const auto handed = nlohmann::json::parse(Libp2pModule::lastCreateNodeConfig);
    Libp2pModule::reset();

    LOGOS_ASSERT_EQ(rc, LD_DISCO_OK);
    LOGOS_ASSERT_EQ(handed["bootstrapNodes"].size(), size_t{3});
    LOGOS_ASSERT_EQ(handed["bootstrapNodes"][0]["peerId"].get<std::string>(), std::string("16Uiu2HAmA"));
    LOGOS_ASSERT_EQ(handed["bootstrapNodes"][2]["peerId"].get<std::string>(), std::string("16Uiu2HAmC"));
}

// libp2p's start outlives its own 10s call deadline while kademlia bootstraps,
// and keeps running. A second start issued meanwhile runs a second switch start
// alongside the first -- two accept loops on one TCP listener, which crashes
// libp2p_module on the next inbound connection. So a timed-out start counts as
// started, whichever layer's deadline it met, and is never issued again.
LOGOS_TEST(discovery_start_timeout_is_a_notice_and_never_reissued) {
    for (const bool atTransport : {false, true}) {
        Libp2pModule::reset();
        Libp2pModule::bringUpSucceeds = true;
        if (atTransport) {
            Libp2pModule::startErrorCode = "timeout";
        } else {
            Libp2pModule::startError = "Failed to start libp2p: timeout";
        }
        Libp2pModule libp2p("delivery_module");
        DeliveryServiceDiscoveryPlugin plugin(&libp2p, "{}");

        char err[1024] = {};
        const LdServiceDiscoveryPlugin* vt = plugin.vtable();
        const int rc = vt->start(vt->pluginCtx, err, sizeof(err));
        vt->registerInterest(vt->pluginCtx, "service:/logos/delivery", err, sizeof(err));
        const int startCalls = Libp2pModule::startCalls;
        Libp2pModule::reset();

        LOGOS_ASSERT_EQ(rc, LD_DISCO_OK);
        LOGOS_ASSERT_EQ(startCalls, 1);
    }
}

// The plugin's entry points are called from Nim over the C ABI: a malformed
// record must not reach std::terminate, and whatever throws is an error reply.
LOGOS_TEST(discovery_lookup_survives_a_record_with_a_non_string_peer_id) {
    Libp2pModule::reset();
    Libp2pModule::bringUpSucceeds = true;
    Libp2pModule::lookupRecords = nlohmann::json::parse(
        R"([{"peerId":42,"addrs":[]},{"peerId":null},{"peerId":"16Uiu2HAmA","addrs":[]}])");
    Libp2pModule libp2p("delivery_module");
    DeliveryServiceDiscoveryPlugin plugin(&libp2p, "{}");

    char err[1024] = {};
    char* out = nullptr;
    const LdServiceDiscoveryPlugin* vt = plugin.vtable();
    const int rc = vt->lookup(vt->pluginCtx, "service:/logos/delivery", 0, &out, err, sizeof(err));
    Libp2pModule::reset();

    LOGOS_ASSERT_EQ(rc, LD_DISCO_OK);
    LOGOS_ASSERT_TRUE(out != nullptr);
    LOGOS_ASSERT_EQ(nlohmann::json::parse(out).size(), size_t{3});
    vt->freeString(vt->pluginCtx, out);
}

LOGOS_TEST(discovery_entry_point_turns_an_exception_into_an_error) {
    Libp2pModule::reset();
    Libp2pModule::bringUpSucceeds = true;
    Libp2pModule::lookupThrows = true;
    Libp2pModule libp2p("delivery_module");
    DeliveryServiceDiscoveryPlugin plugin(&libp2p, "{}");

    char err[1024] = {};
    char* out = nullptr;
    const LdServiceDiscoveryPlugin* vt = plugin.vtable();
    const int rc = vt->lookup(vt->pluginCtx, "service:/logos/delivery", 0, &out, err, sizeof(err));
    Libp2pModule::reset();

    LOGOS_ASSERT_EQ(rc, LD_DISCO_ERROR);
    LOGOS_ASSERT_TRUE(std::string(err).find("stub lookup threw") != std::string::npos);
}

// An explicit refusal is not a start in flight: the bring-up fails, and the
// next call may try again.
LOGOS_TEST(discovery_start_refusal_fails_the_bring_up) {
    Libp2pModule::reset();
    Libp2pModule::bringUpSucceeds = true;
    Libp2pModule::startError = "Failed to start libp2p: address already in use";
    Libp2pModule libp2p("delivery_module");
    DeliveryServiceDiscoveryPlugin plugin(&libp2p, "{}");

    char err[1024] = {};
    const LdServiceDiscoveryPlugin* vt = plugin.vtable();
    const int rc = vt->start(vt->pluginCtx, err, sizeof(err));
    const std::string reason(err);
    vt->registerInterest(vt->pluginCtx, "service:/logos/delivery", err, sizeof(err));
    const int startCalls = Libp2pModule::startCalls;
    Libp2pModule::reset();

    LOGOS_ASSERT_EQ(rc, LD_DISCO_ERROR);
    LOGOS_ASSERT_TRUE(reason.find("address already in use") != std::string::npos);
    LOGOS_ASSERT_EQ(startCalls, 2);
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

LOGOS_TEST(send_succeeds_and_returns_message_hash) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_ctx_send").returns("0xabc123");
    std::vector<uint8_t> payload{'h','e','l','l','o',' ','w','o','r','l','d'};
    StdLogosResult result = impl->send("/test/1/delivery/proto", payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("0xabc123"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_send"));

    delete impl;
}

LOGOS_TEST(send_calls_ffi_with_byte_array_payload) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_ctx_send").returns("0xdef456");
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

    t.mockCFunction("logosdelivery_ctx_get_available_node_info_ids").returns(R"(["Version","PeerId"])");
    StdLogosResult result = impl->getAvailableNodeInfoIDs();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_get_available_node_info_ids"));
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string(R"(["Version","PeerId"])"));

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

// getConnectionStatus

LOGOS_TEST(getConnectionStatus_returns_mocked_status) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createInitializedImpl(t);

    t.mockCFunction("logosdelivery_ctx_get_connection_status").returns("PartiallyConnected");
    StdLogosResult result = impl->getConnectionStatus();

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(result.value.get<std::string>(), std::string("PartiallyConnected"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_get_connection_status"));

    delete impl;
}

LOGOS_TEST(getConnectionStatus_fails_without_createNode) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleImpl impl;
    StdLogosResult result = impl.getConnectionStatus();
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_ctx_get_connection_status"));
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

// RLN lives with the node context: neither stop() nor a later start() touches
// it. Without a framework context the bring-up settled on Failed, and it stays
// there, with no further transition.
LOGOS_TEST(stop_and_start_leave_rln_as_it_is) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnStateEvent();
    delivery_test_events::resetNodeLifecycleEvents();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    LOGOS_ASSERT_EQ(settledRlnState(*impl), std::string("Failed"));
    const int afterBringUp = awaitRlnStateEvents(2).transitions; // Initializing, Failed

    LOGOS_ASSERT_TRUE(impl->stop().success);
    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT_TRUE(impl->rlnBridgeEnable().success == false);
    LOGOS_ASSERT_EQ(impl->rlnState().value.value("state", ""), std::string("Failed"));

    delete impl; // joins every thread of the module, so no transition is still on its way
    LOGOS_ASSERT_EQ(delivery_test_events::lastRlnState().transitions, afterBringUp);
    delivery_test_rln::resetRlnMockState();
}

// The stop after a failed start and an explicit stop() each stop the node;
// destruction waits for the failed start's stop.
LOGOS_TEST(a_failed_start_and_an_explicit_stop_each_stop_the_node) {
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

// logos.test ships relay RLN, logos.dev relay and Mix RLN; "" stays off.
LOGOS_TEST(builtin_presets_enable_rln_on_logos_test_and_logos_dev) {
    ::unsetenv(kRlnPresetsEnvVar);
    std::string error;

    const RlnPresetEntry test = resolveRlnPreset("logos.test", error);
    LOGOS_ASSERT_TRUE(error.empty());
    LOGOS_ASSERT_TRUE(test.enabled);
    LOGOS_ASSERT_FALSE(test.enableValidation);
    LOGOS_ASSERT_EQ(test.registryId,
                    std::string("logos:testnet:"
                                "fd322d32a46ddf57eece9db8d68e9b722262d1f770760fca63b8cb9e7775b971"));
    LOGOS_ASSERT_EQ(test.rlnIdentifier, std::string(kLogosDeliveryRlnIdentifier));
    LOGOS_ASSERT_EQ(test.epochSizeSec, static_cast<uint64_t>(600));
    LOGOS_ASSERT_EQ(test.maxEpochGap, static_cast<uint64_t>(0));

    const RlnPresetEntry dev = resolveRlnPreset("logos.dev", error);
    LOGOS_ASSERT_TRUE(error.empty());
    LOGOS_ASSERT_TRUE(dev.enabled);
    LOGOS_ASSERT_TRUE(dev.hasMix());
    LOGOS_ASSERT_EQ(dev.mixRlnIdentifier, std::string(kMixRlnIdentifierV1));
    LOGOS_ASSERT_FALSE(resolveRlnPreset("", error).enabled);
    LOGOS_ASSERT_TRUE(error.empty());
}

// Naming logos.test with no presets file turns RLN on from the built-in table.
LOGOS_TEST(builtin_logos_test_preset_installs_the_rln_plugin) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    ::unsetenv(kRlnPresetsEnvVar);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO","preset":"logos.test"})").success);
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_callbacksSet);
    auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
    LOGOS_ASSERT_TRUE(libCfg.value("rln-disable-validation", false));
    // No framework context in a unit test, so the bridge cannot come up.
    LOGOS_ASSERT_EQ(settledRlnState(impl), std::string("Failed"));
}

// logos.dev runs relay RLN and, Mix being on by default there, Mix RLN.
LOGOS_TEST(builtin_logos_dev_preset_runs_relay_and_mix_rln) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    ::unsetenv(kRlnPresetsEnvVar);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO","preset":"logos.dev"})").success);
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_callbacksSet);
    auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
    LOGOS_ASSERT_TRUE(libCfg.contains("mix-rln-registry-id"));
    LOGOS_ASSERT_EQ(settledRlnState(impl), std::string("Failed"));
}

// The empty preset carries no RLN, so naming it must not turn it on.
LOGOS_TEST(builtin_empty_preset_leaves_rln_off) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    ::unsetenv(kRlnPresetsEnvVar);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO","preset":""})").success);
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

// A deployment with native Mix: relay on one registry, mix on another.
static constexpr const char* kRlnPresetTableWithMix = R"({
  "logos.test": {
    "enabled": true,
    "registry-id": "reg",
    "rln-identifier": "rln-id",
    "epoch-size-sec": 600,
    "rate-limit": 100,
    "mix-registry-id": "mixreg",
    "mix-rate-limit": 20,
    "mix-metadata-topic": "/mix/1/metadata/proto"
  }
})";

// The mix scope the library needs beyond the registry defaults to native Mix's
// own.
LOGOS_TEST(preset_table_parses_the_mix_registry_with_defaults) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTableWithMix, table).empty());
    const RlnPresetEntry& e = table["logos.test"];
    LOGOS_ASSERT_TRUE(e.hasMix());
    LOGOS_ASSERT_EQ(e.rates.core.rateLimit, static_cast<uint64_t>(100));
    LOGOS_ASSERT_EQ(e.mixRegistryId, std::string("mixreg"));
    LOGOS_ASSERT_EQ(e.mixRlnIdentifier, std::string(kMixRlnIdentifierV1));
    LOGOS_ASSERT_EQ(e.rates.core.mixRateLimit, static_cast<uint64_t>(20));
    LOGOS_ASSERT_EQ(e.mixMetadataTopic, std::string("/mix/1/metadata/proto"));

    // The identifier is taken as written.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,
            "mix-registry-id":"mixreg","mix-metadata-topic":"/t",
            "mix-rln-identifier":")" + std::string(64, 'a') + R"("}})",
        table).empty());
    LOGOS_ASSERT_EQ(table["logos.test"].mixRlnIdentifier, std::string(64, 'a'));

    // Without a mix registry nothing mix-related is in force.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTable, table).empty());
    LOGOS_ASSERT_FALSE(table["logos.test"].hasMix());
    // A preset that sets no rate runs at the default ones.
    LOGOS_ASSERT_EQ(table["logos.test"].rates.core.rateLimit, static_cast<uint64_t>(600));
}

// A mix registry that cannot work is refused where it is written.
LOGOS_TEST(preset_table_refuses_an_unusable_mix_entry) {
    std::map<std::string, RlnPresetEntry> table;
    // The library needs the topic the proof metadata is published on.
    LOGOS_ASSERT_FALSE(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,
            "mix-registry-id":"mixreg"}})",
        table).empty());
    // The identifier is 32 bytes of hex to the library.
    LOGOS_ASSERT_FALSE(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,
            "mix-registry-id":"mixreg","mix-metadata-topic":"/t","mix-rln-identifier":"zz"}})",
        table).empty());
    // Relay and Mix prove on two registries, whatever the relay one runs at.
    for (const char* relay : {R"("epoch-size-sec":600)", R"("epoch-size-sec":10,"max-epoch-gap":3)"}) {
        LOGOS_ASSERT_EQ(parseRlnPresetTable(
                            std::string(R"({"logos.test":{"enabled":true,"registry-id":"reg",)") + relay
                                + R"(,"mix-registry-id":"reg","mix-metadata-topic":"/t"}})",
                            table),
                        std::string("preset \"logos.test\" mix-registry-id must name another registry"
                                    " than registry-id"));
    }
}

// Ids that differ only in letter case are not safely two registries: the
// entry is refused like one that names the same registry twice.
LOGOS_TEST(preset_table_refuses_registry_ids_that_differ_only_in_case) {
    std::map<std::string, RlnPresetEntry> table;
    for (const char* keys : {
             R"("registry-id":"logos:testnet:ab","epoch-size-sec":600,"mix-registry-id":"logos:testnet:AB")",
             R"("registry-id":"Reg","epoch-size-sec":10,"max-epoch-gap":3,"mix-registry-id":"reg")",
         }) {
        const std::string err = parseRlnPresetTable(
            std::string(R"({"logos.test":{"enabled":true,"mix-metadata-topic":"/t",)") + keys + "}}", table);
        LOGOS_ASSERT_EQ(err, std::string("preset \"logos.test\" mix-registry-id must name another registry"
                                         " than registry-id"));
    }
}

// A key of the wrong JSON type is an error naming the preset and the key,
// never its default: "enabled": "true" would turn RLN off.
LOGOS_TEST(preset_table_refuses_a_key_of_the_wrong_type) {
    std::map<std::string, RlnPresetEntry> table;
    struct Typed { const char* key; const char* bad; const char* expected; };
    for (const Typed& k : {
             Typed{"enabled", R"("true")", "a boolean"},
             Typed{"enable-validation", "0", "a boolean"},
             Typed{"manage-backend", R"("false")", "a boolean"},
             Typed{"registry-id", "7", "a string"},
             Typed{"rln-identifier", "true", "a string"},
             Typed{"mix-registry-id", "[]", "a string"},
             Typed{"mix-rln-identifier", "1", "a string"},
             Typed{"mix-metadata-topic", "null", "a string"},
             Typed{"epoch-size-sec", R"("600")", "an unsigned integer"},
             Typed{"max-epoch-gap", "-1", "an unsigned integer"},
             Typed{"rate-limit", "1.5", "an unsigned integer"},
             Typed{"mix-rate-limit", "-20", "an unsigned integer"},
         }) {
        const std::string err = parseRlnPresetTable(
            std::string(R"({"logos.test":{")") + k.key + "\":" + k.bad + "}}", table);
        LOGOS_ASSERT_EQ(err, std::string("preset \"logos.test\" ") + k.key + " must be " + k.expected);
    }

    // The first one found is reported even beside keys that would pass.
    const std::string err = parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":"600"}})", table);
    LOGOS_ASSERT_EQ(err, std::string("preset \"logos.test\" epoch-size-sec must be an unsigned integer"));
}

// A deployment whose rates differ by mode: Edge nodes run at the entry's
// rates, Core nodes at higher ones.
static constexpr const char* kRlnPresetTableWithModes = R"({
  "logos.test": {
    "enabled": true,
    "registry-id": "reg",
    "epoch-size-sec": 600,
    "rate-limit": 100,
    "mix-registry-id": "mixreg",
    "mix-rate-limit": 20,
    "mix-metadata-topic": "/mix/1/metadata/proto",
    "modes": {
      "edge": {},
      "Core": { "rate-limit": 600, "mix-rate-limit": 100 }
    }
  }
})";

// A mode's keys replace the entry's rates for nodes of that mode, a key it
// leaves out is the entry's; the mode name is matched without regard to case.
LOGOS_TEST(preset_table_parses_modes_over_the_entry) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_EQ(parseRlnPresetTable(kRlnPresetTableWithModes, table), std::string());
    {
        const RlnPresetEntry& entry = table["logos.test"];
        LOGOS_ASSERT_TRUE(entry.enabled);
        LOGOS_ASSERT_EQ(entry.registryId, std::string("reg"));
        LOGOS_ASSERT_EQ(entry.mixRegistryId, std::string("mixreg"));
        LOGOS_ASSERT_EQ(entry.mixMetadataTopic, std::string("/mix/1/metadata/proto"));
        LOGOS_ASSERT_EQ(entry.epochSizeSec, static_cast<uint64_t>(600));

        // An empty mode: the entry's rates.
        const RlnRates& edge = entry.rates.of(RlnMode::Edge);
        LOGOS_ASSERT_EQ(edge.rateLimit, static_cast<uint64_t>(100));
        LOGOS_ASSERT_EQ(edge.mixRateLimit, static_cast<uint64_t>(20));

        const RlnRates& core = entry.rates.of(RlnMode::Core);
        LOGOS_ASSERT_EQ(core.rateLimit, static_cast<uint64_t>(600));
        LOGOS_ASSERT_EQ(core.mixRateLimit, static_cast<uint64_t>(100));
    }

    // One mode named: the other runs at the entry's rates, and a rate the
    // entry leaves out at that mode's default.
    LOGOS_ASSERT_EQ(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,"rate-limit":100,
            "mix-registry-id":"mixreg","mix-metadata-topic":"/t",
            "modes":{"EDGE":{"mix-rate-limit":20}}}})",
        table), std::string());
    {
        const RlnModeRates& rates = table["logos.test"].rates;
        LOGOS_ASSERT_EQ(rates.edge.mixRateLimit, static_cast<uint64_t>(20));
        LOGOS_ASSERT_EQ(rates.edge.rateLimit, static_cast<uint64_t>(100));
        LOGOS_ASSERT_EQ(rates.core.mixRateLimit, static_cast<uint64_t>(100));
        LOGOS_ASSERT_EQ(rates.core.rateLimit, static_cast<uint64_t>(100));
    }

    // A mode's 0 replaces the entry's rate too: the RLN module's default.
    LOGOS_ASSERT_EQ(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,"rate-limit":100,
            "modes":{"edge":{"rate-limit":0}}}})",
        table), std::string());
    LOGOS_ASSERT_EQ(table["logos.test"].rates.edge.rateLimit, static_cast<uint64_t>(0));
    LOGOS_ASSERT_EQ(table["logos.test"].rates.core.rateLimit, static_cast<uint64_t>(100));

    // Without `modes` both modes run at the entry's rates.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTableWithMix, table).empty());
    {
        const RlnPresetEntry& plain = table["logos.test"];
        LOGOS_ASSERT_EQ(plain.rates.edge.rateLimit, static_cast<uint64_t>(100));
        LOGOS_ASSERT_EQ(plain.rates.edge.mixRateLimit, static_cast<uint64_t>(20));
        LOGOS_ASSERT_EQ(rlnBackendStartConfig(plain, RlnMode::Edge),
                        rlnBackendStartConfig(plain, RlnMode::Core));
    }
    std::string err;
    ::unsetenv(kRlnPresetsEnvVar);
    const RlnPresetEntry builtin = resolveRlnPreset("logos.test", err);
    LOGOS_ASSERT_TRUE(err.empty());
    LOGOS_ASSERT_EQ(rlnBackendStartConfig(builtin, RlnMode::Edge),
                    rlnBackendStartConfig(builtin, RlnMode::Core));
    LOGOS_ASSERT_EQ(std::string(rlnModeName(RlnMode::Core)), std::string("Core"));
    LOGOS_ASSERT_EQ(std::string(rlnModeName(RlnMode::Edge)), std::string("Edge"));
}

// The built-in presets run at the default rates: relay 600 for both modes,
// Mix 100 for Core and 20 for Edge. A preset file that sets no rate gets the
// same; one that sets a rate overrides it, and 0 leaves it to the RLN module.
LOGOS_TEST(presets_default_to_the_logos_rates) {
    std::string err;
    ::unsetenv(kRlnPresetsEnvVar);
    const RlnPresetEntry dev = resolveRlnPreset("logos.dev", err);
    LOGOS_ASSERT_TRUE(err.empty());
    auto core = nlohmann::json::parse(rlnBackendStartConfig(dev, RlnMode::Core));
    LOGOS_ASSERT_EQ(core["registries"][0].value("rate_limit", 0), 600);
    LOGOS_ASSERT_EQ(core["registries"][1].value("rate_limit", 0), 100);
    auto edge = nlohmann::json::parse(rlnBackendStartConfig(dev, RlnMode::Edge));
    LOGOS_ASSERT_EQ(edge["registries"][0].value("rate_limit", 0), 600);
    LOGOS_ASSERT_EQ(edge["registries"][1].value("rate_limit", 0), 20);

    const RlnPresetEntry test = resolveRlnPreset("logos.test", err);
    LOGOS_ASSERT_TRUE(err.empty());
    auto testCore = nlohmann::json::parse(rlnBackendStartConfig(test, RlnMode::Core));
    LOGOS_ASSERT_EQ(testCore["registries"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(testCore["registries"][0].value("rate_limit", 0), 600);

    const std::string withMix =
        R"("enabled":true,"registry-id":"reg","epoch-size-sec":600,)"
        R"("mix-registry-id":"mixreg","mix-metadata-topic":"/t")";
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_EQ(parseRlnPresetTable(R"({"logos.dev":{)" + withMix + "}}", table),
                    std::string());
    {
        const RlnModeRates& rates = table["logos.dev"].rates;
        LOGOS_ASSERT_EQ(rates.core.rateLimit, static_cast<uint64_t>(600));
        LOGOS_ASSERT_EQ(rates.core.mixRateLimit, static_cast<uint64_t>(100));
        LOGOS_ASSERT_EQ(rates.edge.rateLimit, static_cast<uint64_t>(600));
        LOGOS_ASSERT_EQ(rates.edge.mixRateLimit, static_cast<uint64_t>(20));
    }

    // A top-level rate sets both modes; a mode's key then overrides one.
    LOGOS_ASSERT_EQ(parseRlnPresetTable(R"({"logos.dev":{)" + withMix
                                            + R"(,"rate-limit":300,"modes":{"edge":{"mix-rate-limit":5}}}})",
                                        table),
                    std::string());
    {
        const RlnModeRates& rates = table["logos.dev"].rates;
        LOGOS_ASSERT_EQ(rates.core.rateLimit, static_cast<uint64_t>(300));
        LOGOS_ASSERT_EQ(rates.edge.rateLimit, static_cast<uint64_t>(300));
        LOGOS_ASSERT_EQ(rates.core.mixRateLimit, static_cast<uint64_t>(100));
        LOGOS_ASSERT_EQ(rates.edge.mixRateLimit, static_cast<uint64_t>(5));
    }

    // 0 leaves the rate to the RLN module: the start config carries none.
    LOGOS_ASSERT_EQ(parseRlnPresetTable(R"({"logos.dev":{)" + withMix
                                            + R"(,"rate-limit":0,"mix-rate-limit":0}})",
                                        table),
                    std::string());
    auto none = nlohmann::json::parse(rlnBackendStartConfig(table["logos.dev"], RlnMode::Edge));
    LOGOS_ASSERT_FALSE(none["registries"][0].contains("rate_limit"));
    LOGOS_ASSERT_FALSE(none["registries"][1].contains("rate_limit"));
}

// What `modes` cannot say is refused where it is written, naming the mode.
LOGOS_TEST(preset_table_refuses_unusable_modes) {
    std::map<std::string, RlnPresetEntry> table;
    const std::string relayOnly = R"("enabled":true,"registry-id":"reg","epoch-size-sec":600)";
    const std::string withMix = relayOnly + R"(,"mix-registry-id":"mixreg","mix-metadata-topic":"/t")";
    struct Refused { std::string keys; std::string error; };
    for (const Refused& r : {
             Refused{withMix + R"(,"modes":[])", "modes must be an object"},
             Refused{withMix + R"(,"modes":{"light":{}})", "modes: unknown mode \"light\""},
             Refused{withMix + R"(,"modes":{"edge":{},"Edge":{}})", "modes: mode edge is written twice"},
             Refused{withMix + R"(,"modes":{"edge":7})", "mode edge must be an object"},
             // Only the rates are a mode's.
             Refused{withMix + R"(,"modes":{"edge":{"epoch-size-sec":10}})",
                     "mode edge: epoch-size-sec cannot be set in a mode"},
             Refused{withMix + R"(,"modes":{"edge":{"relay-membership":false}})",
                     "mode edge: relay-membership cannot be set in a mode"},
             // Typed like the entry's own keys.
             Refused{relayOnly + R"(,"modes":{"core":{"rate-limit":"many"}})",
                     "mode core: rate-limit must be an unsigned integer"},
         }) {
        const std::string err = parseRlnPresetTable(R"({"logos.test":{)" + r.keys + "}}", table);
        LOGOS_ASSERT_EQ(err, "preset \"logos.test\" " + r.error);
    }
}

// The node's mode is the config's top-level `mode`, read as the library reads
// it; whatever the library would refuse or ignore is Core here.
LOGOS_TEST(configMode_reads_the_mode_as_the_library_does) {
    const auto mode = [](const char* cfg) { return configMode(nlohmann::json::parse(cfg)); };
    LOGOS_ASSERT_TRUE(mode(R"({"logLevel":"INFO"})") == RlnMode::Core);
    LOGOS_ASSERT_TRUE(mode(R"({"mode":"Core"})") == RlnMode::Core);
    LOGOS_ASSERT_TRUE(mode(R"({"mode":"CORE"})") == RlnMode::Core);
    LOGOS_ASSERT_TRUE(mode(R"({"mode":"Edge"})") == RlnMode::Edge);
    LOGOS_ASSERT_TRUE(mode(R"({"mode":" edge "})") == RlnMode::Edge);
    LOGOS_ASSERT_TRUE(mode(R"({"MODE":"EDGE","preset":"logos.test"})") == RlnMode::Edge);
    // The kernel entry reads no mode.
    LOGOS_ASSERT_TRUE(mode(R"({"entryLayer":"kernel","mode":"Edge","kernelConf":{}})") == RlnMode::Core);
    // The library strips and lower-cases entryLayer.
    LOGOS_ASSERT_TRUE(mode(R"({"EntryLayer":"\tKERNEL\n","mode":"Edge","kernelConf":{}})") == RlnMode::Core);
    LOGOS_ASSERT_TRUE(mode(R"({"entryLayer":"messaging","mode":"Edge"})") == RlnMode::Edge);
    // Not a string, not a mode, not at the top level, not an object.
    LOGOS_ASSERT_TRUE(mode(R"({"mode":1})") == RlnMode::Core);
    LOGOS_ASSERT_TRUE(mode(R"({"mode":"light"})") == RlnMode::Core);
    LOGOS_ASSERT_TRUE(mode(R"({"mode":"   "})") == RlnMode::Core);
    LOGOS_ASSERT_TRUE(mode(R"({"messagingOverrides":{"mode":"Edge"}})") == RlnMode::Core);
    LOGOS_ASSERT_TRUE(mode(R"([])") == RlnMode::Core);
}

// One start() names both registries, each as an entry with its own epoch,
// gap and rate: a top-level gap or rate would be every entry's default.
// Pinned as JSON because the RLN module parses exactly this shape.
LOGOS_TEST(the_backend_start_config_names_both_registries) {
    RlnPresetEntry cfg;
    cfg.enabled = true;
    cfg.registryId = "reg";
    cfg.epochSizeSec = 600;
    cfg.mixRegistryId = "mixreg";
    cfg.rates.core = RlnRates{.rateLimit = 100, .mixRateLimit = 20};

    auto start = nlohmann::json::parse(rlnBackendStartConfig(cfg, RlnMode::Core));
    LOGOS_ASSERT_EQ(start.value("epoch_size_sec", 0), 600);
    LOGOS_ASSERT_FALSE(start.contains("rate_limit"));
    LOGOS_ASSERT_FALSE(start.contains("max_epoch_gap"));
    LOGOS_ASSERT_EQ(start["registries"].size(), static_cast<size_t>(2));
    const nlohmann::json expectedRelay{
        {"registry_id", "reg"}, {"epoch_size_sec", 600}, {"rate_limit", 100}};
    LOGOS_ASSERT_TRUE(start["registries"][0] == expectedRelay);
    const nlohmann::json expectedMix{
        {"registry_id", "mixreg"}, {"epoch_size_sec", 10}, {"max_epoch_gap", 3}, {"rate_limit", 20}};
    LOGOS_ASSERT_TRUE(start["registries"][1] == expectedMix);

    // A relay rate with no mix rate: the mix entry carries none, and nothing
    // at the top level gives it the relay's.
    cfg.rates.core = RlnRates{.rateLimit = 600, .mixRateLimit = 0};
    auto relayRateOnly = nlohmann::json::parse(rlnBackendStartConfig(cfg, RlnMode::Core));
    LOGOS_ASSERT_FALSE(relayRateOnly.contains("rate_limit"));
    LOGOS_ASSERT_EQ(relayRateOnly["registries"][0].value("rate_limit", 0), 600);
    LOGOS_ASSERT_FALSE(relayRateOnly["registries"][1].contains("rate_limit"));

    // The rates are the mode's: Edge, which has none here, asks none.
    auto edge = nlohmann::json::parse(rlnBackendStartConfig(cfg, RlnMode::Edge));
    LOGOS_ASSERT_FALSE(edge["registries"][0].contains("rate_limit"));
    LOGOS_ASSERT_FALSE(edge["registries"][1].contains("rate_limit"));

    // Relay only, with a gap and no rate.
    RlnPresetEntry relay;
    relay.enabled = true;
    relay.registryId = "reg";
    relay.epochSizeSec = 600;
    relay.maxEpochGap = 2;
    auto only = nlohmann::json::parse(rlnBackendStartConfig(relay, RlnMode::Core));
    LOGOS_ASSERT_FALSE(only.contains("max_epoch_gap"));
    LOGOS_ASSERT_EQ(only["registries"].size(), static_cast<size_t>(1));
    const nlohmann::json expectedOnly{
        {"registry_id", "reg"}, {"epoch_size_sec", 600}, {"max_epoch_gap", 2}};
    LOGOS_ASSERT_TRUE(only["registries"][0] == expectedOnly);
}

// A config that turns mix on inherits the deployment's mix registry, scope and
// metadata topic from the preset, so a host does not copy them.
LOGOS_TEST(a_mix_preset_supplies_the_mix_keys_to_a_mix_node) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTableWithMix);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO","preset":"logos.test","mix":true})").success);
    auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
    LOGOS_ASSERT_EQ(libCfg.value("mix-rln-registry-id", ""), std::string("mixreg"));
    LOGOS_ASSERT_EQ(libCfg.value("mix-rln-identifier-hex", ""), std::string(kMixRlnIdentifierV1));
    LOGOS_ASSERT_EQ(libCfg.value("mix-rln-metadata-topic", ""), std::string("/mix/1/metadata/proto"));

    // The resolved deployment names the mix registry too.
    const auto state = impl.rlnState().value;
    LOGOS_ASSERT_EQ(state.value("registryId", ""), std::string("reg"));
    LOGOS_ASSERT_EQ(state.value("mixRegistryId", ""), std::string("mixreg"));
    LOGOS_ASSERT_EQ(state.value("mixRlnIdentifier", ""), std::string(kMixRlnIdentifierV1));
    LOGOS_ASSERT_EQ(state.value("mixEpochSizeSec", 0), 10);
    LOGOS_ASSERT_EQ(state.value("mixMaxEpochGap", 0), 3);
    (void)settledRlnState(impl);
}

LOGOS_TEST(a_caller_supplied_mix_key_wins_and_a_relay_only_node_gets_none) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTableWithMix);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"logLevel":"INFO","preset":"logos.test","mix":true,"mix-rln-metadata-topic":"/mine",
                "mix-rln-identifier-hex":"00"})")
            .success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_EQ(libCfg.value("mix-rln-metadata-topic", ""), std::string("/mine"));
        LOGOS_ASSERT_EQ(libCfg.value("mix-rln-identifier-hex", ""), std::string("00"));
        LOGOS_ASSERT_EQ(libCfg.value("mix-rln-registry-id", ""), std::string("mixreg"));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        // The library's other spelling of a key is the caller's key too: no
        // dashed one is added beside it.
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"logLevel":"INFO","preset":"logos.test","mix":true,"mixRlnRegistryId":"mixreg",
                "mixRlnMetadataTopic":"/mine","mixRlnIdentifierHex":"00"})")
            .success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_EQ(libCfg.value("mixRlnRegistryId", ""), std::string("mixreg"));
        LOGOS_ASSERT_EQ(libCfg.value("mixRlnMetadataTopic", ""), std::string("/mine"));
        LOGOS_ASSERT_EQ(libCfg.value("mixRlnIdentifierHex", ""), std::string("00"));
        LOGOS_ASSERT_FALSE(libCfg.contains("mix-rln-registry-id"));
        LOGOS_ASSERT_FALSE(libCfg.contains("mix-rln-metadata-topic"));
        LOGOS_ASSERT_FALSE(libCfg.contains("mix-rln-identifier-hex"));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        // mix off: the library is told nothing about mix, and the node takes
        // no mix membership.
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(
            impl.createNode(R"({"logLevel":"INFO","preset":"logos.test","mix":false})").success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_FALSE(libCfg.contains("mix-rln-registry-id"));
        LOGOS_ASSERT_FALSE(libCfg.contains("mix-rln-identifier-hex"));
        LOGOS_ASSERT_FALSE(impl.rlnState().value.contains("mixRegistryId"));
        (void)settledRlnState(impl);
    }
}

// The backend is started for the preset's registries only, so a mix registry
// the caller names must be one of them: createNode fails before any node is
// created, with the reason.
LOGOS_TEST(a_caller_supplied_mix_registry_the_preset_does_not_serve_is_refused) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    struct Refused { const char* table; const char* cfg; const char* needle; };
    for (const Refused& r : {
             // Not the preset's mix registry: another one, a number.
             Refused{kRlnPresetTableWithMix,
                     R"({"preset":"logos.test","mix":true,"mix-rln-registry-id":"other"})",
                     "is not the preset's mix registry mixreg"},
             Refused{kRlnPresetTableWithMix,
                     R"({"preset":"logos.test","messagingOverrides":{"mix":true,"mix-rln-registry-id":7}})",
                     "is not the preset's mix registry mixreg"},
             // Checked wherever the library reads it: in messagingOverrides
             // beside a kernelConf, in kernelConf, at the top level.
             Refused{kRlnPresetTableWithMix,
                     R"({"preset":"logos.test","kernelConf":{"mix":true},"messagingOverrides":{"mix-rln-registry-id":"other"}})",
                     "is not the preset's mix registry mixreg"},
             Refused{kRlnPresetTableWithMix,
                     R"({"preset":"logos.test","kernelConf":{"mix-rln-registry-id":"other"},"messagingOverrides":{"mix-rln-registry-id":"mixreg"}})",
                     "is not the preset's mix registry mixreg"},
             // Without `mix: true` beside it: anonymityLevel mounts mix too.
             Refused{kRlnPresetTableWithMix,
                     R"({"preset":"logos.test","mix-rln-registry-id":"other"})",
                     "is not the preset's mix registry mixreg"},
             // The other spelling the library's parser takes, and both at once.
             Refused{kRlnPresetTableWithMix,
                     R"({"preset":"logos.test","mix":true,"mixRlnRegistryId":"other"})",
                     "is not the preset's mix registry mixreg"},
             Refused{kRlnPresetTableWithMix,
                     R"({"preset":"logos.test","mix":true,"mix-rln-registry-id":"mixreg","mixRlnRegistryId":"other"})",
                     "is not the preset's mix registry mixreg"},
             // No mix registry in the preset: nothing serves Mix RLN, also
             // when the relay registry runs at Mix's epoch size and gap.
             Refused{kRlnPresetTable,
                     R"({"preset":"logos.test","mix":true,"mix-rln-registry-id":"reg"})",
                     "the preset names no mix registry"},
             Refused{R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":10,"max-epoch-gap":3}})",
                     R"({"preset":"logos.test","mix":true,"mix-rln-registry-id":"reg"})",
                     "the preset names no mix registry"},
         }) {
        RlnPresetsFile presets(r.table);
        DeliveryModuleImpl impl;
        const auto result = impl.createNode(r.cfg);
        LOGOS_ASSERT_FALSE(result.success);
        LOGOS_ASSERT_TRUE(result.error.find("mix-rln-registry-id") != std::string::npos);
        LOGOS_ASSERT_TRUE(result.error.find(r.needle) != std::string::npos);
    }
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_ctx_create"));
}

// A host that manages the backend names the registries in its own start, so
// the preset is no measure of the caller's mix registry.
LOGOS_TEST(a_caller_supplied_mix_registry_is_not_checked_on_a_host_managed_backend) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    RlnPresetsFile presets(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,"manage-backend":false}})");
    DeliveryModuleImpl impl;
    const auto result = impl.createNode(
        R"({"preset":"logos.test","mix":true,"mix-rln-registry-id":"other","mix-rln-metadata-topic":"/t"})");
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_TRUE(t.cFunctionCalled("logosdelivery_ctx_create"));
}

// The preset's own mix registry, written out by the caller, passes; with RLN
// off the key is not looked at.
LOGOS_TEST(a_caller_supplied_mix_registry_that_is_the_presets_passes) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    delivery_test_rln::resetRlnMockState();
    {
        RlnPresetsFile presets(kRlnPresetTableWithMix);
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"preset":"logos.test","messagingOverrides":{"anonymityLevel":"Required","mix-rln-registry-id":"mixreg"}})")
            .success);
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        // The empty preset runs no RLN: nothing to check the key against.
        ::unsetenv(kRlnPresetsEnvVar);
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"logLevel":"INFO","preset":"","mix":true,"mix-rln-registry-id":"other"})")
            .success);
    }
}

// createNode resolves the preset for the config's mode: the node's RLN state
// names the mode and the rates it asks at.
LOGOS_TEST(createNode_resolves_the_preset_for_the_config_mode) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTableWithModes);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(R"({"mode":"Edge","preset":"logos.test"})").success);
        const auto state = impl.rlnState().value;
        LOGOS_ASSERT_EQ(state.value("mode", ""), std::string("Edge"));
        LOGOS_ASSERT_EQ(state.value("rateLimit", 0), 100);
        LOGOS_ASSERT_EQ(state.value("mixRateLimit", 0), 20);
        LOGOS_ASSERT_EQ(state.value("registryId", ""), std::string("reg"));
        LOGOS_ASSERT_EQ(state.value("mixRegistryId", ""), std::string("mixreg"));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(R"({"mode":"Core","preset":"logos.test"})").success);
        const auto state = impl.rlnState().value;
        LOGOS_ASSERT_EQ(state.value("mode", ""), std::string("Core"));
        LOGOS_ASSERT_EQ(state.value("rateLimit", 0), 600);
        LOGOS_ASSERT_EQ(state.value("mixRateLimit", 0), 100);
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
}

// A preset that sets no rate reports the default ones, a rate it leaves to
// the RLN module (0) is not reported, and a preset without modes still names
// the node's mode.
LOGOS_TEST(rlnState_reports_the_mode_and_the_rates_asked) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    {
        RlnPresetsFile presets(kRlnPresetTable);
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(R"({"mode":"edge","preset":"logos.test"})").success);
        const auto state = impl.rlnState().value;
        LOGOS_ASSERT_EQ(state.value("mode", ""), std::string("Edge"));
        LOGOS_ASSERT_EQ(state.value("rateLimit", 0), 600);
        LOGOS_ASSERT_FALSE(state.contains("mixRateLimit"));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        RlnPresetsFile presets(
            R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,"rate-limit":0}})");
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(R"({"mode":"edge","preset":"logos.test"})").success);
        LOGOS_ASSERT_FALSE(impl.rlnState().value.contains("rateLimit"));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        // No RLN: nothing but the state.
        ::unsetenv(kRlnPresetsEnvVar);
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(R"({"mode":"Edge","preset":""})").success);
        LOGOS_ASSERT_FALSE(impl.rlnState().value.contains("mode"));
    }
}

// An anonymity level above None mounts Mix as `mix: true` does, so it gets
// the preset's mix keys too: at the top level of a flat config, in
// messagingOverrides for a layered one, which is where that shape carries the
// level. Without a level the network preset decides (logos.test runs Mix).
// The kernel entry reads no level: there `mix` in kernelConf, else the preset,
// turns the keys on.
LOGOS_TEST(a_mix_preset_supplies_the_mix_keys_for_an_anonymity_level) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTableWithMix);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    const auto hasMixKeys = [](const nlohmann::json& obj) {
        return obj.value("mix-rln-registry-id", "") == std::string("mixreg")
            && obj.value("mix-rln-identifier-hex", "") == std::string(kMixRlnIdentifierV1)
            && obj.value("mix-rln-metadata-topic", "") == std::string("/mix/1/metadata/proto");
    };
    const auto hasNoMixKeys = [](const nlohmann::json& obj) {
        return !obj.contains("mix-rln-registry-id") && !obj.contains("mix-rln-identifier-hex")
            && !obj.contains("mix-rln-metadata-topic");
    };

    // Flat: the level and the keys are top-level.
    for (const char* cfg : {
             R"({"logLevel":"INFO","preset":"logos.test","anonymityLevel":"Preferred"})",
             R"({"logLevel":"INFO","preset":"logos.test","anonymityLevel":"Required"})",
         }) {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(cfg).success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_TRUE(hasMixKeys(libCfg));
        LOGOS_ASSERT_FALSE(libCfg.contains("messagingOverrides"));
        (void)settledRlnState(impl);
        delivery_test_rln::resetRlnMockState();
    }
    for (const char* cfg : {
             R"({"logLevel":"INFO","preset":"logos.test","anonymityLevel":"None"})",
             R"({"logLevel":"INFO","preset":"logos.test","mix":false})",
         }) {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(cfg).success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_TRUE(hasNoMixKeys(libCfg));
        (void)settledRlnState(impl);
        delivery_test_rln::resetRlnMockState();
    }

    // Layered: both live in messagingOverrides.
    for (const char* cfg : {
             R"({"mode":"Edge","preset":"logos.test","messagingOverrides":{"anonymityLevel":"Preferred"}})",
             R"({"preset":"logos.test","messagingOverrides":{"anonymity-level":"required"}})",
             // No level: logos.test runs Mix, as the library defaults it.
             R"({"mode":"Edge","preset":"logos.test","messagingOverrides":{}})",
             R"({"mode":"Edge","preset":"logos.test"})",
         }) {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(cfg).success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_TRUE(hasNoMixKeys(libCfg));
        LOGOS_ASSERT_TRUE(libCfg.contains("messagingOverrides"));
        LOGOS_ASSERT_TRUE(hasMixKeys(libCfg["messagingOverrides"]));
        (void)settledRlnState(impl);
        delivery_test_rln::resetRlnMockState();
    }
    for (const char* cfg : {
             R"({"mode":"Edge","preset":"logos.test","messagingOverrides":{"anonymityLevel":"None"}})",
             R"({"mode":"Edge","preset":"logos.test","messagingOverrides":{"mix":false}})",
         }) {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(cfg).success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_TRUE(hasNoMixKeys(libCfg));
        LOGOS_ASSERT_TRUE(libCfg.contains("messagingOverrides"));
        LOGOS_ASSERT_TRUE(hasNoMixKeys(libCfg["messagingOverrides"]));
        (void)settledRlnState(impl);
        delivery_test_rln::resetRlnMockState();
    }

    // Kernel entry: the keys go to kernelConf, for `mix: true` there.
    {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"entryLayer":"kernel","preset":"logos.test","kernelConf":{"logLevel":"INFO","mix":true}})")
            .success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_TRUE(hasNoMixKeys(libCfg));
        LOGOS_ASSERT_FALSE(libCfg.contains("messagingOverrides"));
        LOGOS_ASSERT_TRUE(hasMixKeys(libCfg["kernelConf"]));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"entryLayer":"kernel","preset":"logos.test","kernelConf":{"logLevel":"INFO","mix":false}})")
            .success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_TRUE(hasNoMixKeys(libCfg["kernelConf"]));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
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

LOGOS_TEST(discovery_from_requirements_tells_a_request_from_an_unreadable_reply) {
    delivery_discovery::PluginRequest req;
    LOGOS_ASSERT_FALSE(delivery_discovery::fromRequirements("not json", nlohmann::json::object(), req).empty());
    LOGOS_ASSERT_FALSE(req.requested);
    LOGOS_ASSERT_FALSE(delivery_discovery::fromRequirements(
        R"({"externalServiceDiscovery":true,"bootstrapNodes":["/ip4/10.0.0.2/tcp/1"]})",
        nlohmann::json::object(), req).empty());
    LOGOS_ASSERT_TRUE(req.requested);
    LOGOS_ASSERT_FALSE(req.enabled);
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

// A reply that cannot be read, or no reply at all, says nothing about a
// request: no plugin, and createNode goes on. A node that did ask for one
// then fails its start in logos-delivery.
LOGOS_TEST(createNode_installs_no_plugin_on_an_unreadable_requirements_reply) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);
    t.mockCFunction("logosdelivery_ctx_get_discovery_requirements").returns("nonsense");

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"preset":"logos.test"})").success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_ctx_set_service_discovery_plugin"));
}

LOGOS_TEST(createNode_installs_no_plugin_when_the_requirements_call_fails) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);
    t.mockCFunction("logosdelivery_ctx_get_discovery_requirements.dispatch").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"preset":"logos.test"})").success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_ctx_get_discovery_requirements"));
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_ctx_set_service_discovery_plugin"));
}

// Asked for, but the node's own data is unusable: fail where the reason is
// still clear.
LOGOS_TEST(createNode_fails_when_a_requested_plugin_cannot_be_set_up) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);
    t.mockCFunction("logosdelivery_ctx_get_discovery_requirements")
        .returns(R"({"externalServiceDiscovery":true,"bootstrapNodes":["/ip4/10.0.0.2/tcp/1"]})");

    DeliveryModuleImpl impl;
    const auto r = impl.createNode(R"({"preset":"logos.test"})");
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_TRUE(r.error.find("discovery") != std::string::npos);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_ctx_set_service_discovery_plugin"));
}

// ---------------------------------------------------------------------------
// RlnBridge, driven directly.
//
// The bridge calls the typed client, whose stub answers from the calling thread with whatever stubError()
// says, so a request op's whole reply path runs inside the test with no host,
// no chain and no waiting. What these pin is the SHAPE the delivery library
// receives — the two error dialects, and the classification that decides
// whether the library retries.
// ---------------------------------------------------------------------------

// A bridge wired to a stub client and enabled, which is all an op entry point
// needs — bringUpRlnBridge's context gate is a DeliveryModuleImpl concern.
struct BridgeFixture {
    RlnModule client{"delivery_module"};
    RlnBridge bridge;

    BridgeFixture() {
        rln_module_stub::resetStubError();
        delivery_test_rln::resetRlnMockState();
        bridge.init(&client);
    }
    ~BridgeFixture() { rln_module_stub::resetStubError(); }
};

LOGOS_TEST(rln_bridge_tstr_transport_failure_answers_the_bare_error_object) {
    auto t = LogosTestContext("delivery_module");
    BridgeFixture f;
    LOGOS_ASSERT_EQ(f.bridge.enable(), std::string());

    f.bridge.getMembershipState(7, "reg", "rln-id");

    LOGOS_ASSERT_TRUE(delivery_test_rln::g_responseFired);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastResponseReqId, static_cast<uint64_t>(7));
    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastResponseJson);
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

    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastResponseReqId, static_cast<uint64_t>(8));
    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastResponseJson);
    // result dialect: success=false, and the error arm is a JSON-ENCODED object.
    LOGOS_ASSERT_FALSE(reply["success"].get<bool>());
    const auto inner = nlohmann::json::parse(reply["error"].get<std::string>());
    LOGOS_ASSERT_EQ(inner["class"].get<std::string>(), std::string("transient"));
}

LOGOS_TEST(rln_bridge_dispatches_registry_parameters) {
    auto t = LogosTestContext("delivery_module");
    BridgeFixture f;
    LOGOS_ASSERT_EQ(f.bridge.enable(), std::string());

    f.bridge.callMix(9, "get_registry_parameters", R"(["reg","rln-id"])");

    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastResponseReqId, static_cast<uint64_t>(9));
    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastResponseJson);
    LOGOS_ASSERT_FALSE(reply["success"].get<bool>());
    const auto inner = nlohmann::json::parse(reply["error"].get<std::string>());
    LOGOS_ASSERT_EQ(inner["class"].get<std::string>(), std::string("transient"));
    LOGOS_ASSERT_EQ(inner["kind"].get<std::string>(),
                    std::string("rln_bridge_transport"));
}

LOGOS_TEST(rln_bridge_provider_refusal_is_permanent) {
    auto t = LogosTestContext("delivery_module");
    BridgeFixture f;
    LOGOS_ASSERT_EQ(f.bridge.enable(), std::string());
    // A refusal is the module declining the call itself — a contract mismatch
    // no retry fixes, unlike every other CallError the bridge sees.
    rln_module_stub::stubError() = logos::CallError{
        "invalid_args", "rate_limit is not a string", "rln_module"};

    f.bridge.generateProof(9, "reg", "rln-id", "deadbeef", 1700000000);

    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastResponseJson);
    const auto inner = nlohmann::json::parse(reply["error"].get<std::string>());
    LOGOS_ASSERT_EQ(inner["class"].get<std::string>(), std::string("permanent"));
    LOGOS_ASSERT_EQ(inner["kind"].get<std::string>(),
                    std::string("rln_bridge_dispatch"));
}

LOGOS_TEST(rln_bridge_without_a_client_still_answers_the_request) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnBridge bridge; // never init()ed, so never enabled

    bridge.validateProof(10, "reg", "rln-id", "deadbeef", 1700000000, "{}");

    // The library is owed an answer for every reqId it raises, including the
    // ones this bridge cannot serve — an unserved request only goes quiet and
    // expires against the library's own budget.
    LOGOS_ASSERT_TRUE(delivery_test_rln::g_responseFired);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastResponseReqId, static_cast<uint64_t>(10));
    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastResponseJson);
    const auto inner = nlohmann::json::parse(reply["error"].get<std::string>());
    // Permanent: with no client nothing will enable this bridge, so a retry
    // finds the same.
    LOGOS_ASSERT_EQ(inner["class"].get<std::string>(), std::string("permanent"));
    LOGOS_ASSERT_EQ(inner["kind"].get<std::string>(),
                    std::string("rln_bridge_disabled"));
}

LOGOS_TEST(rln_bridge_not_enabled_yet_answers_not_ready) {
    auto t = LogosTestContext("delivery_module");
    BridgeFixture f; // init()ed with a client, enable() not run yet

    // enable() runs on the bring-up thread, so the library's first read at
    // node start can arrive before it. not_ready is its "retry later" class:
    // the retry finds the bridge up.
    f.bridge.validateProof(11, "reg", "rln-id", "deadbeef", 1700000000, "{}");

    LOGOS_ASSERT_TRUE(delivery_test_rln::g_responseFired);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastResponseReqId, static_cast<uint64_t>(11));
    const auto reply = nlohmann::json::parse(delivery_test_rln::g_lastResponseJson);
    LOGOS_ASSERT_FALSE(reply["success"].get<bool>());
    const auto inner = nlohmann::json::parse(reply["error"].get<std::string>());
    LOGOS_ASSERT_EQ(inner["class"].get<std::string>(), std::string("not_ready"));
    LOGOS_ASSERT_EQ(inner["kind"].get<std::string>(),
                    std::string("rln_bridge_disabled"));

    // The tstr dialect carries the same class at the top level.
    f.bridge.getMembershipState(12, "reg", "rln-id");
    const auto state = nlohmann::json::parse(delivery_test_rln::g_lastResponseJson);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastResponseReqId, static_cast<uint64_t>(12));
    LOGOS_ASSERT_EQ(state["error"]["class"].get<std::string>(), std::string("not_ready"));
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
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_responseFired);
}

LOGOS_TEST(rln_bridge_start_backend_refuses_before_enable) {
    auto t = LogosTestContext("delivery_module");
    rln_module_stub::resetStubError();
    RlnModule client{"delivery_module"};
    RlnBridge bridge;
    bridge.init(&client);

    LOGOS_ASSERT_EQ(bridge.startBackend("{}"),
                    std::string("rln bridge is not enabled"));
}

LOGOS_TEST(shared_rln_backend_lifecycle_is_explicit) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTable, table).empty());
    LOGOS_ASSERT_TRUE(table.at("logos.test").manageBackend);
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"r","epoch-size-sec":10,"manage-backend":false}})", table).empty());
    LOGOS_ASSERT_FALSE(table.at("logos.test").manageBackend);
    LOGOS_ASSERT_FALSE(parseRlnPresetTable(
        R"({"logos.test":{"manage-backend":"false"}})", table).empty());
}

LOGOS_TEST(mix_peer_methods_require_a_node_and_forward_library_results) {
    auto t = LogosTestContext("delivery_module");
    t.mockCFunction("logosdelivery_ctx_create").returns(1);
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.getLocalMixPeerRecord().success);
    LOGOS_ASSERT_FALSE(impl.addMixPeer("{}").success);
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_mixCallbackSetBeforeRuntime);
    t.mockCFunction("waku_mix_get_peer_record").returns(std::string(R"({"peerId":"mix-peer"})"));
    LOGOS_ASSERT_TRUE(impl.getLocalMixPeerRecord().success);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastMixCtxHandle,
                    delivery_test_rln::g_createdCtxHandle);
    LOGOS_ASSERT_TRUE(impl.addMixPeer(R"({"peerId":"mix-peer"})").success);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_lastMixCtxHandle,
                    delivery_test_rln::g_createdCtxHandle);
}

LOGOS_TEST(mix_rln_callback_remains_until_the_last_instance_is_released) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    auto first = std::make_unique<DeliveryModuleImpl>();
    auto second = std::make_unique<DeliveryModuleImpl>();
    LOGOS_ASSERT_TRUE(first->createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT_TRUE(second->createNode(R"({"logLevel":"INFO"})").success);
    LOGOS_ASSERT(delivery_test_rln::g_mixCallback != nullptr);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_mixSetCallbackCalls, 1);

    first.reset();
    LOGOS_ASSERT(delivery_test_rln::g_mixCallback != nullptr);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_mixSetCallbackCalls, 1);

    second.reset();
    LOGOS_ASSERT(delivery_test_rln::g_mixCallback == nullptr);
    LOGOS_ASSERT_EQ(delivery_test_rln::g_mixSetCallbackCalls, 2);
}

// Relay RLN and Mix RLN are independent: a preset may name a mix registry
// without relay RLN.
LOGOS_TEST(a_preset_may_name_a_mix_registry_without_relay_rln) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(R"({"": {"enabled": false, "mix-registry-id": "mixreg",
        "mix-metadata-topic": "/mix/1/metadata/proto"}})",
                                          table)
                          .empty());
    const RlnPresetEntry& entry = table.at("");
    LOGOS_ASSERT_FALSE(entry.enabled);
    LOGOS_ASSERT_TRUE(entry.hasMix());
    LOGOS_ASSERT_TRUE(entry.runsRln());
}

// Mix only: one registry, and the top-level epoch size is Mix's.
LOGOS_TEST(the_backend_start_config_for_mix_rln_only) {
    RlnPresetEntry cfg;
    cfg.mixRegistryId = "mixreg";
    auto start = nlohmann::json::parse(rlnBackendStartConfig(cfg, RlnMode::Core));
    LOGOS_ASSERT_EQ(start.value("epoch_size_sec", 0), static_cast<int>(kMixEpochSizeSec));
    LOGOS_ASSERT_EQ(start["registries"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(start["registries"][0].value("registry_id", ""), std::string("mixreg"));
}

// Mix as the delivery library decides it: a level above None, else `mix`,
// else the network preset.
LOGOS_TEST(mixIsOn_follows_the_library_rule) {
    const auto on = [](const char* cfg, const char* preset) {
        return mixIsOn(nlohmann::json::parse(cfg), preset);
    };
    // The network preset's default.
    LOGOS_ASSERT_TRUE(on(R"({})", "logos.dev"));
    LOGOS_ASSERT_TRUE(on(R"({})", "logos.test"));
    LOGOS_ASSERT_FALSE(on(R"({})", ""));
    LOGOS_ASSERT_FALSE(on(R"({})", "twn"));
    // `mix` in any layer, messagingOverrides last.
    LOGOS_ASSERT_FALSE(on(R"({"mix":false})", "logos.dev"));
    LOGOS_ASSERT_FALSE(on(R"({"messagingOverrides":{"mix":false}})", "logos.dev"));
    LOGOS_ASSERT_TRUE(on(R"({"messagingOverrides":{"mix":true}})", ""));
    LOGOS_ASSERT_TRUE(on(R"({"entryLayer":"kernel","kernelConf":{"mix":true}})", ""));
    // The level.
    LOGOS_ASSERT_FALSE(on(R"({"messagingOverrides":{"anonymityLevel":"None"}})", "logos.dev"));
    LOGOS_ASSERT_TRUE(on(R"({"messagingOverrides":{"anonymity-level":"Preferred"}})", ""));
    LOGOS_ASSERT_TRUE(on(R"({"anonymityLevel":"Required"})", ""));
    // The kernel entry reads no level.
    LOGOS_ASSERT_FALSE(on(R"({"entryLayer":"kernel","anonymityLevel":"Required"})", ""));
}

LOGOS_TEST(relayRlnIsOn_drops_relay_rln_on_the_callers_word) {
    const auto on = [](const char* cfg) { return relayRlnIsOn(nlohmann::json::parse(cfg)); };
    LOGOS_ASSERT_TRUE(on(R"({})"));
    LOGOS_ASSERT_TRUE(on(R"({"rln-relay":true})"));
    LOGOS_ASSERT_FALSE(on(R"({"rln-relay":false})"));
    LOGOS_ASSERT_FALSE(on(R"({"messagingOverrides":{"rln-relay":false}})"));
    LOGOS_ASSERT_FALSE(on(R"({"kernelConf":{"rlnRelay":false}})"));
    // An on-chain contract picks the library's own backend.
    LOGOS_ASSERT_FALSE(on(R"({"messagingOverrides":{"rln-relay-eth-contract-address":"0xabc"}})"));
    LOGOS_ASSERT_TRUE(on(R"({"rln-relay-eth-contract-address":""})"));
    // So does a keystore: the library's RLN conf builder reads it as on-chain.
    LOGOS_ASSERT_FALSE(on(R"({"messagingOverrides":{"rln-relay-cred-path":"/k.json"}})"));
    LOGOS_ASSERT_FALSE(on(R"({"kernelConf":{"rlnRelayCredPath":"/k.json"}})"));
}

// The preset decides which memberships exist; the config only drops them, each
// on its own.
LOGOS_TEST(nodeRlnPreset_drops_each_membership_independently) {
    RlnPresetEntry preset;
    preset.enabled = true;
    preset.registryId = "reg";
    preset.epochSizeSec = 600;
    preset.mixRegistryId = "mixreg";
    const auto node = [&](const char* cfg, const char* name) {
        return nodeRlnPreset(preset, nlohmann::json::parse(cfg), name);
    };

    auto both = node(R"({})", "logos.dev");
    LOGOS_ASSERT_TRUE(both.enabled);
    LOGOS_ASSERT_TRUE(both.hasMix());

    auto relayOnly = node(R"({"messagingOverrides":{"mix":false}})", "logos.dev");
    LOGOS_ASSERT_TRUE(relayOnly.enabled);
    LOGOS_ASSERT_FALSE(relayOnly.hasMix());

    auto mixOnly = node(R"({"messagingOverrides":{"rln-relay":false}})", "logos.dev");
    LOGOS_ASSERT_FALSE(mixOnly.enabled);
    LOGOS_ASSERT_TRUE(mixOnly.hasMix());

    auto none = node(R"({"messagingOverrides":{"rln-relay":false,"anonymityLevel":"None"}})",
                     "logos.dev");
    LOGOS_ASSERT_FALSE(none.runsRln());

    // Config cannot add a membership the preset lacks.
    RlnPresetEntry off;
    LOGOS_ASSERT_FALSE(
        nodeRlnPreset(off, nlohmann::json::parse(R"({"rln-relay":true,"mix":true})"), "")
            .runsRln());
}

// Mix RLN without relay RLN: the library's relay plugin is not installed, the
// mix keys still reach the library, and the bring-up still runs.
LOGOS_TEST(a_mix_only_node_installs_no_relay_plugin_but_gets_the_mix_keys) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTableWithMix);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(
        R"({"preset":"logos.test","messagingOverrides":{"rln-relay":false,"anonymityLevel":"Required"}})")
        .success);
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_callbacksSet);
    auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
    LOGOS_ASSERT_EQ(libCfg["messagingOverrides"].value("mix-rln-registry-id", ""),
                    std::string("mixreg"));
    const auto state = impl.rlnState().value;
    LOGOS_ASSERT_FALSE(state.contains("registryId"));
    LOGOS_ASSERT_EQ(state.value("mixRegistryId", ""), std::string("mixreg"));
    LOGOS_ASSERT_EQ(settledRlnState(impl), std::string("Failed"));
}
