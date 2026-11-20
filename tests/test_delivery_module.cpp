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
#include "libp2p_bring_up.h"
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

// A discovery plugin's bring-up from a libp2p client and its createNode config.
static std::shared_ptr<Libp2pBringUp> discoveryBringUp(Libp2pModule* libp2p, std::string config) {
    return std::make_shared<Libp2pBringUp>(libp2p, std::move(config), [](const std::string&) {});
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

    delete impl; // joins the lifecycle worker, which runs the stop first
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
    DeliveryServiceDiscoveryPlugin plugin(discoveryBringUp(&libp2p, "{}"));

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
    DeliveryServiceDiscoveryPlugin plugin(discoveryBringUp(&libp2p, R"({"bootstrapNodes":[
        {"peerId":"16Uiu2HAmA","addrs":["/ip4/10.0.0.1/tcp/1"]},
        {"peerId":"16Uiu2HAmB","addrs":["/ip4/10.0.0.2/tcp/1"]},
        {"peerId":"16Uiu2HAmC","addrs":["/ip4/10.0.0.3/tcp/1"]}],
        "mountKad":true,"mountServiceDiscovery":true})"));

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
        DeliveryServiceDiscoveryPlugin plugin(discoveryBringUp(&libp2p, "{}"));

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
    DeliveryServiceDiscoveryPlugin plugin(discoveryBringUp(&libp2p, "{}"));

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
    DeliveryServiceDiscoveryPlugin plugin(discoveryBringUp(&libp2p, "{}"));

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
    DeliveryServiceDiscoveryPlugin plugin(discoveryBringUp(&libp2p, "{}"));

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
    DeliveryServiceDiscoveryPlugin plugin(discoveryBringUp(nullptr, "{}"));

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

// A node started again after a stop gets its RLN backend brought up again:
// stop publishes Stopped, the next start runs the bring-up anew (here against
// no framework context, so it settles on Failed again, as createNode's did).
LOGOS_TEST(stop_then_start_brings_the_rln_backend_up_again) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnStateEvent();
    delivery_test_events::resetNodeLifecycleEvents();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    LOGOS_ASSERT_EQ(settledRlnState(*impl), std::string("Failed"));
    const int afterBringUp = awaitRlnStateEvents(2).transitions; // Initializing, Failed

    LOGOS_ASSERT_TRUE(impl->stop().success);
    const auto stopped = awaitRlnStateEvents(afterBringUp + 1);
    LOGOS_ASSERT_EQ(stopped.state, std::string("Stopped"));

    LOGOS_ASSERT_TRUE(impl->start().success);
    const auto restarted = awaitRlnStateEvents(afterBringUp + 3); // Initializing, Failed again
    LOGOS_ASSERT_EQ(restarted.state, std::string("Failed"));
    LOGOS_ASSERT_EQ(settledRlnState(*impl), std::string("Failed"));

    // Only a Ready verdict stands. This bring-up Failed, so another start()
    // runs it once more: Initializing, Failed again.
    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT_EQ(awaitRlnStateEvents(afterBringUp + 5).transitions, afterBringUp + 5);
    delete impl;
    delivery_test_rln::resetRlnMockState();
}

// The stop after a failed start and an explicit stop() both stop the RLN
// backend. Both are requests to the one lifecycle worker, which runs them in
// turn; destruction waits for the failed start's node stop.
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

// A start that fails at once must leave RLN Stopped: its bring-up is ordered
// before the teardown the failed start asks for, never after it.
LOGOS_TEST(a_start_that_fails_at_once_leaves_rln_stopped) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnStateEvent();
    delivery_test_events::resetNodeLifecycleEvents();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    LOGOS_ASSERT_EQ(settledRlnState(*impl), std::string("Failed"));
    const int afterBringUp = awaitRlnStateEvents(2).transitions; // Initializing, Failed

    // The mock replies from inside the dispatch, so the failure is known
    // before start() returns.
    delivery_test_rln::g_startNodeReplyError = "failed to start external service discovery";
    LOGOS_ASSERT_TRUE(impl->start().success);
    LOGOS_ASSERT_FALSE(delivery_test_events::g_lastNodeStarted.success);

    // The start's bring-up (Initializing, Failed), then the teardown.
    const auto settled = awaitRlnStateEvents(afterBringUp + 3);
    LOGOS_ASSERT_EQ(settled.transitions, afterBringUp + 3);
    LOGOS_ASSERT_EQ(settled.state, std::string("Stopped"));

    // Nothing follows it: no bring-up is left over for the stopped node.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    LOGOS_ASSERT_EQ(delivery_test_events::lastRlnState().transitions, afterBringUp + 3);
    LOGOS_ASSERT_EQ(impl->rlnState().value.value("state", ""), std::string("Stopped"));

    delete impl;
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_stop_node"), 1);
    delivery_test_rln::resetRlnMockState();
}

// While a failed start's cleanup is pending -- here queued behind a held
// bring-up -- another start() is refused and dispatches nothing: it would
// start a node that was never stopped. Once the cleanup has run, start() is
// accepted again.
LOGOS_TEST(a_start_is_refused_while_a_failed_start_is_cleaned_up) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnStateEvent();
    delivery_test_events::resetNodeLifecycleEvents();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    LOGOS_ASSERT_EQ(settledRlnState(*impl), std::string("Failed"));
    const int afterBringUp = awaitRlnStateEvents(2).transitions;

    // Hold the worker in the first start's bring-up, so that start's cleanup
    // stays queued behind it.
    auto& gate = delivery_test_events::g_rlnStateGate;
    gate.arm("Initializing");
    delivery_test_rln::g_startNodeReplyError = "failed to start external service discovery";
    const bool firstStart = impl->start().success;
    const bool held = gate.awaitHolding(std::chrono::seconds(5));
    const int startsDispatched = t.cFunctionCallCount("logosdelivery_ctx_start_node");

    delivery_test_rln::g_startNodeReplyError.clear();
    const StdLogosResult refused = impl->start();
    const int startsAfterRefusal = t.cFunctionCallCount("logosdelivery_ctx_start_node");
    const int stopsWhileHeld = t.cFunctionCallCount("logosdelivery_ctx_stop_node");
    gate.release();

    LOGOS_ASSERT_TRUE(firstStart);
    LOGOS_ASSERT_TRUE(held);
    LOGOS_ASSERT_FALSE(refused.success);
    LOGOS_ASSERT_FALSE(refused.error.empty());
    LOGOS_ASSERT_EQ(startsAfterRefusal, startsDispatched);
    LOGOS_ASSERT_EQ(stopsWhileHeld, 0);

    // The held bring-up's Failed, then the cleanup: node stop, RLN Stopped.
    const auto settled = awaitRlnStateEvents(afterBringUp + 3);
    LOGOS_ASSERT_EQ(settled.transitions, afterBringUp + 3);
    LOGOS_ASSERT_EQ(settled.state, std::string("Stopped"));

    // The cleanup is over a moment after Stopped is published; from then on a
    // start is accepted. A refused attempt dispatches nothing.
    bool accepted = false;
    for (int i = 0; i < 500 && !accepted; ++i) {
        accepted = impl->start().success;
        if (!accepted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    LOGOS_ASSERT_TRUE(accepted);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_start_node"), startsDispatched + 1);
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.success);

    delete impl;
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("logosdelivery_ctx_stop_node"), 1);
    delivery_test_rln::resetRlnMockState();
}

// stop() and start() only post to the lifecycle worker: neither waits for a
// bring-up that is still running. The bring-up is held in flight at its
// Initializing event (see RlnStateGate): without a framework context the
// stubbed RLN module's start is never reached, so that is where a unit test
// can hold the worker.
LOGOS_TEST(a_slow_rln_bring_up_does_not_block_stop_or_start) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnStateEvent();
    delivery_test_events::resetNodeLifecycleEvents();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    LOGOS_ASSERT_EQ(settledRlnState(*impl), std::string("Failed"));
    const int afterBringUp = awaitRlnStateEvents(2).transitions; // Initializing, Failed

    auto& gate = delivery_test_events::g_rlnStateGate;
    gate.arm("Initializing");
    const bool firstStart = impl->start().success;
    const bool held = gate.awaitHolding(std::chrono::seconds(5));

    // The bring-up is in flight and stays so across the calls below.
    const auto before = std::chrono::steady_clock::now();
    const bool stopped = impl->stop().success;
    const bool started = impl->start().success;
    const bool enabled = impl->rlnBridgeEnable().success; // no framework: refused, but returns
    const auto elapsed = std::chrono::steady_clock::now() - before;
    const bool stillHeld = gate.isHolding();
    const int whileHeld = delivery_test_events::lastRlnState().transitions;
    gate.release();

    LOGOS_ASSERT_TRUE(firstStart);
    LOGOS_ASSERT_TRUE(held);
    LOGOS_ASSERT_TRUE(stopped);
    LOGOS_ASSERT_TRUE(started);
    LOGOS_ASSERT_FALSE(enabled);
    LOGOS_ASSERT_TRUE(stillHeld);
    LOGOS_ASSERT_TRUE(elapsed < std::chrono::seconds(2));
    LOGOS_ASSERT_EQ(whileHeld, afterBringUp + 1); // the held Initializing only

    delete impl;
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

// Only logos.test ships with RLN on; "" and logos.dev stay off.
LOGOS_TEST(builtin_presets_enable_rln_only_on_logos_test) {
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
    // It turns allocator discovery on itself, with peer-any, and every
    // bootstrap node it names is a /p2p/ multiaddr.
    LOGOS_ASSERT_TRUE(test.hasAllocator());
    LOGOS_ASSERT_TRUE(test.allocatorDiscover);
    LOGOS_ASSERT_EQ(test.allocatorAuthType, std::string("peer-any"));
    LOGOS_ASSERT_TRUE(test.allocatorPeerId.empty());
    LOGOS_ASSERT_FALSE(test.allocatorBootstrapNodes.empty());
    for (const auto& addr : test.allocatorBootstrapNodes) {
        nlohmann::json node;
        LOGOS_ASSERT_TRUE(delivery_discovery::bootstrapNodeFrom(addr, node).empty());
    }

    LOGOS_ASSERT_FALSE(resolveRlnPreset("logos.dev", error).enabled);
    LOGOS_ASSERT_TRUE(error.empty());
    LOGOS_ASSERT_FALSE(resolveRlnPreset("", error).enabled);
    LOGOS_ASSERT_TRUE(error.empty());
    // Neither asks an allocator.
    LOGOS_ASSERT_FALSE(resolveRlnPreset("logos.dev", error).hasAllocator());
    LOGOS_ASSERT_FALSE(resolveRlnPreset("", error).hasAllocator());
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

// logos.dev carries RLN off, so naming it must not turn it on.
LOGOS_TEST(builtin_logos_dev_preset_leaves_rln_off) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"INFO","preset":"logos.dev"})").success);
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

// The mix deployment with a membership allocator: the RLN module asks it
// first for both registries.
static constexpr const char* kRlnPresetTableWithAllocator = R"({
  "logos.test": {
    "enabled": true,
    "registry-id": "reg",
    "epoch-size-sec": 600,
    "rate-limit": 100,
    "mix-registry-id": "mixreg",
    "mix-rate-limit": 20,
    "mix-metadata-topic": "/mix/1/metadata/proto",
    "allocator-peer-id": "16Uiu2HAmAllocator",
    "allocator-multiaddr": "/ip4/127.0.0.1/tcp/9000"
  }
})";

// allocator-bootstrap-nodes is read as written; an entry that is not a /p2p/
// multiaddr fails the table.
LOGOS_TEST(preset_table_parses_the_allocator_bootstrap_nodes) {
    const std::string base = R"("enabled":true,"registry-id":"reg","epoch-size-sec":600)";
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(
        R"({"logos.test":{)" + base + R"(,"allocator-bootstrap-nodes":[
            "/ip4/10.0.0.1/tcp/1/p2p/16Uiu2HAmA","/ip4/10.0.0.2/tcp/1/p2p/16Uiu2HAmB"]}})",
        table).empty());
    LOGOS_ASSERT_EQ(table["logos.test"].allocatorBootstrapNodes.size(), size_t{2});
    LOGOS_ASSERT_EQ(table["logos.test"].allocatorBootstrapNodes[1],
                    std::string("/ip4/10.0.0.2/tcp/1/p2p/16Uiu2HAmB"));

    for (const char* bad : {R"("/ip4/10.0.0.1/tcp/1")", R"(7)", R"("/p2p/16Uiu2HAmA")"}) {
        const std::string err = parseRlnPresetTable(
            R"({"logos.test":{)" + base + R"(,"allocator-bootstrap-nodes":[)" + bad + "]}}", table);
        LOGOS_ASSERT_TRUE(err.find("allocator-bootstrap-nodes") != std::string::npos);
    }
    LOGOS_ASSERT_FALSE(parseRlnPresetTable(
        R"({"logos.test":{)" + base + R"(,"allocator-bootstrap-nodes":"x"}})", table).empty());
}

// A preset's allocator defaults to the peer-any vector; its keys are read as
// written; the combinations that cannot work are refused where written.
LOGOS_TEST(preset_table_parses_the_allocator_with_defaults_and_refusals) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTableWithAllocator, table).empty());
    const RlnPresetEntry& e = table["logos.test"];
    LOGOS_ASSERT_TRUE(e.hasAllocator());
    LOGOS_ASSERT_EQ(e.allocatorPeerId, std::string("16Uiu2HAmAllocator"));
    LOGOS_ASSERT_EQ(e.allocatorMultiaddr, std::string("/ip4/127.0.0.1/tcp/9000"));
    LOGOS_ASSERT_EQ(e.allocatorAuthType, std::string("peer-any"));
    LOGOS_ASSERT_TRUE(e.allocatorAuthPayload.empty());
    LOGOS_ASSERT_TRUE(e.allocatorAuthProvider.empty());

    LOGOS_ASSERT_TRUE(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,
            "allocator-peer-id":"p","allocator-multiaddr":"/m","allocator-auth-type":"eth-allowlist",
            "allocator-auth-provider":"rln_sign_module","allocator-auth-args":"{\"rate_max\":100}"}})",
        table).empty());
    LOGOS_ASSERT_EQ(table["logos.test"].allocatorAuthType, std::string("eth-allowlist"));
    LOGOS_ASSERT_EQ(table["logos.test"].allocatorAuthProvider, std::string("rln_sign_module"));
    LOGOS_ASSERT_EQ(table["logos.test"].allocatorAuthArgs, std::string("{\"rate_max\":100}"));

    // A peer names an allocator, so discovery is on beside it.
    LOGOS_ASSERT_TRUE(table["logos.test"].allocatorDiscover);

    // Without allocator keys an RLN preset has no allocator: funded
    // provisioning only, like a disabled preset.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTableWithMix, table).empty());
    LOGOS_ASSERT_FALSE(table["logos.test"].hasAllocator());
    LOGOS_ASSERT_FALSE(table["logos.test"].allocatorDiscover);
    LOGOS_ASSERT_TRUE(table["logos.test"].allocatorAuthType.empty());
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(R"({"logos.test":{"enabled":false}})", table).empty());
    LOGOS_ASSERT_FALSE(table["logos.test"].hasAllocator());
    LOGOS_ASSERT_TRUE(table["logos.test"].allocatorAuthType.empty());

    const std::string base = R"("enabled":true,"registry-id":"reg","epoch-size-sec":600)";
    // Any allocator key but allocator-discover turns discovery on.
    for (const char* key : {
             R"("allocator-bootstrap-nodes":["/ip4/10.0.0.1/tcp/1/p2p/16Uiu2HAmA"])",
             R"("allocator-auth-type":"peer-any")",
             R"("allocator-auth-args":"{}")",
             R"("allocator-auth-type":"eth-allowlist","allocator-auth-payload":"00")",
             R"("allocator-auth-type":"eth-allowlist","allocator-auth-provider":"rln_sign_module")",
         }) {
        LOGOS_ASSERT_TRUE(parseRlnPresetTable(R"({"logos.test":{)" + base + "," + key + "}}", table).empty());
        LOGOS_ASSERT_TRUE(table["logos.test"].allocatorDiscover);
        LOGOS_ASSERT_TRUE(table["logos.test"].hasAllocator());
    }
    // Written, allocator-discover decides: on with no other key (peer-any),
    // off beside one.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(R"({"logos.test":{)" + base + R"(,"allocator-discover":true}})", table).empty());
    LOGOS_ASSERT_TRUE(table["logos.test"].allocatorDiscover);
    LOGOS_ASSERT_TRUE(table["logos.test"].allocatorPeerId.empty());
    LOGOS_ASSERT_EQ(table["logos.test"].allocatorAuthType, std::string("peer-any"));
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(
        R"({"logos.test":{)" + base + R"(,"allocator-discover":false,"allocator-bootstrap-nodes":[]}})", table).empty());
    LOGOS_ASSERT_FALSE(table["logos.test"].hasAllocator());

    // Discovery off and no peer: funded provisioning only. Off with a peer:
    // that peer alone.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(R"({"logos.test":{)" + base + R"(,"allocator-discover":false}})", table).empty());
    LOGOS_ASSERT_FALSE(table["logos.test"].hasAllocator());
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(
        R"({"logos.test":{)" + base + R"(,"allocator-discover":false,"allocator-peer-id":"p","allocator-multiaddr":"/m"}})",
        table).empty());
    LOGOS_ASSERT_TRUE(table["logos.test"].hasAllocator());
    LOGOS_ASSERT_FALSE(table["logos.test"].allocatorDiscover);
    LOGOS_ASSERT_EQ(table["logos.test"].allocatorAuthType, std::string("peer-any"));

    struct Refused { std::string entry; std::string needle; };
    for (const Refused& r : {
             Refused{R"({"logos.test":{)" + base + R"(,"allocator-peer-id":"p"}})", "both allocator-peer-id and allocator-multiaddr"},
             Refused{R"({"logos.test":{)" + base + R"(,"allocator-discover":"yes"}})", "allocator-discover must be a boolean"},
             Refused{R"({"logos.test":{)" + base + R"(,"allocator-auth-type":"eth-allowlist"}})", "needs allocator-auth-payload or allocator-auth-provider"},
             Refused{R"({"logos.test":{"enabled":false,"allocator-discover":true}})", "RLN is not enabled"},
             Refused{R"({"logos.test":{)" + base + R"(,"allocator-peer-id":"p","allocator-multiaddr":"/m",
                        "allocator-auth-payload":"00","allocator-auth-provider":"m"}})", "sets both"},
         }) {
        const std::string err = parseRlnPresetTable(r.entry, table);
        LOGOS_ASSERT_TRUE(err.find(r.needle) != std::string::npos);
    }
}

// With an allocator every registry entry is an object carrying its own
// allocator block; peer-any carries no payload or provider, explicit payload
// material passes through.
LOGOS_TEST(the_backend_start_config_carries_the_allocator_per_registry) {
    DeliveryRlnConfig cfg;
    cfg.enabled = true;
    cfg.registryId = "reg";
    cfg.epochSizeSec = 600;
    cfg.rateLimit = 100;
    cfg.mixRegistryId = "mixreg";
    cfg.mixEpochSizeSec = 10;
    cfg.mixMaxEpochGap = 3;
    cfg.mixRateLimit = 20;
    cfg.allocatorDiscover = true;
    cfg.allocatorPeerId = "16Uiu2HAmAllocator";
    cfg.allocatorMultiaddr = "/ip4/127.0.0.1/tcp/9000";
    cfg.allocatorAuthType = "peer-any";

    auto start = nlohmann::json::parse(rlnBackendStartConfig(cfg));
    LOGOS_ASSERT_EQ(start["registries"].size(), static_cast<size_t>(2));
    const nlohmann::json relayAllocator{
        {"discover", true}, {"peer_id", "16Uiu2HAmAllocator"}, {"multiaddr", "/ip4/127.0.0.1/tcp/9000"},
        {"auth_type", "peer-any"}};
    LOGOS_ASSERT_TRUE(start["registries"][0] == nlohmann::json({{"registry_id", "reg"}, {"allocator", relayAllocator}}));
    LOGOS_ASSERT_FALSE(start["registries"][1]["allocator"].contains("auth_payload"));
    LOGOS_ASSERT_FALSE(start["registries"][1]["allocator"].contains("auth_provider"));
    LOGOS_ASSERT_EQ(start["registries"][1]["registry_id"].get<std::string>(), std::string("mixreg"));
    LOGOS_ASSERT_EQ(start["registries"][1]["epoch_size_sec"].get<int>(), 10);
    LOGOS_ASSERT_FALSE(start.contains("allocator"));

    // A producer, or a payload, from the preset passes through unchanged.
    DeliveryRlnConfig relay = cfg;
    relay.mixRegistryId.clear();
    DeliveryRlnConfig producer = relay;
    producer.allocatorAuthType = "eth-allowlist";
    producer.allocatorAuthProvider = "rln_sign_module";
    producer.allocatorAuthArgs = "{\"rate_max\":100}";
    auto viaProducer = nlohmann::json::parse(rlnBackendStartConfig(producer));
    const nlohmann::json& block = viaProducer["registries"][0]["allocator"];
    LOGOS_ASSERT_EQ(block["auth_type"].get<std::string>(), std::string("eth-allowlist"));
    LOGOS_ASSERT_EQ(block["auth_provider"].get<std::string>(), std::string("rln_sign_module"));
    LOGOS_ASSERT_EQ(block["auth_args"].get<std::string>(), std::string("{\"rate_max\":100}"));
    LOGOS_ASSERT_FALSE(block.contains("auth_payload"));
    DeliveryRlnConfig payload = relay;
    payload.allocatorAuthPayload = "aa";
    auto viaPayload = nlohmann::json::parse(rlnBackendStartConfig(payload));
    LOGOS_ASSERT_EQ(viaPayload["registries"][0]["allocator"]["auth_payload"].get<std::string>(), std::string("aa"));
    LOGOS_ASSERT_FALSE(viaPayload["registries"][0]["allocator"].contains("auth_provider"));

    // Discovery alone: the block names no peer. A peer with discovery off:
    // the block says so. Neither: a plain-string entry, funded only.
    DeliveryRlnConfig discovered = relay;
    discovered.allocatorPeerId.clear();
    discovered.allocatorMultiaddr.clear();
    auto viaDiscovery = nlohmann::json::parse(rlnBackendStartConfig(discovered));
    LOGOS_ASSERT_TRUE(viaDiscovery["registries"][0]["allocator"]
                      == nlohmann::json({{"discover", true}, {"auth_type", "peer-any"}}));
    DeliveryRlnConfig pinned = relay;
    pinned.allocatorDiscover = false;
    auto viaPeer = nlohmann::json::parse(rlnBackendStartConfig(pinned));
    LOGOS_ASSERT_TRUE(viaPeer["registries"][0]["allocator"]["discover"] == nlohmann::json(false));
    LOGOS_ASSERT_EQ(viaPeer["registries"][0]["allocator"]["peer_id"].get<std::string>(), std::string("16Uiu2HAmAllocator"));
    DeliveryRlnConfig funded = discovered;
    funded.allocatorDiscover = false;
    auto viaFunding = nlohmann::json::parse(rlnBackendStartConfig(funded));
    LOGOS_ASSERT_TRUE(viaFunding["registries"][0] == nlohmann::json("reg"));
}

// The shared bring-up issues createNode and start once each, whoever asks.
LOGOS_TEST(the_libp2p_bring_up_issues_each_call_once) {
    Libp2pModule::reset();
    Libp2pModule::bringUpSucceeds = true;
    Libp2pModule libp2p("test");
    std::vector<std::string> lines;
    Libp2pBringUp bringUp(&libp2p, "{}", [&lines](const std::string& l) { lines.push_back(l); });
    LOGOS_ASSERT_FALSE(bringUp.ready());
    LOGOS_ASSERT_TRUE(bringUp.ensure().empty());
    LOGOS_ASSERT_TRUE(bringUp.ready());
    LOGOS_ASSERT_TRUE(bringUp.ensure().empty());
    LOGOS_ASSERT_EQ(Libp2pModule::startCalls, 1);
    LOGOS_ASSERT_FALSE(lines.empty());

    Libp2pModule::reset();
    Libp2pModule::createNodeErrorCode = "object_unavailable";
    Libp2pBringUp absent(&libp2p, "{}", [](const std::string&) {});
    LOGOS_ASSERT_EQ(absent.ensure(), std::string(Libp2pBringUp::kUnavailable));
    LOGOS_ASSERT_EQ(Libp2pModule::startCalls, 0);
    Libp2pBringUp none(nullptr, "{}", [](const std::string&) {});
    LOGOS_ASSERT_EQ(none.ensure(), std::string("no libp2p_module client"));
    Libp2pModule::reset();
}

// A createNode libp2p refuses because a node already exists (a module
// reload, another consumer) is taken as that node, created and running: no
// second start, which would crash libp2p_module.
LOGOS_TEST(a_refused_createNode_on_a_running_node_issues_no_second_start) {
    Libp2pModule::reset();
    Libp2pModule::bringUpSucceeds = false; // createNode: success=false
    Libp2pModule::nodeInfoSucceeds = true;
    Libp2pModule libp2p("test");
    Libp2pBringUp bringUp(&libp2p, "{}", [](const std::string&) {});
    LOGOS_ASSERT_TRUE(bringUp.ensure().empty());
    LOGOS_ASSERT_TRUE(bringUp.ready());
    LOGOS_ASSERT_EQ(Libp2pModule::startCalls, 0);

    // Refused and nobody answers: the refusal verdict stands.
    Libp2pModule::reset();
    Libp2pBringUp refused(&libp2p, "{}", [](const std::string&) {});
    LOGOS_ASSERT_FALSE(refused.ensure().empty());
    Libp2pModule::reset();
}

// Two callers (the lifecycle worker and the discovery plugin's start) share
// one bring-up: one createNode, one start, both ready.
LOGOS_TEST(concurrent_ensure_calls_share_one_bring_up) {
    Libp2pModule::reset();
    Libp2pModule::bringUpSucceeds = true;
    Libp2pModule libp2p("test");
    Libp2pBringUp bringUp(&libp2p, "{}", [](const std::string&) {});
    std::string a, b;
    std::thread ta([&] { a = bringUp.ensure(); });
    std::thread tb([&] { b = bringUp.ensure(); });
    ta.join();
    tb.join();
    LOGOS_ASSERT_TRUE(a.empty());
    LOGOS_ASSERT_TRUE(b.empty());
    LOGOS_ASSERT_TRUE(bringUp.ready());
    LOGOS_ASSERT_EQ(Libp2pModule::startCalls, 1);
    Libp2pModule::reset();
}

// rlnBridgeEnable after a failed bring-up runs the bring-up again instead of
// enabling a bridge with no backend behind it.
LOGOS_TEST(rlnBridgeEnable_after_a_failed_bring_up_tries_again) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    delivery_test_events::resetRlnStateEvent();
    RlnPresetsFile presets(kRlnPresetTable);
    auto* impl = createRlnImpl(t);
    LOGOS_ASSERT_EQ(settledRlnState(*impl), std::string("Failed"));
    const int afterBringUp = awaitRlnStateEvents(2).transitions;
    (void)impl->rlnBridgeEnable(); // fails here too (no framework context)
    const auto again = awaitRlnStateEvents(afterBringUp + 2); // Initializing, Failed
    LOGOS_ASSERT_EQ(again.transitions, afterBringUp + 2);
    LOGOS_ASSERT_EQ(again.state, std::string("Failed"));
    delete impl;
    delivery_test_rln::resetRlnMockState();
}

// The mix keys the library needs beyond the registry default to native Mix's
// own scope and epoch profile.
LOGOS_TEST(preset_table_parses_the_mix_registry_with_defaults) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTableWithMix, table).empty());
    const RlnPresetEntry& e = table["logos.test"];
    LOGOS_ASSERT_TRUE(e.hasMix());
    LOGOS_ASSERT_EQ(e.rateLimit, static_cast<uint64_t>(100));
    LOGOS_ASSERT_EQ(e.mixRegistryId, std::string("mixreg"));
    LOGOS_ASSERT_EQ(e.mixRlnIdentifier, std::string(kMixRlnIdentifierV1));
    LOGOS_ASSERT_EQ(e.mixEpochSizeSec, static_cast<uint64_t>(10));
    LOGOS_ASSERT_EQ(e.mixMaxEpochGap, static_cast<uint64_t>(3));
    LOGOS_ASSERT_EQ(e.mixRateLimit, static_cast<uint64_t>(20));
    LOGOS_ASSERT_EQ(e.mixMetadataTopic, std::string("/mix/1/metadata/proto"));

    // The identifier is taken as written; the epoch size and gap may be
    // written, as the values Mix runs at.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,
            "mix-registry-id":"mixreg","mix-metadata-topic":"/t","mix-epoch-size-sec":10,
            "mix-max-epoch-gap":3,"mix-rln-identifier":")" + std::string(64, 'a') + R"("}})",
        table).empty());
    LOGOS_ASSERT_EQ(table["logos.test"].mixEpochSizeSec, static_cast<uint64_t>(10));
    LOGOS_ASSERT_EQ(table["logos.test"].mixMaxEpochGap, static_cast<uint64_t>(3));
    LOGOS_ASSERT_EQ(table["logos.test"].mixRlnIdentifier, std::string(64, 'a'));

    // Without a mix registry nothing mix-related is in force.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTable, table).empty());
    LOGOS_ASSERT_FALSE(table["logos.test"].hasMix());
    LOGOS_ASSERT_EQ(table["logos.test"].rateLimit, static_cast<uint64_t>(0));
}

// A mix registry that cannot work is refused where it is written.
LOGOS_TEST(preset_table_refuses_an_unusable_mix_entry) {
    std::map<std::string, RlnPresetEntry> table;
    // Mix proofs ride relay RLN.
    LOGOS_ASSERT_FALSE(parseRlnPresetTable(
        R"({"logos.test":{"enabled":false,"mix-registry-id":"mixreg","mix-metadata-topic":"/t"}})",
        table).empty());
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
    // Native Mix's proof plugin runs at 10 s epochs and a gap of 3 and fails
    // node start on anything else, so nothing else can be written.
    const std::string mixBase = R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,
            "mix-registry-id":"mixreg","mix-metadata-topic":"/t",)";
    struct Refused { std::string keys; std::string needle; };
    for (const Refused& r : {
             Refused{R"("mix-epoch-size-sec":600)", "mix-epoch-size-sec must be 10"},
             Refused{R"("mix-max-epoch-gap":0)", "mix-max-epoch-gap must be 3"},
         }) {
        const std::string err = parseRlnPresetTable(mixBase + r.keys + "}}", table);
        LOGOS_ASSERT_TRUE(err.find("preset \"logos.test\"") != std::string::npos);
        LOGOS_ASSERT_TRUE(err.find(r.needle) != std::string::npos);
    }
    // Without a mix registry the keys are not in force.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,"mix-epoch-size-sec":600}})",
        table).empty());
}

// Relay and mix may name one registry when relay runs at Mix's epoch size and
// gap and the two agree on a rate: the registry's one membership backs both
// scopes.
LOGOS_TEST(preset_table_accepts_a_registry_shared_by_relay_and_mix) {
    std::map<std::string, RlnPresetEntry> table;
    const std::string shared = R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":10,
            "max-epoch-gap":3,"mix-registry-id":"reg","mix-metadata-topic":"/t")";
    for (const char* rates : {
             "", R"(,"rate-limit":50)", R"(,"mix-rate-limit":50)",
             R"(,"rate-limit":50,"mix-rate-limit":50)",
         }) {
        LOGOS_ASSERT_TRUE(parseRlnPresetTable(shared + rates + "}}", table).empty());
        LOGOS_ASSERT_TRUE(table["logos.test"].sharesRegistry());
        LOGOS_ASSERT_EQ(table["logos.test"].sharedRateLimit(),
                        static_cast<uint64_t>(std::string(rates).empty() ? 0 : 50));
    }

    // Two registries share nothing.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTableWithMix, table).empty());
    LOGOS_ASSERT_FALSE(table["logos.test"].sharesRegistry());
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTable, table).empty());
    LOGOS_ASSERT_FALSE(table["logos.test"].sharesRegistry());
}

// Ids that differ only in letter case are neither one registry nor safely
// two: the entry is refused whatever its other values.
LOGOS_TEST(preset_table_refuses_registry_ids_that_differ_only_in_case) {
    std::map<std::string, RlnPresetEntry> table;
    for (const char* keys : {
             R"("registry-id":"logos:testnet:ab","epoch-size-sec":600,"mix-registry-id":"logos:testnet:AB")",
             R"("registry-id":"Reg","epoch-size-sec":10,"max-epoch-gap":3,"mix-registry-id":"reg")",
         }) {
        const std::string err = parseRlnPresetTable(
            std::string(R"({"logos.test":{"enabled":true,"mix-metadata-topic":"/t",)") + keys + "}}", table);
        LOGOS_ASSERT_TRUE(err.find("preset \"logos.test\" registry-id and mix-registry-id differ only in letter case")
                          != std::string::npos);
        LOGOS_ASSERT_TRUE(err.find("spelled identically") != std::string::npos);
    }
}

// Each condition a shared registry fails is named. An unwritten max-epoch-gap
// is the RLN module's default of 1, which is not Mix's 3.
LOGOS_TEST(preset_table_refuses_a_shared_registry_relay_cannot_run_at_mix_values) {
    std::map<std::string, RlnPresetEntry> table;
    const std::string tail = R"(,"mix-registry-id":"reg","mix-metadata-topic":"/t"}})";
    const std::string head = R"({"logos.test":{"enabled":true,"registry-id":"reg",)";
    struct Refused { std::string keys; std::string needle; };
    for (const Refused& r : {
             Refused{R"("epoch-size-sec":600,"max-epoch-gap":3)", "which needs epoch-size-sec 10"},
             Refused{R"("epoch-size-sec":10)", "which needs max-epoch-gap 3"},
             Refused{R"("epoch-size-sec":10,"max-epoch-gap":3,"rate-limit":100,"mix-rate-limit":20)",
                     "which needs rate-limit and mix-rate-limit to be equal"},
         }) {
        const std::string err = parseRlnPresetTable(head + r.keys + tail, table);
        LOGOS_ASSERT_TRUE(err.find("names the same registry for relay and mix") != std::string::npos);
        LOGOS_ASSERT_TRUE(err.find(r.needle) != std::string::npos);
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
             Typed{"allocator-discover", "1", "a boolean"},
             Typed{"registry-id", "7", "a string"},
             Typed{"rln-identifier", "true", "a string"},
             Typed{"mix-registry-id", "[]", "a string"},
             Typed{"mix-rln-identifier", "1", "a string"},
             Typed{"mix-metadata-topic", "null", "a string"},
             Typed{"allocator-peer-id", "1", "a string"},
             Typed{"allocator-multiaddr", "1", "a string"},
             Typed{"allocator-auth-type", "1", "a string"},
             Typed{"allocator-auth-payload", "1", "a string"},
             Typed{"allocator-auth-provider", "1", "a string"},
             Typed{"allocator-auth-args", "{}", "a string"},
             Typed{"epoch-size-sec", R"("600")", "an unsigned integer"},
             Typed{"max-epoch-gap", "-1", "an unsigned integer"},
             Typed{"rate-limit", "1.5", "an unsigned integer"},
             Typed{"mix-epoch-size-sec", R"("10")", "an unsigned integer"},
             Typed{"mix-max-epoch-gap", "true", "an unsigned integer"},
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

// A deployment whose membership tiers differ by mode: Edge nodes ask the
// peer-any allocator at the entry's rates, Core nodes the peer-allowlist one
// at higher ones. The entry itself names no allocator.
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
      "edge": { "allocator-auth-type": "peer-any" },
      "Core": { "rate-limit": 600, "mix-rate-limit": 100, "allocator-auth-type": "peer-allowlist" }
    }
  }
})";

// A mode's keys replace the entry's and the rest is inherited; the mode name
// is matched without regard to case. Each mode's entry is resolved by itself,
// so an allocator key only a mode names turns discovery on for that mode.
LOGOS_TEST(preset_table_parses_modes_over_the_entry) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_EQ(parseRlnPresetTable(kRlnPresetTableWithModes, table), std::string());
    const RlnPresetEntry& written = table["logos.test"];
    // The entry as written: no allocator, its own rates.
    LOGOS_ASSERT_FALSE(written.hasAllocator());
    LOGOS_ASSERT_EQ(written.rateLimit, static_cast<uint64_t>(100));
    LOGOS_ASSERT_EQ(written.mixRateLimit, static_cast<uint64_t>(20));
    LOGOS_ASSERT_TRUE(written.relayMembership);

    const RlnPresetEntry edge = written.forMode(RlnMode::Edge);
    LOGOS_ASSERT_TRUE(edge.enabled);
    LOGOS_ASSERT_EQ(edge.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(edge.mixRegistryId, std::string("mixreg"));
    LOGOS_ASSERT_EQ(edge.mixMetadataTopic, std::string("/mix/1/metadata/proto"));
    LOGOS_ASSERT_EQ(edge.epochSizeSec, static_cast<uint64_t>(600));
    LOGOS_ASSERT_EQ(edge.rateLimit, static_cast<uint64_t>(100));
    LOGOS_ASSERT_EQ(edge.mixRateLimit, static_cast<uint64_t>(20));
    LOGOS_ASSERT_TRUE(edge.allocatorDiscover);
    LOGOS_ASSERT_EQ(edge.allocatorAuthType, std::string("peer-any"));
    LOGOS_ASSERT_TRUE(edge.relayMembership);
    // The resolved entry carries no modes of its own.
    LOGOS_ASSERT_TRUE(edge.coreMode == nullptr);
    LOGOS_ASSERT_TRUE(edge.edgeMode == nullptr);

    const RlnPresetEntry core = written.forMode(RlnMode::Core);
    LOGOS_ASSERT_EQ(core.registryId, std::string("reg"));
    LOGOS_ASSERT_EQ(core.rateLimit, static_cast<uint64_t>(600));
    LOGOS_ASSERT_EQ(core.mixRateLimit, static_cast<uint64_t>(100));
    LOGOS_ASSERT_TRUE(core.allocatorDiscover);
    LOGOS_ASSERT_EQ(core.allocatorAuthType, std::string("peer-allowlist"));
    LOGOS_ASSERT_TRUE(core.allocatorAuthPayload.empty());
    LOGOS_ASSERT_TRUE(core.allocatorAuthProvider.empty());
    LOGOS_ASSERT_TRUE(core.coreMode == nullptr);
    LOGOS_ASSERT_TRUE(core.edgeMode == nullptr);

    // One mode named: the other runs the entry as written. An explicit
    // allocator-discover in a mode wins over the entry's allocator key, and a
    // mode can turn the Relay membership off beside a mix registry of its own.
    LOGOS_ASSERT_EQ(parseRlnPresetTable(
        R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":600,"rate-limit":100,
            "mix-registry-id":"mixreg","mix-metadata-topic":"/t","allocator-auth-type":"peer-allowlist",
            "modes":{"EDGE":{"mix-rate-limit":20,"allocator-discover":false,"relay-membership":false}}}})",
        table), std::string());
    const RlnPresetEntry onlyEdge = table["logos.test"].forMode(RlnMode::Edge);
    LOGOS_ASSERT_EQ(onlyEdge.mixRateLimit, static_cast<uint64_t>(20));
    LOGOS_ASSERT_FALSE(onlyEdge.hasAllocator());
    LOGOS_ASSERT_FALSE(onlyEdge.relayMembership);
    LOGOS_ASSERT_FALSE(onlyEdge.needsRelayMembership());
    const RlnPresetEntry otherCore = table["logos.test"].forMode(RlnMode::Core);
    LOGOS_ASSERT_EQ(otherCore.mixRateLimit, static_cast<uint64_t>(0));
    LOGOS_ASSERT_EQ(otherCore.rateLimit, static_cast<uint64_t>(100));
    LOGOS_ASSERT_TRUE(otherCore.allocatorDiscover);
    LOGOS_ASSERT_EQ(otherCore.allocatorAuthType, std::string("peer-allowlist"));
    LOGOS_ASSERT_TRUE(otherCore.relayMembership);
    LOGOS_ASSERT_TRUE(otherCore.needsRelayMembership());
    LOGOS_ASSERT_TRUE(otherCore.edgeMode == nullptr);

    // Without `modes` every mode runs the entry as written.
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTableWithAllocator, table).empty());
    const RlnPresetEntry& plain = table["logos.test"];
    LOGOS_ASSERT_TRUE(plain.coreMode == nullptr);
    LOGOS_ASSERT_TRUE(plain.edgeMode == nullptr);
    for (RlnMode mode : {RlnMode::Core, RlnMode::Edge}) {
        const RlnPresetEntry resolved = plain.forMode(mode);
        LOGOS_ASSERT_EQ(rlnBackendStartConfig(resolved), rlnBackendStartConfig(plain));
        LOGOS_ASSERT_EQ(resolved.rlnIdentifier, plain.rlnIdentifier);
        LOGOS_ASSERT_EQ(resolved.mixRlnIdentifier, plain.mixRlnIdentifier);
        LOGOS_ASSERT_EQ(resolved.mixMetadataTopic, plain.mixMetadataTopic);
        LOGOS_ASSERT_EQ(resolved.allocatorPeerId, plain.allocatorPeerId);
        LOGOS_ASSERT_EQ(resolved.enableValidation, plain.enableValidation);
        LOGOS_ASSERT_EQ(resolved.manageBackend, plain.manageBackend);
        LOGOS_ASSERT_TRUE(resolved.relayMembership);
    }
    std::string err;
    ::unsetenv(kRlnPresetsEnvVar);
    const RlnPresetEntry builtin = resolveRlnPreset("logos.test", err);
    LOGOS_ASSERT_TRUE(err.empty());
    LOGOS_ASSERT_EQ(rlnBackendStartConfig(builtin.forMode(RlnMode::Edge)), rlnBackendStartConfig(builtin));
    LOGOS_ASSERT_EQ(std::string(rlnModeName(RlnMode::Core)), std::string("Core"));
    LOGOS_ASSERT_EQ(std::string(rlnModeName(RlnMode::Edge)), std::string("Edge"));
}

// What `modes` cannot say is refused where it is written; a rule that only
// one mode's resolved entry breaks names that mode.
LOGOS_TEST(preset_table_refuses_unusable_modes) {
    std::map<std::string, RlnPresetEntry> table;
    const std::string relayOnly = R"("enabled":true,"registry-id":"reg","epoch-size-sec":600)";
    const std::string withMix = relayOnly + R"(,"mix-registry-id":"mixreg","mix-metadata-topic":"/t")";
    const std::string shared = R"("enabled":true,"registry-id":"reg","epoch-size-sec":10,"max-epoch-gap":3,)"
                               R"("mix-registry-id":"reg","mix-metadata-topic":"/t","rate-limit":50)";
    struct Refused { std::string keys; std::string error; };
    for (const Refused& r : {
             Refused{withMix + R"(,"modes":[])", "modes must be an object"},
             Refused{withMix + R"(,"modes":{"light":{}})", "modes: unknown mode \"light\""},
             Refused{withMix + R"(,"modes":{"edge":{},"Edge":{}})", "modes: mode edge is written twice"},
             Refused{withMix + R"(,"modes":{"edge":7})", "mode edge must be an object"},
             // Only the rates, the allocator and relay-membership are a mode's.
             Refused{withMix + R"(,"modes":{"edge":{"epoch-size-sec":10}})",
                     "mode edge: epoch-size-sec cannot be set in a mode"},
             // Typed like the entry's own keys.
             Refused{withMix + R"(,"modes":{"edge":{"relay-membership":"no"}})",
                     "mode edge: relay-membership must be a boolean"},
             // The entry's rules, broken by one mode's resolved entry.
             Refused{withMix + R"(,"modes":{"edge":{},"core":{"allocator-peer-id":"p"}})",
                     "mode core: needs both allocator-peer-id and allocator-multiaddr"},
             Refused{shared + R"(,"modes":{"core":{"mix-rate-limit":20}})",
                     "mode core: names the same registry for relay and mix, which needs rate-limit and"
                     " mix-rate-limit to be equal, or one of them unset"},
             // No Relay membership needs a mix registry that is not the relay one.
             Refused{relayOnly + R"(,"modes":{"edge":{"relay-membership":false}})",
                     "mode edge: relay-membership false needs a mix registry of its own"},
             Refused{shared + R"(,"modes":{"Edge":{"relay-membership":false}})",
                     "mode edge: relay-membership false needs a mix registry of its own"},
             // And it is a mode's key only.
             Refused{withMix + R"(,"relay-membership":false)",
                     "relay-membership can be set only in a mode, under modes"},
         }) {
        const std::string err = parseRlnPresetTable(R"({"logos.test":{)" + r.keys + "}}", table);
        LOGOS_ASSERT_EQ(err, "preset \"logos.test\" " + r.error);
    }

    // The same keys pass where the rule holds.
    LOGOS_ASSERT_EQ(parseRlnPresetTable(
        R"({"logos.test":{)" + withMix + R"(,"modes":{"edge":{"relay-membership":false},"core":{"relay-membership":true}}}})",
        table), std::string());
    LOGOS_ASSERT_FALSE(table["logos.test"].forMode(RlnMode::Edge).relayMembership);
    LOGOS_ASSERT_TRUE(table["logos.test"].forMode(RlnMode::Core).relayMembership);
    LOGOS_ASSERT_EQ(parseRlnPresetTable(
        R"({"logos.test":{)" + shared + R"(,"modes":{"core":{"mix-rate-limit":50},"edge":{"rate-limit":20}}}})",
        table), std::string());
    LOGOS_ASSERT_EQ(table["logos.test"].forMode(RlnMode::Edge).sharedRateLimit(), static_cast<uint64_t>(20));
}

// A vector of the `peer` scheme signs nothing, so it needs no payload and no
// producer, in an entry or in a mode; any other scheme still does.
LOGOS_TEST(a_peer_auth_vector_needs_no_payload) {
    std::map<std::string, RlnPresetEntry> table;
    const std::string base = R"("enabled":true,"registry-id":"reg","epoch-size-sec":600)";
    LOGOS_ASSERT_EQ(parseRlnPresetTable(
        R"({"logos.test":{)" + base + R"(,"allocator-auth-type":"peer-allowlist"}})", table), std::string());
    const RlnPresetEntry& e = table["logos.test"];
    LOGOS_ASSERT_TRUE(e.allocatorDiscover);
    LOGOS_ASSERT_EQ(e.allocatorAuthType, std::string("peer-allowlist"));

    for (const char* type : {"eth-allowlist", "peerless", "peer"}) {
        const std::string err = parseRlnPresetTable(
            R"({"logos.test":{)" + base + R"(,"allocator-auth-type":")" + type + R"("}})", table);
        LOGOS_ASSERT_EQ(err, std::string("preset \"logos.test\" allocator-auth-type ") + type
                                 + " needs allocator-auth-payload or allocator-auth-provider");
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

// A deployment whose Edge nodes take the mix membership alone.
static constexpr const char* kRlnPresetTableEdgeWithoutRelay = R"({
  "logos.test": {
    "enabled": true,
    "registry-id": "reg",
    "epoch-size-sec": 600,
    "max-epoch-gap": 2,
    "rate-limit": 100,
    "mix-registry-id": "mixreg",
    "mix-rate-limit": 100,
    "mix-metadata-topic": "/mix/1/metadata/proto",
    "modes": {
      "edge": { "mix-rate-limit": 20, "relay-membership": false }
    }
  }
})";

// Without a Relay membership the start names the mix registry alone: no relay
// entry and no rate for it. The top-level epoch size and gap stay, as the
// module's defaults for a registry no call has named.
LOGOS_TEST(the_backend_start_config_without_a_relay_membership_names_only_mix) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_EQ(parseRlnPresetTable(kRlnPresetTableEdgeWithoutRelay, table), std::string());

    auto edge = nlohmann::json::parse(rlnBackendStartConfig(table["logos.test"].forMode(RlnMode::Edge)));
    const nlohmann::json mixOnly{
        {"registry_id", "mixreg"}, {"epoch_size_sec", 10}, {"max_epoch_gap", 3}, {"rate_limit", 20}};
    LOGOS_ASSERT_EQ(edge.size(), static_cast<size_t>(3));
    LOGOS_ASSERT_EQ(edge.value("epoch_size_sec", 0), 600);
    LOGOS_ASSERT_EQ(edge.value("max_epoch_gap", 0), 2);
    LOGOS_ASSERT_FALSE(edge.contains("rate_limit"));
    LOGOS_ASSERT_EQ(edge["registries"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_TRUE(edge["registries"][0] == mixOnly);

    // With an allocator the one entry carries the block.
    DeliveryRlnConfig allocated = table["logos.test"].forMode(RlnMode::Edge);
    allocated.allocatorDiscover = true;
    allocated.allocatorAuthType = "peer-any";
    auto viaAllocator = nlohmann::json::parse(rlnBackendStartConfig(allocated));
    LOGOS_ASSERT_EQ(viaAllocator["registries"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(viaAllocator["registries"][0]["registry_id"].get<std::string>(), std::string("mixreg"));
    LOGOS_ASSERT_TRUE(viaAllocator["registries"][0]["allocator"]
                      == nlohmann::json({{"discover", true}, {"auth_type", "peer-any"}}));

    // The flag alone, with no mix registry to prove on, changes nothing.
    DeliveryRlnConfig relayOnly;
    relayOnly.enabled = true;
    relayOnly.registryId = "reg";
    relayOnly.epochSizeSec = 600;
    relayOnly.relayMembership = false;
    auto only = nlohmann::json::parse(rlnBackendStartConfig(relayOnly));
    LOGOS_ASSERT_EQ(only["registries"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(only["registries"][0].get<std::string>(), std::string("reg"));
}

// One start() names both registries: the relay one as this call's defaults,
// the mix one as an object with its own epoch, gap and rate. Pinned as JSON
// because the RLN module parses exactly this shape.
LOGOS_TEST(the_backend_start_config_names_both_registries) {
    DeliveryRlnConfig cfg;
    cfg.enabled = true;
    cfg.registryId = "reg";
    cfg.epochSizeSec = 600;
    cfg.rateLimit = 100;
    cfg.mixRegistryId = "mixreg";
    cfg.mixEpochSizeSec = 10;
    cfg.mixMaxEpochGap = 3;
    cfg.mixRateLimit = 20;

    auto start = nlohmann::json::parse(rlnBackendStartConfig(cfg));
    LOGOS_ASSERT_EQ(start.value("epoch_size_sec", 0), 600);
    LOGOS_ASSERT_EQ(start.value("rate_limit", 0), 100);
    LOGOS_ASSERT_FALSE(start.contains("max_epoch_gap"));
    LOGOS_ASSERT_EQ(start["registries"].size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(start["registries"][0].get<std::string>(), std::string("reg"));
    const nlohmann::json expectedMix{
        {"registry_id", "mixreg"}, {"epoch_size_sec", 10}, {"max_epoch_gap", 3}, {"rate_limit", 20}};
    LOGOS_ASSERT_TRUE(start["registries"][1] == expectedMix);

    // Relay only, no rate: the module's own defaults, one plain entry.
    DeliveryRlnConfig relay;
    relay.enabled = true;
    relay.registryId = "reg";
    relay.epochSizeSec = 600;
    relay.maxEpochGap = 2;
    auto only = nlohmann::json::parse(rlnBackendStartConfig(relay));
    LOGOS_ASSERT_EQ(only.value("max_epoch_gap", 0), 2);
    LOGOS_ASSERT_FALSE(only.contains("rate_limit"));
    LOGOS_ASSERT_EQ(only["registries"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_TRUE(only["registries"][0].is_string());
}

// A registry relay and mix share is named once: the RLN module keeps the last
// entry for a registry_id, so a second one would replace the relay values.
LOGOS_TEST(the_backend_start_config_names_a_shared_registry_once) {
    DeliveryRlnConfig cfg;
    cfg.enabled = true;
    cfg.registryId = "reg";
    cfg.epochSizeSec = 10;
    cfg.maxEpochGap = 3;
    cfg.mixRegistryId = "reg";
    cfg.mixRateLimit = 20;

    // The rate only mix names is the registry's rate.
    auto start = nlohmann::json::parse(rlnBackendStartConfig(cfg));
    LOGOS_ASSERT_TRUE(start == nlohmann::json({{"epoch_size_sec", 10},
                                               {"max_epoch_gap", 3},
                                               {"rate_limit", 20},
                                               {"registries", nlohmann::json::array({"reg"})}}));

    // No rate at all: the module's default.
    cfg.mixRateLimit = 0;
    auto unrated = nlohmann::json::parse(rlnBackendStartConfig(cfg));
    LOGOS_ASSERT_FALSE(unrated.contains("rate_limit"));
    LOGOS_ASSERT_EQ(unrated["registries"].size(), static_cast<size_t>(1));
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
        // mix off (or absent): the library is told nothing about mix.
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(kRlnNodeCfg).success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_FALSE(libCfg.contains("mix-rln-registry-id"));
        LOGOS_ASSERT_FALSE(libCfg.contains("mix-rln-identifier-hex"));
        // Nor does the node take a membership on the mix registry: its RLN
        // state is that of a deployment without one.
        const auto state = impl.rlnState().value;
        LOGOS_ASSERT_EQ(state.value("registryId", ""), std::string("reg"));
        LOGOS_ASSERT_EQ(state.value("rateLimit", 0), 100);
        LOGOS_ASSERT_FALSE(state.contains("mixRegistryId"));
        LOGOS_ASSERT_FALSE(state.contains("mixRateLimit"));
        (void)settledRlnState(impl);
    }
}

// A node is provisioned for the mix registry only when its config turns Mix
// on, by the same rule that hands it the mix keys: a relay-only node of a mix
// deployment costs its payer, or the allocator, no second registration.
LOGOS_TEST(a_mix_registry_is_provisioned_only_for_a_node_that_runs_mix) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTableWithMix);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    const auto mixRegistryOf = [](const char* cfg) -> std::string {
        DeliveryModuleImpl impl;
        if (const auto created = impl.createNode(cfg); !created.success) {
            return "createNode failed: " + created.error;
        }
        const std::string registry = impl.rlnState().value.value("mixRegistryId", "");
        (void)settledRlnState(impl);
        delivery_test_rln::resetRlnMockState();
        return registry;
    };
    // Mix on, in each config shape's own way.
    for (const char* cfg : {
             R"({"logLevel":"INFO","preset":"logos.test","mix":true})",
             R"({"logLevel":"INFO","preset":"logos.test","anonymityLevel":"Required"})",
             R"({"mode":"Core","preset":"logos.test","messagingOverrides":{"anonymityLevel":"Preferred"}})",
             R"({"entryLayer":"kernel","preset":"logos.test","kernelConf":{"logLevel":"INFO","mix":true}})",
         }) {
        LOGOS_ASSERT_EQ(mixRegistryOf(cfg), std::string("mixreg"));
    }
    // Mix off: absent, false, a level of None, or a key the shape does not read.
    for (const char* cfg : {
             R"({"logLevel":"INFO","preset":"logos.test"})",
             R"({"logLevel":"INFO","preset":"logos.test","mix":false})",
             R"({"logLevel":"INFO","preset":"logos.test","anonymityLevel":"None"})",
             R"({"mode":"Core","preset":"logos.test"})",
             R"({"mode":"Core","preset":"logos.test","messagingOverrides":{"anonymityLevel":"None"}})",
             R"({"entryLayer":"kernel","preset":"logos.test","kernelConf":{"logLevel":"INFO","anonymityLevel":"Required"}})",
         }) {
        LOGOS_ASSERT_EQ(mixRegistryOf(cfg), std::string());
    }
}

// The entry a relay-only node is provisioned with starts the backend for the
// relay registry alone, with the deployment's allocator.
LOGOS_TEST(an_entry_without_mix_starts_the_backend_for_the_relay_registry_alone) {
    std::map<std::string, RlnPresetEntry> table;
    LOGOS_ASSERT_TRUE(parseRlnPresetTable(kRlnPresetTableWithAllocator, table).empty());
    const RlnPresetEntry entry = table["logos.test"];
    LOGOS_ASSERT_TRUE(entry.hasMix());

    const RlnPresetEntry relayOnly = entry.withoutMix();
    LOGOS_ASSERT_FALSE(relayOnly.hasMix());
    LOGOS_ASSERT_TRUE(relayOnly.needsRelayMembership());
    LOGOS_ASSERT_TRUE(relayOnly.hasAllocator());
    LOGOS_ASSERT_EQ(relayOnly.registryId, entry.registryId);
    LOGOS_ASSERT_EQ(relayOnly.rateLimit, entry.rateLimit);

    const auto start = nlohmann::json::parse(rlnBackendStartConfig(relayOnly));
    LOGOS_ASSERT_EQ(start["registries"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(start["registries"][0].value("registry_id", ""), entry.registryId);
    LOGOS_ASSERT_TRUE(start["registries"][0].contains("allocator"));

    // A registry relay and mix share: one membership either way, asked at the
    // relay rate once the node does not run Mix.
    RlnPresetEntry shared;
    shared.enabled = true;
    shared.registryId = "reg";
    shared.mixRegistryId = "reg";
    shared.epochSizeSec = 10;
    shared.maxEpochGap = 3;
    shared.mixRateLimit = 20;
    LOGOS_ASSERT_EQ(nlohmann::json::parse(rlnBackendStartConfig(shared)).value("rate_limit", 0), 20);
    const auto sharedStart = nlohmann::json::parse(rlnBackendStartConfig(shared.withoutMix()));
    LOGOS_ASSERT_EQ(sharedStart["registries"].size(), static_cast<size_t>(1));
    LOGOS_ASSERT_FALSE(sharedStart.contains("rate_limit"));
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
             // No mix registry in the preset: only its relay registry, and
             // only at Mix's epoch size and a written gap of 3.
             Refused{R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":10,"max-epoch-gap":3}})",
                     R"({"preset":"logos.test","mix":true,"mix-rln-registry-id":"other"})",
                     "is not a registry of the preset"},
             Refused{kRlnPresetTable,
                     R"({"preset":"logos.test","mix":true,"mix-rln-registry-id":"reg"})",
                     "only at epoch-size-sec 10 and max-epoch-gap 3"},
             Refused{R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":10}})",
                     R"({"preset":"logos.test","mix":true,"mix-rln-registry-id":"reg"})",
                     "max-epoch-gap unset"},
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

// A preset with relay keys only, at Mix's epoch size and gap, serves a Mix
// node that names the relay registry itself; with RLN off the key is not
// looked at.
LOGOS_TEST(a_caller_supplied_mix_registry_may_be_a_relay_registry_at_mix_values) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    {
        RlnPresetsFile presets(
            R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":10,"max-epoch-gap":3}})");
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"logLevel":"INFO","preset":"logos.test","mix":true,"mix-rln-registry-id":"reg"})")
            .success);
        auto libCfg = nlohmann::json::parse(delivery_test_rln::g_lastCreateConfigJson);
        LOGOS_ASSERT_EQ(libCfg.value("mix-rln-registry-id", ""), std::string("reg"));
        // The preset has no mix scope or topic to supply.
        LOGOS_ASSERT_FALSE(libCfg.contains("mix-rln-identifier-hex"));
        LOGOS_ASSERT_FALSE(libCfg.contains("mix-rln-metadata-topic"));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        // The preset's own mix registry, written out by the caller, passes.
        RlnPresetsFile presets(kRlnPresetTableWithMix);
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"preset":"logos.test","messagingOverrides":{"anonymityLevel":"Required","mix-rln-registry-id":"mixreg"}})")
            .success);
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        // logos.dev runs no RLN: nothing to check the key against, and no Mix
        // is on to use it.
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"logLevel":"INFO","preset":"logos.dev","mix-rln-registry-id":"other"})")
            .success);
    }
}

// A node that runs Mix runs Mix RLN exactly when its deployment runs RLN.
// Without Mix either is fine.
LOGOS_TEST(mix_runs_with_mix_rln_exactly_when_the_deployment_runs_rln) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    const auto outcome = [](const char* cfg) -> std::string {
        DeliveryModuleImpl impl;
        const auto created = impl.createNode(cfg);
        if (created.success && impl.rlnState().value.contains("registryId")) {
            (void)settledRlnState(impl);
        }
        delivery_test_rln::resetRlnMockState();
        return created.success ? std::string("ok") : created.error;
    };
    const auto refusedFor = [&outcome](const char* cfg, const char* why) {
        return outcome(cfg).find(why) != std::string::npos;
    };

    // No Mix: valid with or without RLN.
    LOGOS_ASSERT_EQ(outcome(R"({"logLevel":"INFO","preset":"logos.dev"})"), std::string("ok"));
    {
        RlnPresetsFile presets(kRlnPresetTable);
        LOGOS_ASSERT_EQ(outcome(kRlnNodeCfg), std::string("ok"));
    }

    // No RLN: Mix alone is valid, Mix RLN is not.
    LOGOS_ASSERT_EQ(outcome(R"({"logLevel":"INFO","preset":"logos.dev","mix":true})"),
                    std::string("ok"));
    for (const char* cfg : {
             R"({"logLevel":"INFO","preset":"logos.dev","mix":true,"mix-rln-registry-id":"reg"})",
             R"({"logLevel":"INFO","preset":"logos.dev","anonymityLevel":"Required","mixRlnRegistryId":"reg"})",
             R"({"mode":"Core","preset":"logos.dev","messagingOverrides":{"anonymityLevel":"Preferred","mix-rln-registry-id":"reg"}})",
         }) {
        LOGOS_ASSERT_TRUE(refusedFor(cfg, "the preset runs no RLN"));
    }

    // RLN without a mix registry: Mix needs the caller's Mix RLN.
    {
        RlnPresetsFile presets(kRlnPresetTable);
        for (const char* cfg : {
                 R"({"logLevel":"INFO","preset":"logos.test","mix":true})",
                 R"({"logLevel":"INFO","preset":"logos.test","anonymityLevel":"Required"})",
                 R"({"mode":"Core","preset":"logos.test","messagingOverrides":{"anonymityLevel":"Preferred"}})",
                 R"({"entryLayer":"kernel","preset":"logos.test","kernelConf":{"logLevel":"INFO","mix":true}})",
             }) {
            LOGOS_ASSERT_TRUE(refusedFor(cfg, "Mix needs Mix RLN"));
        }
    }
    // The relay registry at Mix's values may serve Mix RLN, named by the
    // caller: where the shape takes node keys. A key in another object is not
    // the node's Mix RLN.
    {
        RlnPresetsFile presets(
            R"({"logos.test":{"enabled":true,"registry-id":"reg","epoch-size-sec":10,"max-epoch-gap":3}})");
        LOGOS_ASSERT_EQ(
            outcome(R"({"mode":"Core","preset":"logos.test","messagingOverrides":{"anonymityLevel":"Required","mix-rln-registry-id":"reg"}})"),
            std::string("ok"));
        for (const char* cfg : {
                 R"({"logLevel":"INFO","preset":"logos.test","mix":true,"messagingOverrides":{"mix-rln-registry-id":"reg"}})",
                 R"({"entryLayer":"kernel","preset":"logos.test","mix-rln-registry-id":"reg","kernelConf":{"logLevel":"INFO","mix":true}})",
             }) {
            LOGOS_ASSERT_TRUE(refusedFor(cfg, "Mix needs Mix RLN"));
        }
    }
    // RLN with a mix registry: the preset supplies Mix RLN.
    {
        RlnPresetsFile presets(kRlnPresetTableWithMix);
        LOGOS_ASSERT_EQ(outcome(R"({"logLevel":"INFO","preset":"logos.test","mix":true})"),
                        std::string("ok"));
    }
}

// createNode resolves the preset for the config's mode: the node's RLN state
// names the mode, the rates it asks at and that mode's allocator.
LOGOS_TEST(createNode_resolves_the_preset_for_the_config_mode) {
    auto t = LogosTestContext("delivery_module");
    delivery_test_rln::resetRlnMockState();
    RlnPresetsFile presets(kRlnPresetTableWithModes);
    t.mockCFunction("logosdelivery_ctx_create").returns(1);

    // Both nodes run Mix: a node that does not is asked no mix rate.
    {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"mode":"Edge","preset":"logos.test","messagingOverrides":{"anonymityLevel":"Required"}})")
            .success);
        const auto state = impl.rlnState().value;
        LOGOS_ASSERT_EQ(state.value("mode", ""), std::string("Edge"));
        LOGOS_ASSERT_EQ(state.value("rateLimit", 0), 100);
        LOGOS_ASSERT_EQ(state.value("mixRateLimit", 0), 20);
        LOGOS_ASSERT_EQ(state.value("allocatorAuthType", ""), std::string("peer-any"));
        LOGOS_ASSERT_TRUE(state.value("allocatorDiscover", false));
        LOGOS_ASSERT_FALSE(state.contains("relayMembership"));
        LOGOS_ASSERT_EQ(state.value("registryId", ""), std::string("reg"));
        LOGOS_ASSERT_EQ(state.value("mixRegistryId", ""), std::string("mixreg"));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(
            R"({"mode":"Core","preset":"logos.test","messagingOverrides":{"anonymityLevel":"Required"}})")
            .success);
        const auto state = impl.rlnState().value;
        LOGOS_ASSERT_EQ(state.value("mode", ""), std::string("Core"));
        LOGOS_ASSERT_EQ(state.value("rateLimit", 0), 600);
        LOGOS_ASSERT_EQ(state.value("mixRateLimit", 0), 100);
        LOGOS_ASSERT_EQ(state.value("allocatorAuthType", ""), std::string("peer-allowlist"));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
}

// A rate the preset leaves to the RLN module is not reported, a preset
// without modes still names the node's mode, and a node without a Relay
// membership says so instead of a relay rate.
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
        LOGOS_ASSERT_FALSE(state.contains("rateLimit"));
        LOGOS_ASSERT_FALSE(state.contains("mixRateLimit"));
        LOGOS_ASSERT_FALSE(state.contains("relayMembership"));
        (void)settledRlnState(impl);
    }
    delivery_test_rln::resetRlnMockState();
    {
        RlnPresetsFile presets(kRlnPresetTableEdgeWithoutRelay);
        {
            // Without a Relay membership and without Mix the node would hold
            // no membership at all: refused, naming the preset and the mode.
            DeliveryModuleImpl impl;
            const auto refused = impl.createNode(R"({"mode":"Edge","preset":"logos.test"})");
            LOGOS_ASSERT_FALSE(refused.success);
            LOGOS_ASSERT_TRUE(refused.error.find("\"logos.test\"") != std::string::npos);
            LOGOS_ASSERT_TRUE(refused.error.find("Edge") != std::string::npos);
            LOGOS_ASSERT_TRUE(refused.error.find("relay-membership") != std::string::npos);
        }
        delivery_test_rln::resetRlnMockState();
        {
            DeliveryModuleImpl impl;
            LOGOS_ASSERT_TRUE(impl.createNode(
                R"({"mode":"Edge","preset":"logos.test","messagingOverrides":{"anonymityLevel":"Required"}})")
                .success);
            const auto state = impl.rlnState().value;
            LOGOS_ASSERT_EQ(state.value("mode", ""), std::string("Edge"));
            LOGOS_ASSERT_TRUE(state.contains("relayMembership"));
            LOGOS_ASSERT_FALSE(state.value("relayMembership", true));
            LOGOS_ASSERT_FALSE(state.contains("rateLimit"));
            LOGOS_ASSERT_EQ(state.value("mixRateLimit", 0), 20);
            (void)settledRlnState(impl);
        }
        delivery_test_rln::resetRlnMockState();
        {
            DeliveryModuleImpl impl;
            LOGOS_ASSERT_TRUE(impl.createNode(
                R"({"mode":"Core","preset":"logos.test","messagingOverrides":{"anonymityLevel":"Required"}})")
                .success);
            const auto state = impl.rlnState().value;
            LOGOS_ASSERT_EQ(state.value("mode", ""), std::string("Core"));
            LOGOS_ASSERT_FALSE(state.contains("relayMembership"));
            LOGOS_ASSERT_EQ(state.value("rateLimit", 0), 100);
            LOGOS_ASSERT_EQ(state.value("mixRateLimit", 0), 100);
            (void)settledRlnState(impl);
        }
    }
    delivery_test_rln::resetRlnMockState();
    {
        // No RLN: nothing but the state.
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(R"({"mode":"Edge","preset":"logos.dev"})").success);
        LOGOS_ASSERT_FALSE(impl.rlnState().value.contains("mode"));
    }
}

// An anonymity level above None mounts Mix as `mix: true` does, so it gets
// the preset's mix keys too: at the top level of a flat config, in
// messagingOverrides for a layered one, which is where that shape carries the
// level. The kernel entry ignores the messaging layer: there only `mix: true`
// inside kernelConf turns the keys on.
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
             R"({"logLevel":"INFO","preset":"logos.test","anonymityLevel":2})",
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
             R"({"mode":"Edge","preset":"logos.test","messagingOverrides":{}})",
             R"({"mode":"Edge","preset":"logos.test"})",
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
            R"({"entryLayer":"kernel","preset":"logos.test","kernelConf":{"logLevel":"INFO"}})")
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

// Without the plugin libp2p joins the deployment's DHT all the same: the
// allocator path looks allocators up on it.
LOGOS_TEST(discovery_from_requirements_disabled_means_no_plugin) {
    delivery_discovery::PluginRequest req;
    LOGOS_ASSERT_TRUE(delivery_discovery::fromRequirements(kDisabledReply, nlohmann::json::object(), req).empty());
    LOGOS_ASSERT_FALSE(req.enabled);
    const auto libp2p = nlohmann::json::parse(req.libp2pConfig);
    LOGOS_ASSERT_TRUE(libp2p["mountServiceDiscovery"].get<bool>());
    LOGOS_ASSERT_TRUE(libp2p["bootstrapNodes"].empty());

    // The node's DHT peers are the bootstrap nodes.
    LOGOS_ASSERT_TRUE(delivery_discovery::fromRequirements(
        R"({"externalServiceDiscovery":false,"bootstrapNodes":[
            "/ip4/10.0.0.1/tcp/30303/p2p/16Uiu2HAmA",
            "/ip4/10.0.0.2/tcp/30303/p2p/16Uiu2HAmB"]})",
        nlohmann::json::object(), req).empty());
    LOGOS_ASSERT_FALSE(req.requested);
    LOGOS_ASSERT_FALSE(req.enabled);
    const auto named = nlohmann::json::parse(req.libp2pConfig);
    LOGOS_ASSERT_TRUE(named["mountKad"].get<bool>());
    LOGOS_ASSERT_TRUE(named["mountServiceDiscovery"].get<bool>());
    LOGOS_ASSERT_EQ(named["bootstrapNodes"].size(), size_t{2});
    LOGOS_ASSERT_EQ(named["bootstrapNodes"][0]["peerId"].get<std::string>(), std::string("16Uiu2HAmA"));
    LOGOS_ASSERT_EQ(named["bootstrapNodes"][1]["peerId"].get<std::string>(), std::string("16Uiu2HAmB"));

    // When the node names none, a preset's allocator-bootstrap-nodes are.
    LOGOS_ASSERT_TRUE(delivery_discovery::fromRequirements(
        kDisabledReply, nlohmann::json::object(), req,
        {"/ip4/10.0.0.1/tcp/30303/p2p/16Uiu2HAmA", "/ip4/10.0.0.2/tcp/30303/p2p/16Uiu2HAmB"}).empty());
    const auto preset = nlohmann::json::parse(req.libp2pConfig);
    LOGOS_ASSERT_TRUE(preset["mountKad"].get<bool>());
    LOGOS_ASSERT_TRUE(preset["mountServiceDiscovery"].get<bool>());
    LOGOS_ASSERT_EQ(preset["bootstrapNodes"].size(), size_t{2});
    LOGOS_ASSERT_EQ(preset["bootstrapNodes"][0]["peerId"].get<std::string>(), std::string("16Uiu2HAmA"));
    LOGOS_ASSERT_EQ(preset["bootstrapNodes"][1]["addrs"][0].get<std::string>(), std::string("/ip4/10.0.0.2/tcp/30303"));
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
    // The node decides the mounts and leads the DHT peers; the operator's
    // peers it does not name follow, and everything else in libp2p's own
    // config survives.
    delivery_discovery::PluginRequest req;
    const auto base = parseJson(R"({"addrs":["/ip4/0.0.0.0/tcp/9000"],"transport":"tcp",
        "mountKad":false,"bootstrapNodes":[{"peerId":"own","addrs":["/ip4/1.1.1.1/tcp/1"]},
        {"peerId":"16Uiu2HAmB","addrs":["/ip4/9.9.9.9/tcp/1"]}]})");
    LOGOS_ASSERT_TRUE(delivery_discovery::fromRequirements(kEnabledReply, base, req).empty());
    const auto libp2p = nlohmann::json::parse(req.libp2pConfig);
    LOGOS_ASSERT_EQ(libp2p["addrs"][0].get<std::string>(), std::string("/ip4/0.0.0.0/tcp/9000"));
    LOGOS_ASSERT_EQ(libp2p["transport"].get<std::string>(), std::string("tcp"));
    LOGOS_ASSERT_TRUE(libp2p["mountKad"].get<bool>());
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"].size(), size_t{3});
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"][0]["peerId"].get<std::string>(), std::string("16Uiu2HAmA"));
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"][1]["addrs"][0].get<std::string>(), std::string("/ip4/10.0.0.2/tcp/30303"));
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"][2]["peerId"].get<std::string>(), std::string("own"));
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

// A requirements reply that cannot be read leaves an empty libp2p config; the
// allocator path then takes one built from the preset's peers alone, with the
// discovery mounts on and libp2p's own config underneath, and no plugin.
LOGOS_TEST(discovery_without_requirements_joins_the_preset_peers) {
    ScopedLibp2pEnv env(R"({"addrs":["/ip4/0.0.0.0/tcp/9100"],"mountKad":false,
        "bootstrapNodes":[{"peerId":"16Uiu2HAmOwn","addrs":["/ip4/10.0.0.9/tcp/1"]},
                          {"peerId":"16Uiu2HAmB","addrs":["/ip4/9.9.9.9/tcp/1"]}]})");
    const std::vector<std::string> presetPeers = {
        "/ip4/10.0.0.1/tcp/30303/p2p/16Uiu2HAmA", "/ip4/10.0.0.2/tcp/30303/p2p/16Uiu2HAmB"};
    delivery_discovery::PluginRequest req;
    LOGOS_ASSERT_FALSE(delivery_discovery::fromRequirements(
        "nonsense", delivery_discovery::libp2pEnvConfig(), req, presetPeers).empty());
    LOGOS_ASSERT_EQ(req.libp2pConfig, std::string("{}"));

    LOGOS_ASSERT_TRUE(delivery_discovery::withoutRequirements(
        delivery_discovery::libp2pEnvConfig(), req, presetPeers).empty());
    LOGOS_ASSERT_FALSE(req.requested);
    LOGOS_ASSERT_FALSE(req.enabled);
    const auto libp2p = nlohmann::json::parse(req.libp2pConfig);
    LOGOS_ASSERT_TRUE(libp2p["mountKad"].get<bool>());
    LOGOS_ASSERT_TRUE(libp2p["mountServiceDiscovery"].get<bool>());
    LOGOS_ASSERT_EQ(libp2p["addrs"][0].get<std::string>(), std::string("/ip4/0.0.0.0/tcp/9100"));
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"].size(), size_t{3});
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"][0]["peerId"].get<std::string>(), std::string("16Uiu2HAmA"));
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"][1]["addrs"][0].get<std::string>(), std::string("/ip4/10.0.0.2/tcp/30303"));
    LOGOS_ASSERT_EQ(libp2p["bootstrapNodes"][2]["peerId"].get<std::string>(), std::string("16Uiu2HAmOwn"));

    // No peers: the mounts alone.
    LOGOS_ASSERT_TRUE(delivery_discovery::withoutRequirements(nlohmann::json::object(), req, {}).empty());
    const auto bare = nlohmann::json::parse(req.libp2pConfig);
    LOGOS_ASSERT_TRUE(bare["mountKad"].get<bool>());
    LOGOS_ASSERT_TRUE(bare["mountServiceDiscovery"].get<bool>());
    LOGOS_ASSERT_TRUE(bare["bootstrapNodes"].empty());

    // A peer that is no /p2p/ multiaddr is the failure, with nothing built.
    LOGOS_ASSERT_FALSE(delivery_discovery::withoutRequirements(
        nlohmann::json::object(), req, {"/ip4/10.0.0.1/tcp/1"}).empty());
    LOGOS_ASSERT_EQ(req.libp2pConfig, std::string("{}"));
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

// The membership read-back, driven a round at a time on the test's thread: a
// DeliveryModuleImpl with no node, so no worker, and a stub client behind its
// bridge. DeliveryModuleImpl names this struct a friend.
struct DeliveryModuleReadBackTestAccess {
    RlnModule client{"delivery_module"};
    DeliveryModuleImpl impl;

    DeliveryModuleReadBackTestAccess() {
        rln_module_stub::resetStubError();
        delivery_test_rln::resetRlnMockState();
        delivery_test_events::resetRlnStateEvent();
    }
    ~DeliveryModuleReadBackTestAccess() { rln_module_stub::resetStubError(); }

    // Installs `cfg` and takes the steps bringRlnUp takes around a backend
    // start that succeeded.
    void begin(const DeliveryRlnConfig& cfg) {
        impl.rlnBridge->init(&client);
        (void)impl.rlnBridge->enable();
        auto next = std::make_shared<DeliveryRlnConfig>(cfg);
        next->enabled = true;
        {
            std::lock_guard<std::mutex> lock(impl.rlnConfigMutex);
            impl.rlnConfig = std::move(next);
        }
        {
            std::lock_guard<std::mutex> lock(impl.rlnStateMutex);
            impl.rlnStateConfig = *impl.rlnConfig;
        }
        impl.resetMembershipReadBack();
        impl.rlnReady = true;
        impl.setRlnState(RlnState::Ready, {});
        impl.startMembershipWatch();
    }
    // What the worker runs on a tick. False when the watch owes no read, as
    // the worker then waits untimed.
    bool round() {
        if (!impl.membershipWatchDue()) {
            return false;
        }
        impl.readRlnMemberships();
        return true;
    }
    long long quietSeconds() {
        return static_cast<long long>(impl.membershipWatch.interval.count());
    }
    nlohmann::json state() { return impl.rlnState().value; }
};

// Relay on "reg", mix on "mixreg", both asked at 100.
static DeliveryRlnConfig readBackConfig() {
    DeliveryRlnConfig cfg;
    cfg.enabled = true;
    cfg.registryId = "reg";
    cfg.rlnIdentifier = "rln-id";
    cfg.epochSizeSec = 600;
    cfg.rateLimit = 100;
    cfg.mixRegistryId = "mixreg";
    cfg.mixRlnIdentifier = "mix-id";
    cfg.mixRateLimit = 100;
    return cfg;
}

LOGOS_TEST(the_read_back_reports_each_registrys_membership) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleReadBackTestAccess f;
    f.begin(readBackConfig());
    LOGOS_ASSERT_EQ(f.state().value("state", ""), std::string("Ready"));
    // Nothing before the first round.
    LOGOS_ASSERT_FALSE(f.state().contains("membership"));
    LOGOS_ASSERT_FALSE(f.state().contains("mixMembership"));

    auto& replies = rln_module_stub::membershipStateReplies();
    replies["reg"] =
        R"({"registry_id":"reg","state":"unknown","rate_limit":100,"provisioning":{"step":"delegating"}})";
    replies["mixreg"] = R"({"registry_id":"mixreg","state":"active","rate_limit":100})";
    LOGOS_ASSERT_TRUE(f.round());

    auto state = f.state();
    LOGOS_ASSERT_EQ(state["membership"].dump(),
                    std::string(R"({"state":"unknown","step":"delegating"})"));
    LOGOS_ASSERT_FALSE(state.contains("grantedRateLimit"));
    LOGOS_ASSERT_EQ(state["mixMembership"].dump(), std::string(R"({"state":"active"})"));
    LOGOS_ASSERT_EQ(state.value("grantedMixRateLimit", 0), 100);
    // Granted as asked: Ready stays as it was.
    LOGOS_ASSERT_EQ(state.value("state", ""), std::string("Ready"));
    LOGOS_ASSERT_EQ(state.value("message", "x"), std::string());
    LOGOS_ASSERT_TRUE(rln_module_stub::membershipStateCalls()
                      == (std::vector<std::string>{"reg", "mixreg"}));

    // An active registry is not read again; the other one is.
    replies["reg"] = R"({"registry_id":"reg","state":"active","rate_limit":100})";
    LOGOS_ASSERT_TRUE(f.round());
    state = f.state();
    LOGOS_ASSERT_EQ(state["membership"].dump(), std::string(R"({"state":"active"})"));
    LOGOS_ASSERT_EQ(state.value("grantedRateLimit", 0), 100);
    LOGOS_ASSERT_TRUE(rln_module_stub::membershipStateCalls()
                      == (std::vector<std::string>{"reg", "mixreg", "reg"}));

    // Both active: the watch is over.
    LOGOS_ASSERT_FALSE(f.round());
    LOGOS_ASSERT_EQ(rln_module_stub::membershipStateCalls().size(), static_cast<size_t>(3));
}

LOGOS_TEST(the_read_back_reports_a_shared_registry_under_both_names) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleReadBackTestAccess f;
    DeliveryRlnConfig cfg = readBackConfig();
    cfg.epochSizeSec = 10;
    cfg.maxEpochGap = 3;
    cfg.mixRegistryId = "reg";
    f.begin(cfg);

    rln_module_stub::membershipStateReplies()["reg"] = R"({"state":"active","rate_limit":20})";
    LOGOS_ASSERT_TRUE(f.round());

    const auto state = f.state();
    LOGOS_ASSERT_EQ(state["membership"].dump(), std::string(R"({"state":"active"})"));
    LOGOS_ASSERT_EQ(state["mixMembership"].dump(), std::string(R"({"state":"active"})"));
    LOGOS_ASSERT_EQ(state.value("grantedRateLimit", 0), 20);
    LOGOS_ASSERT_EQ(state.value("grantedMixRateLimit", 0), 20);
    // One membership, one read, one message for the capped grant.
    LOGOS_ASSERT_TRUE(rln_module_stub::membershipStateCalls() == (std::vector<std::string>{"reg"}));
    LOGOS_ASSERT_FALSE(state.value("message", "").empty());
    LOGOS_ASSERT_FALSE(f.round());
}

LOGOS_TEST(the_read_back_without_a_relay_membership_reads_only_mix) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleReadBackTestAccess f;
    DeliveryRlnConfig cfg = readBackConfig();
    cfg.relayMembership = false;
    f.begin(cfg);

    rln_module_stub::membershipStateReply() = R"({"state":"pending","rate_limit":100})";
    LOGOS_ASSERT_TRUE(f.round());

    const auto state = f.state();
    LOGOS_ASSERT_FALSE(state.contains("membership"));
    LOGOS_ASSERT_FALSE(state.contains("grantedRateLimit"));
    LOGOS_ASSERT_EQ(state["mixMembership"].dump(), std::string(R"({"state":"pending"})"));
    LOGOS_ASSERT_TRUE(rln_module_stub::membershipStateCalls()
                      == (std::vector<std::string>{"mixreg"}));
}

LOGOS_TEST(a_capped_grant_republishes_ready_with_the_message) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleReadBackTestAccess f;
    f.begin(readBackConfig());
    const int afterReady = delivery_test_events::lastRlnState().transitions;
    LOGOS_ASSERT_EQ(delivery_test_events::lastRlnState().state, std::string("Ready"));

    auto& replies = rln_module_stub::membershipStateReplies();
    replies["reg"] = R"({"state":"active","rate_limit":100})";
    replies["mixreg"] = R"({"state":"pending","rate_limit":100})";
    LOGOS_ASSERT_TRUE(f.round());
    // Nothing differs yet: no event.
    LOGOS_ASSERT_EQ(delivery_test_events::lastRlnState().transitions, afterReady);

    replies["mixreg"] = R"({"state":"active","rate_limit":20})";
    LOGOS_ASSERT_TRUE(f.round());
    const auto event = delivery_test_events::lastRlnState();
    LOGOS_ASSERT_EQ(event.transitions, afterReady + 1);
    LOGOS_ASSERT_EQ(event.state, std::string("Ready"));
    LOGOS_ASSERT_EQ(event.message,
                    std::string("mix membership granted at rate 20; the preset asks 100"));
    const auto state = f.state();
    LOGOS_ASSERT_EQ(state.value("state", ""), std::string("Ready"));
    LOGOS_ASSERT_EQ(state.value("message", ""), event.message);
    LOGOS_ASSERT_EQ(state.value("grantedMixRateLimit", 0), 20);
    LOGOS_ASSERT_EQ(state.value("mixRateLimit", 0), 100);
}

// While rln_module does not answer, a round stops at its first read and the
// quiet before the next one doubles, up to a minute; an answer brings it back.
LOGOS_TEST(the_read_back_backs_off_while_the_module_does_not_answer) {
    auto t = LogosTestContext("delivery_module");
    DeliveryModuleReadBackTestAccess f;
    f.begin(readBackConfig());
    LOGOS_ASSERT_EQ(f.quietSeconds(), 5LL);

    // The stub's default: a call error, no host behind the client.
    const long long expected[] = {10, 20, 40, 60, 60};
    for (const long long seconds : expected) {
        LOGOS_ASSERT_TRUE(f.round());
        LOGOS_ASSERT_EQ(f.quietSeconds(), seconds);
    }
    // One read a round: the mix registry was never asked.
    LOGOS_ASSERT_TRUE(rln_module_stub::membershipStateCalls()
                      == (std::vector<std::string>(5, "reg")));
    LOGOS_ASSERT_FALSE(f.state().contains("membership"));

    // An answer that cannot be used (an in-band error) is an answer: the
    // interval is left as it is and the mix read follows in the same round.
    rln_module_stub::membershipStateReplies()["reg"] =
        R"({"error":{"class":"transient","message":"registry not synced"}})";
    LOGOS_ASSERT_TRUE(f.round());
    LOGOS_ASSERT_EQ(rln_module_stub::membershipStateCalls().back(), std::string("mixreg"));
    LOGOS_ASSERT_FALSE(f.state().contains("membership"));

    rln_module_stub::membershipStateReplies()["reg"] = R"({"state":"pending"})";
    rln_module_stub::membershipStateReplies()["mixreg"] = R"({"state":"pending"})";
    LOGOS_ASSERT_TRUE(f.round());
    LOGOS_ASSERT_EQ(f.quietSeconds(), 5LL);
    LOGOS_ASSERT_TRUE(f.state().contains("membership"));
    LOGOS_ASSERT_TRUE(f.state().contains("mixMembership"));
}

// readMembershipState: the lifecycle worker's read-back of one registry.
LOGOS_TEST(rln_bridge_reads_a_membership_state) {
    auto t = LogosTestContext("delivery_module");
    BridgeFixture f; // init()ed, not enabled
    nlohmann::json out;

    // Not enabled: refused, whatever the module would answer.
    rln_module_stub::membershipStateReply() = R"({"state":"active"})";
    LOGOS_ASSERT_EQ(f.bridge.readMembershipState("reg", "rln-id", out),
                    std::string("rln bridge is not enabled"));
    LOGOS_ASSERT_TRUE(out.is_null());
    rln_module_stub::membershipStateReply().reset();

    // Enabled, the stub's default: no host behind the client.
    LOGOS_ASSERT_EQ(f.bridge.enable(), std::string());
    LOGOS_ASSERT_FALSE(f.bridge.readMembershipState("reg", "rln-id", out).empty());
    LOGOS_ASSERT_TRUE(out.is_null());

    // An in-band error is a failure.
    rln_module_stub::membershipStateReply() =
        R"({"error":{"class":"transient","message":"registry not synced"}})";
    const std::string failure = f.bridge.readMembershipState("reg", "rln-id", out);
    LOGOS_ASSERT_TRUE(failure.find("registry not synced") != std::string::npos);
    LOGOS_ASSERT_TRUE(out.is_null());

    // So is a reply that is no JSON object.
    rln_module_stub::membershipStateReply() = "not json";
    LOGOS_ASSERT_FALSE(f.bridge.readMembershipState("reg", "rln-id", out).empty());
    LOGOS_ASSERT_TRUE(out.is_null());

    rln_module_stub::membershipStateReply() =
        R"({"registry_id":"reg","state":"active","leaf_index":3,"rate_limit":20})";
    LOGOS_ASSERT_EQ(f.bridge.readMembershipState("reg", "rln-id", out), std::string());
    LOGOS_ASSERT_EQ(out["state"].get<std::string>(), std::string("active"));
    LOGOS_ASSERT_EQ(out["rate_limit"].get<uint64_t>(), static_cast<uint64_t>(20));
    // A read for the worker: nothing is answered to the library.
    LOGOS_ASSERT_FALSE(delivery_test_rln::g_responseFired);
}

// What rlnState reports of a get_membership_state reply.
LOGOS_TEST(a_membership_reply_is_read_into_state_step_and_granted_rate) {
    // Provisioning: the state and the step, and no rate.
    const auto provisioning = readRlnMembershipReply(nlohmann::json::parse(
        R"({"registry_id":"reg","state":"unknown","provisioning":{"step":"delegating","detail":"x"}})"));
    LOGOS_ASSERT_EQ(provisioning.membership.dump(),
                    std::string(R"({"state":"unknown","step":"delegating"})"));
    LOGOS_ASSERT_FALSE(provisioning.active);
    LOGOS_ASSERT_FALSE(provisioning.grantedRate.has_value());

    // Pending: rate_limit is still the rate asked, so it is not a grant.
    const auto pending = readRlnMembershipReply(
        nlohmann::json::parse(R"({"state":"pending","rate_limit":100})"));
    LOGOS_ASSERT_EQ(pending.membership.dump(), std::string(R"({"state":"pending"})"));
    LOGOS_ASSERT_FALSE(pending.active);
    LOGOS_ASSERT_FALSE(pending.grantedRate.has_value());

    // Active: the chain's rate.
    const auto active = readRlnMembershipReply(
        nlohmann::json::parse(R"({"state":"active","rate_limit":20})"));
    LOGOS_ASSERT_EQ(active.membership.dump(), std::string(R"({"state":"active"})"));
    LOGOS_ASSERT_TRUE(active.active);
    LOGOS_ASSERT_TRUE(active.grantedRate.has_value());
    LOGOS_ASSERT_EQ(*active.grantedRate, static_cast<uint64_t>(20));

    // Active with no rate in the reply: a state, no grant.
    const auto bare = readRlnMembershipReply(nlohmann::json::parse(R"({"state":"active"})"));
    LOGOS_ASSERT_TRUE(bare.active);
    LOGOS_ASSERT_FALSE(bare.grantedRate.has_value());

    // No state to report.
    LOGOS_ASSERT_TRUE(readRlnMembershipReply(nlohmann::json::parse(R"({"rate_limit":20})"))
                          .membership.is_null());
    LOGOS_ASSERT_TRUE(readRlnMembershipReply(nlohmann::json::parse(R"("active")"))
                          .membership.is_null());
}

LOGOS_TEST(a_capped_grant_is_named_per_registry) {
    const std::optional<uint64_t> none;
    // Granted as asked, not granted yet, or asked at the module's default (0).
    LOGOS_ASSERT_EQ(rlnCappedGrantMessage(100, 100, 50, 50, false), std::string());
    LOGOS_ASSERT_EQ(rlnCappedGrantMessage(100, none, 50, none, false), std::string());
    LOGOS_ASSERT_EQ(rlnCappedGrantMessage(0, 20, 0, 5, false), std::string());

    LOGOS_ASSERT_EQ(rlnCappedGrantMessage(100, 100, 100, 20, false),
                    std::string("mix membership granted at rate 20; the preset asks 100"));
    LOGOS_ASSERT_EQ(rlnCappedGrantMessage(100, 20, 50, none, false),
                    std::string("relay membership granted at rate 20; the preset asks 100"));
    LOGOS_ASSERT_EQ(rlnCappedGrantMessage(100, 20, 50, 5, false),
                    std::string("relay membership granted at rate 20, mix membership at rate 5; "
                                "the preset asks 100 and 50"));

    // A shared registry has one membership: the relay pair, named for both.
    LOGOS_ASSERT_EQ(rlnCappedGrantMessage(100, 20, 100, none, true),
                    std::string("relay and mix membership granted at rate 20; the preset asks 100"));
    LOGOS_ASSERT_EQ(rlnCappedGrantMessage(100, 100, 100, 20, true), std::string());
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

    // enable() runs on the lifecycle worker, so the library's first read at
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
