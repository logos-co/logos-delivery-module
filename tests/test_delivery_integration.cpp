// Integration tests for DeliveryModuleImpl - uses the REAL liblogosdelivery library.
// No mocking. These tests start an actual delivery node and exercise the API
// as shown in examples/simple.cpp.
//
// Requires liblogosdelivery to be available in ../lib at build time.
// Skipped automatically when liblogosdelivery is not found.

#include <logos_test.h>
#include "delivery_module_plugin.h"
#include "mocks/delivery_module_events_stub.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// Minimal config - no preset, no network peers, Edge mode with relay+sharding
// so subscribe/send can be exercised without connecting to any external nodes.
static const char* kMinimalConfig = R"({
  "logLevel": "INFO",
  "mode": "Edge",
  "relay": true,
  "numShardsInNetwork": 8
})";

static const char* kTestTopic = "/test/2/delivery-integration/proto";

static const int DEFAULT_TIMEOUT_MS = 30000;

// start()/stop() return once the request is dispatched; completion is reported
// via the nodeStarted / nodeStopped events. These helpers block until the
// corresponding event fires (or time out), so the rest of a test can rely on
// the node actually being up/down.
static bool waitForNodeStarted(int timeoutMs = DEFAULT_TIMEOUT_MS) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (delivery_test_events::g_lastNodeStarted.fired) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return delivery_test_events::g_lastNodeStarted.fired;
}

static bool waitForNodeStopped(int timeoutMs = DEFAULT_TIMEOUT_MS) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (delivery_test_events::g_lastNodeStopped.fired) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return delivery_test_events::g_lastNodeStopped.fired;
}

// ---------------------------------------------------------------------------
// Shared impl instance - restarted before each test group.
// ---------------------------------------------------------------------------

static DeliveryModuleImpl* g_impl = nullptr;

static void ensureStarted() {
    if (g_impl) {
        g_impl->stop();
        delete g_impl;
        g_impl = nullptr;
    }

    g_impl = new DeliveryModuleImpl();

    delivery_test_events::resetNodeLifecycleEvents();

    if (!g_impl->createNode(kMinimalConfig).success) {
        delete g_impl;
        g_impl = nullptr;
        throw LogosTestFailure("Integration: failed to createNode.");
    }

    // wait for the nodeStarted event before the caller exercises
    // subscribe/send/etc. against the node.
    if (!g_impl->start().success) {
        delete g_impl;
        g_impl = nullptr;
        throw LogosTestFailure("Integration: failed to dispatch start.");
    }
    if (!waitForNodeStarted() || !delivery_test_events::g_lastNodeStarted.success) {
        delete g_impl;
        g_impl = nullptr;
        throw LogosTestFailure("Integration: node did not start (no nodeStarted event).");
    }
}

// ---------------------------------------------------------------------------
// Tests - lifecycle
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_createNode) {
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(kMinimalConfig).success);
    // Node was never started; let the destructor tear the context down via
    // logosdelivery_destroy (no stop() needed, and stop-without-start is a
    // no-op the library does not expect).
}

LOGOS_TEST(integration_createNode_with_logos_dev_preset) {
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"logLevel":"DEBUG","mode":"Core","preset":"logos.dev"})").success);
}

LOGOS_TEST(integration_start_stop) {
    delivery_test_events::resetNodeLifecycleEvents();

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(kMinimalConfig).success);

    // start() dispatches; the node is up once nodeStarted fires.
    LOGOS_ASSERT_TRUE(impl.start().success);
    LOGOS_ASSERT_TRUE(waitForNodeStarted());
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.success);

    // stop() dispatches; the node is down once nodeStopped fires.
    LOGOS_ASSERT_TRUE(impl.stop().success);
    LOGOS_ASSERT_TRUE(waitForNodeStopped());
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStopped.success);
}

// ---------------------------------------------------------------------------
// Tests - queries (mirror the simple.cpp info loop)
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_getAvailableConfigs_returns_non_empty) {
    ensureStarted();

    StdLogosResult result = g_impl->getAvailableConfigs();
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_FALSE(result.value.get<std::string>().empty());
}

LOGOS_TEST(integration_getAvailableNodeInfoIDs_returns_non_empty) {
    ensureStarted();

    StdLogosResult result = g_impl->getAvailableNodeInfoIDs();
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_FALSE(result.value.get<std::string>().empty());
}

LOGOS_TEST(integration_getNodeInfo_returns_value_for_each_id) {
    ensureStarted();

    StdLogosResult idsResult = g_impl->getAvailableNodeInfoIDs();
    LOGOS_ASSERT_TRUE(idsResult.success);

    std::string nodeInfoIDs = idsResult.value.get<std::string>();
    LOGOS_ASSERT_FALSE(nodeInfoIDs.empty());

    // Older liblogosdelivery returns the IDs as Nim repr "@[ID1, ID2, ...]",
    // newer ones as a JSON array ["ID1", "ID2", ...]. Strip either wrapper,
    // then split on comma and drop spaces/quotes.
    if (nodeInfoIDs.size() > 3 &&
        nodeInfoIDs[0] == '@' && nodeInfoIDs[1] == '[' &&
        nodeInfoIDs.back() == ']') {
        nodeInfoIDs = nodeInfoIDs.substr(2, nodeInfoIDs.size() - 3);
    } else if (nodeInfoIDs.size() > 2 &&
               nodeInfoIDs.front() == '[' && nodeInfoIDs.back() == ']') {
        nodeInfoIDs = nodeInfoIDs.substr(1, nodeInfoIDs.size() - 2);
    }

    // Split on comma
    std::vector<std::string> ids;
    std::string current;
    for (char c : nodeInfoIDs) {
        if (c == ',') {
            if (!current.empty()) ids.push_back(current);
            current.clear();
        } else if (c != ' ' && c != '"') {
            current.push_back(c);
        }
    }
    if (!current.empty()) ids.push_back(current);

    LOGOS_ASSERT_GT(static_cast<int>(ids.size()), 0);

    for (const std::string& id : ids) {
        StdLogosResult infoResult = g_impl->getNodeInfo(id);
        LOGOS_ASSERT_TRUE(infoResult.success);
        // An advertised node-info item may legitimately be empty when its
        // feature is unconfigured (e.g. MixPubKey when the node has no mix
        // key), so only require the lookup to succeed, not to be non-empty.
    }
}

// ---------------------------------------------------------------------------
// Tests - metrics (openmetrics text source)
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_collectOpenMetricsText_returns_real_exposition_text) {
    ensureStarted();

    std::string text = g_impl->collectOpenMetricsText();

    // A started node has eagerly-registered metric families in the global
    // registry, so the rendered document must be non-empty and look like
    // Prometheus/OpenMetrics exposition text — exactly what the openmetrics
    // module's collectOpenMetricsText() text source consumes and re-renders.
    LOGOS_ASSERT_FALSE(text.empty());
    LOGOS_ASSERT(text.find("# HELP") != std::string::npos);
    LOGOS_ASSERT(text.find("# TYPE") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Tests - pub/sub (as in simple.cpp)
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_subscribe_succeeds) {
    ensureStarted();
    LOGOS_ASSERT_TRUE(g_impl->subscribe(kTestTopic).success);
}

LOGOS_TEST(integration_subscribe_unsubscribe) {
    ensureStarted();

    LOGOS_ASSERT_TRUE(g_impl->subscribe(kTestTopic).success);
    LOGOS_ASSERT_TRUE(g_impl->unsubscribe(kTestTopic).success);
}

// ---------------------------------------------------------------------------
// Tests - store query
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_storeQuery_reports_error_for_invalid_peer) {
    ensureStarted();

    // No store service peer is running in this environment; the point is that
    // the real waku_store_query FFI round-trip completes and surfaces a proper
    // error result (peer parse / query failure) instead of hanging or crashing.
    StdLogosResult result = g_impl->storeQuery(
        R"({"requestId":"integration-store-1","includeData":true,"paginationForward":true})",
        "not-a-multiaddress", 3000);

    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_FALSE(result.error.empty());
}

// ---------------------------------------------------------------------------
// Tests - reliable channels
// ---------------------------------------------------------------------------

static const char* kTestChannelId = "integration-test-channel";
static const char* kTestChannelTopic = "/test/2/delivery-integration-chan/proto";
static const char* kTestSenderId = "integration-test-sender";

LOGOS_TEST(integration_channel_lifecycle) {
    ensureStarted();

    // Unknown id: not an error, reports "false".
    StdLogosResult missing = g_impl->channelExists("no-such-channel");
    LOGOS_ASSERT_TRUE(missing.success);
    LOGOS_ASSERT_EQ(missing.value.get<std::string>(), std::string("false"));

    // Create returns the channel id; the channel then exists.
    StdLogosResult created = g_impl->channelCreate(kTestChannelId, kTestChannelTopic, kTestSenderId);
    LOGOS_ASSERT_TRUE(created.success);
    LOGOS_ASSERT_EQ(created.value.get<std::string>(), std::string(kTestChannelId));

    StdLogosResult existing = g_impl->channelExists(kTestChannelId);
    LOGOS_ASSERT_TRUE(existing.success);
    LOGOS_ASSERT_EQ(existing.value.get<std::string>(), std::string("true"));

    // Close releases the channel; it no longer exists (persisted SDS state
    // survives, but a closed channel does not count as existing).
    LOGOS_ASSERT_TRUE(g_impl->channelClose(kTestChannelId).success);

    StdLogosResult closed = g_impl->channelExists(kTestChannelId);
    LOGOS_ASSERT_TRUE(closed.success);
    LOGOS_ASSERT_EQ(closed.value.get<std::string>(), std::string("false"));

    // Closing an unknown channel is an error (unlike channelExists).
    LOGOS_ASSERT_FALSE(g_impl->channelClose("no-such-channel").success);
}

LOGOS_TEST(integration_channel_send_returns_request_id) {
    ensureStarted();

    LOGOS_ASSERT_TRUE(g_impl->channelCreate(kTestChannelId, kTestChannelTopic, kTestSenderId).success);

    std::string msg = "hello from channel integration test";
    std::vector<uint8_t> payload(msg.begin(), msg.end());
    StdLogosResult result = g_impl->channelSend(kTestChannelId, payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_FALSE(result.value.get<std::string>().empty());

    LOGOS_ASSERT_TRUE(g_impl->channelClose(kTestChannelId).success);
}

LOGOS_TEST(integration_channel_send_fails_on_unknown_channel) {
    ensureStarted();

    std::vector<uint8_t> payload{'x'};
    LOGOS_ASSERT_FALSE(g_impl->channelSend("no-such-channel", payload).success);
}

// ---------------------------------------------------------------------------
// Tests - RLN bridge (registration + response path against the real library)
//
// The full request round trip (library fires a callback -> rln*Request event ->
// rlnRespond completes it) cannot be exercised yet: nothing in the library
// calls its internal rlnInvoke, and no trigger entry point is exported. These
// tests cover what IS reachable: the real logosdelivery_rln_set_callbacks /
// logosdelivery_rln_response symbols resolve, registration and clearing
// survive against the real library, and the response path rejects unknown
// request ids through the real in-flight list.
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_rlnRespond_rejects_unknown_reqid) {
    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(kMinimalConfig).success);

    // No RLN request is in flight (nothing triggers rlnInvoke yet), so any
    // reqId is unknown: the real library returns non-zero and the module
    // surfaces it as an error.
    StdLogosResult result = impl.rlnRespond(123456789, R"({"ok":{}})");
    LOGOS_ASSERT_FALSE(result.success);
    LOGOS_ASSERT_FALSE(result.error.empty());
}

LOGOS_TEST(integration_rln_callbacks_register_and_clear) {
    {
        DeliveryModuleImpl impl;
        LOGOS_ASSERT_TRUE(impl.createNode(kMinimalConfig).success);
        // Destructor clears the RLN surface (NULL registration) before
        // destroying the node; must complete without crashing.
    }

    // The surface is process-global in the library; a fresh module instance
    // must be able to register again after a clear.
    DeliveryModuleImpl impl2;
    LOGOS_ASSERT_TRUE(impl2.createNode(kMinimalConfig).success);
    LOGOS_ASSERT_FALSE(impl2.rlnRespond(1, R"({"ok":{}})").success);
}

// Blocks until any rln*Request event with the given op fires (or times out).
// The library awaits each response for ~10s before synthesizing a TRANSIENT
// failure, so requests appear well inside this window when the chain is live.
static bool waitForRlnRequestOp(const char* op, int timeoutMs = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (delivery_test_events::g_lastRlnRequest.op == op) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return delivery_test_events::g_lastRlnRequest.op == op;
}

// The full start chain, with this test playing the RLN module:
//
//   start() -> library fires the start callback -> rlnStartRequest event
//   -> rlnRespond(reqId, ok) -> library's await returns
//   -> library fires register_membership -> rlnRegisterRequest event
//   -> rlnRespond(reqId, ok) -> start_node completes -> nodeStarted
//
// node_api.nim drives this chain from logosdelivery_start_node, but only when
// self.waku.conf.rlnRelayConf.isSome(). Enabling that (kRlnConfig below) also
// makes waku want to mount its own on-node RLN relay, which needs a live eth
// RPC (rln-relay-dynamic=true) or a provisioned zerokit membership instance
// (dynamic=false, else SIGSEGV in createRLNInstanceLocal). The live path here
// therefore depends on logos-delivery temporarily skipping that native mount
// ("skipping native RLN relay mount; external RLN module in use"). Because a
// segfault can't be caught, this test does NOT enable RLN by default — it runs
// the live chain only when LOGOS_DELIVERY_RLN_LIVE is set (against a build that
// has the native-mount skip); otherwise it uses kMinimalConfig, sees no RLN
// request, and SKIPs. The real end-to-end (real logos-rln-module) is Tier 2.
static const char* kRlnConfig = R"({
  "logLevel": "INFO",
  "mode": "Edge",
  "relay": true,
  "numShardsInNetwork": 8,
  "rln-relay": true,
  "rln-relay-dynamic": false,
  "rln-relay-chain-id": 1,
  "rln-relay-eth-contract-address": "0x0000000000000000000000000000000000000000"
})";

LOGOS_TEST(integration_rln_start_chain_round_trip) {
    delivery_test_events::resetNodeLifecycleEvents();
    delivery_test_events::resetRlnRequestEvent();

    const bool rlnLive = std::getenv("LOGOS_DELIVERY_RLN_LIVE") != nullptr;

    // The shared ensureStarted() node (g_impl) from earlier tests may still be
    // running and holding the fixed discv5 UDP port; tear it down so this test's
    // node can bind. integration_send (next) re-creates it via ensureStarted().
    if (g_impl) {
        g_impl->stop();
        delete g_impl;
        g_impl = nullptr;
    }

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(rlnLive ? kRlnConfig : kMinimalConfig).success);
    LOGOS_ASSERT_TRUE(impl.start().success);

    if (!waitForRlnRequestOp("start")) {
        fprintf(stderr,
                "SKIP integration_rln_start_chain_round_trip: no RLN start "
                "request (set LOGOS_DELIVERY_RLN_LIVE against a native-mount-skip "
                "build to exercise the live chain)\n");
        impl.stop();
        waitForNodeStopped();
        return;
    }

    // Answer the start request; the library's await must accept it.
    const int64_t startReqId = delivery_test_events::g_lastRlnRequest.reqId;
    LOGOS_ASSERT_TRUE(impl.rlnRespond(startReqId, R"({"ok":{}})").success);

    // On success the library registers the membership: distinct request,
    // typed identity args populated from the node's RLN bring-up config.
    LOGOS_ASSERT_TRUE(waitForRlnRequestOp("register_membership"));
    const auto& reg = delivery_test_events::g_lastRlnRequest;
    LOGOS_ASSERT_FALSE(reg.registryId.empty());
    LOGOS_ASSERT_FALSE(reg.rlnIdentifier.empty());
    LOGOS_ASSERT_FALSE(reg.optionsJson.empty());
    LOGOS_ASSERT_TRUE(
        impl.rlnRespond(reg.reqId, R"({"ok":{"state":"REGISTERED"}})").success);

    // With the RLN chain answered, node startup completes.
    LOGOS_ASSERT_TRUE(waitForNodeStarted());
    LOGOS_ASSERT_TRUE(delivery_test_events::g_lastNodeStarted.success);

    // A second response for an already-completed request must be rejected
    // through the real in-flight list.
    LOGOS_ASSERT_FALSE(impl.rlnRespond(startReqId, R"({"ok":{}})").success);

    LOGOS_ASSERT_TRUE(impl.stop().success);
    LOGOS_ASSERT_TRUE(waitForNodeStopped());
}

// ---------------------------------------------------------------------------
// Tests - send (as in simple.cpp interactive loop)
// ---------------------------------------------------------------------------

LOGOS_TEST(integration_send_returns_success_with_request_id) {
    ensureStarted();

    LOGOS_ASSERT_TRUE(g_impl->subscribe(kTestTopic).success);

    std::string msg = "hello from integration test";
    std::vector<uint8_t> payload(msg.begin(), msg.end());
    StdLogosResult result = g_impl->send(kTestTopic, payload);

    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_FALSE(result.value.get<std::string>().empty());
}
