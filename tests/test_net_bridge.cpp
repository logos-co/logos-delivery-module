// Bridged mode (libp2pProvider): mocks/net_bridge_mock.cpp fakes the provider and the ABI, so the op allow-list in src/libp2p_module_transport.cpp is out of reach here.

#include <logos_test.h>

#include <nlohmann/json.hpp>

#include "delivery_module_plugin.h"
#include "mocks/net_bridge_mock.h"

namespace {

using nlohmann::json;
namespace net = delivery_test_net;

constexpr const char* kPeerId = "16Uiu2HAmShared";

std::string bridgedCfg(const char* provider = kDefaultLibp2pProvider)
{
    return json{{"mode", "Edge"}, {"preset", "logos.test"}, {"libp2pProvider", provider}}.dump();
}

void arrange(LogosTestContext& t, const char* localPeerId, net::Handler provider)
{
    t.mockCFunction("logosdelivery_create_node").returns(1);
    t.mockCFunction("logosdelivery_get_node_info").returns(localPeerId);
    net::reset();
    net::setHandler(std::move(provider));
}

DeliveryModuleImpl* createBridgedImpl(LogosTestContext& t)
{
    arrange(t, kPeerId, net::providerWithPeerId(kPeerId));

    auto* impl = new DeliveryModuleImpl();
    LOGOS_ASSERT_TRUE(impl->createNode(bridgedCfg()).success);
    return impl;
}

net::Answer answerFor(uint64_t requestId, const json& op)
{
    LOGOS_ASSERT_TRUE(net::submit(requestId, op.dump()));
    net::Answer answer;
    LOGOS_ASSERT_TRUE(net::waitForAnswer(requestId, answer, 2000));
    return answer;
}

} // namespace

LOGOS_TEST(bridged_createNode_registers_the_net_backend) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createBridgedImpl(t);

    LOGOS_ASSERT_TRUE(net::registeredName() == kDefaultLibp2pProvider);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_create_node"));
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_get_node_info"));
    delete impl;
}

LOGOS_TEST(bridged_createNode_registers_under_the_configured_name) {
    auto t = LogosTestContext("delivery_module");
    arrange(t, kPeerId, net::providerWithPeerId(kPeerId));

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(bridgedCfg("other_libp2p")).success);
    LOGOS_ASSERT_TRUE(net::registeredName() == "other_libp2p");
}

LOGOS_TEST(default_createNode_registers_no_net_backend) {
    auto t = LogosTestContext("delivery_module");
    arrange(t, kPeerId, net::providerWithPeerId(kPeerId));

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_TRUE(impl.createNode(R"({"mode":"Core","preset":"logos.test"})").success);
    LOGOS_ASSERT_TRUE(net::callCount() == 0);
}

// liblogosdelivery rejects a blank name, so the module must not bridge behind its back.
LOGOS_TEST(blank_provider_registers_no_net_backend) {
    auto t = LogosTestContext("delivery_module");
    arrange(t, kPeerId, net::providerWithPeerId(kPeerId));

    DeliveryModuleImpl impl;
    (void) impl.createNode(bridgedCfg("   "));
    LOGOS_ASSERT_TRUE(net::callCount() == 0);
}

LOGOS_TEST(bridged_createNode_fails_when_the_provider_is_unreachable) {
    auto t = LogosTestContext("delivery_module");
    arrange(t, kPeerId, [](const std::string&, const json&) -> StdLogosResult {
        return {false, {}, "module not loaded"};
    });

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.createNode(bridgedCfg()).success);
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("logosdelivery_create_node"));
}

LOGOS_TEST(bridged_createNode_fails_on_a_peer_id_mismatch) {
    auto t = LogosTestContext("delivery_module");
    arrange(t, "16Uiu2HAmOther", net::providerWithPeerId(kPeerId));

    DeliveryModuleImpl impl;
    LOGOS_ASSERT_FALSE(impl.createNode(bridgedCfg()).success);
    LOGOS_ASSERT(t.cFunctionCalled("logosdelivery_destroy"));
}

LOGOS_TEST(bridged_submit_forwards_the_op_and_answers) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createBridgedImpl(t);

    net::setHandler([](const std::string& method, const json& args) -> StdLogosResult {
        if (method != "protocolRequest") {
            return {false, {}, "unexpected op: " + method};
        }
        return {true, json{{"responseB64", args.value("requestB64", std::string{})}}, ""};
    });

    auto answer = answerFor(1, json{{"op", "protocolRequest"},
                                    {"args", json{{"peerId", "16Uiu2HAmPeer"},
                                                  {"proto", "/vac/waku/store/3.0.0"},
                                                  {"requestB64", "cGluZw=="}}}});
    LOGOS_ASSERT_TRUE(answer.ok);
    LOGOS_ASSERT_TRUE(json::parse(answer.data)["responseB64"].get<std::string>() == "cGluZw==");
    delete impl;
}

// Every request gets one answer, so a bad one may neither hang nor kill a worker.
LOGOS_TEST(bridged_submit_answers_every_bad_request) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createBridgedImpl(t);

    net::setHandler([](const std::string& method, const json& args) -> StdLogosResult {
        if (method == "throws") {
            return {true, args.at("missing"), ""};
        }
        return {false, {}, "mock rejects op " + method};
    });

    auto unknownOp = answerFor(2, json{{"op", "selfDestruct"}});
    LOGOS_ASSERT_FALSE(unknownOp.ok);
    LOGOS_ASSERT_TRUE(unknownOp.data.find("selfDestruct") != std::string::npos);

    LOGOS_ASSERT_TRUE(net::submit(3, "{not json"));
    net::Answer malformed;
    LOGOS_ASSERT_TRUE(net::waitForAnswer(3, malformed, 2000));
    LOGOS_ASSERT_FALSE(malformed.ok);

    LOGOS_ASSERT_FALSE(answerFor(4, json{{"op", 42}}).ok);
    LOGOS_ASSERT_FALSE(answerFor(5, json{{"op", "throws"}, {"args", 7}}).ok);
    delete impl;
}

LOGOS_TEST(bridged_submit_after_stop_answers_with_an_error) {
    auto t = LogosTestContext("delivery_module");
    auto* impl = createBridgedImpl(t);
    delete impl;

    auto answer = answerFor(6, json{{"op", "getNodeInfo"}, {"args", json{{"field", "PeerId"}}}});
    LOGOS_ASSERT_FALSE(answer.ok);
}
