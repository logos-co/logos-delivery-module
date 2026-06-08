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

    // IDs are returned as "@[ID1,ID2,...]" - strip the "@[" prefix and "]" suffix.
    if (nodeInfoIDs.size() > 3 &&
        nodeInfoIDs[0] == '@' && nodeInfoIDs[1] == '[' &&
        nodeInfoIDs.back() == ']') {
        nodeInfoIDs = nodeInfoIDs.substr(2, nodeInfoIDs.size() - 3);
    }

    // Split on comma
    std::vector<std::string> ids;
    std::string current;
    for (char c : nodeInfoIDs) {
        if (c == ',') {
            if (!current.empty()) ids.push_back(current);
            current.clear();
        } else if (c != ' ') {
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
