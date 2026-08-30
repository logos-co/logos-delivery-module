// Stub implementations for logos_events: methods.
// In the real build, the codegen generates delivery_module_events.cpp with
// bodies that route through LogosModuleContext::emitEventImpl_. For unit tests,
// the codegen doesn't run so we provide stubs here.
//
// The node-lifecycle events (nodeStarted / nodeStopped) record their last
// payload in process-global slots (see delivery_module_events_stub.h) so tests
// can assert that start()/stop() emit them. The message/connection events stay
// no-ops.

#include "delivery_module_plugin.h"

#include "delivery_module_events_stub.h"

namespace delivery_test_events {
NodeLifecycleEvent g_lastNodeStarted{};
NodeLifecycleEvent g_lastNodeStopped{};
RlnRequestEvent g_lastRlnRequest{};
} // namespace delivery_test_events

void DeliveryModuleImpl::messageSent(const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::messageError(const std::string&, const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::messagePropagated(const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::messageReceived(const std::string&, const std::string&, const std::vector<uint8_t>&, int64_t) {}
void DeliveryModuleImpl::connectionStateChanged(const std::string&, int64_t) {}
void DeliveryModuleImpl::channelMessageReceived(const std::string&, const std::string&, const std::vector<uint8_t>&, int64_t) {}
void DeliveryModuleImpl::channelMessageSent(const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::channelMessageError(const std::string&, const std::string&, const std::string&, int64_t) {}

void DeliveryModuleImpl::nodeStarted(bool success, const std::string& message, int64_t timestamp) {
    delivery_test_events::g_lastNodeStarted = {success, message, timestamp, true};
}
void DeliveryModuleImpl::nodeStopped(bool success, const std::string& message, int64_t timestamp) {
    delivery_test_events::g_lastNodeStopped = {success, message, timestamp, true};
}

void DeliveryModuleImpl::rlnStartRequest(int64_t reqId, const std::string& configJson,
                                         int64_t timestamp) {
    auto& e = delivery_test_events::g_lastRlnRequest;
    e = {};
    e.op = "start";
    e.reqId = reqId;
    e.configJson = configJson;
    e.timestamp = timestamp;
}

void DeliveryModuleImpl::rlnStopRequest(int64_t reqId, int64_t timestamp) {
    auto& e = delivery_test_events::g_lastRlnRequest;
    e = {};
    e.op = "stop";
    e.reqId = reqId;
    e.timestamp = timestamp;
}

void DeliveryModuleImpl::rlnRegisterRequest(int64_t reqId, const std::string& registryId,
                                            const std::string& rlnIdentifier, int64_t rateLimit,
                                            const std::string& optionsJson, int64_t timestamp) {
    auto& e = delivery_test_events::g_lastRlnRequest;
    e = {};
    e.op = "register_membership";
    e.reqId = reqId;
    e.registryId = registryId;
    e.rlnIdentifier = rlnIdentifier;
    e.rateLimit = rateLimit;
    e.optionsJson = optionsJson;
    e.timestamp = timestamp;
}

void DeliveryModuleImpl::rlnGetMembershipStateRequest(int64_t reqId, const std::string& registryId,
                                                      const std::string& rlnIdentifier,
                                                      int64_t timestamp) {
    auto& e = delivery_test_events::g_lastRlnRequest;
    e = {};
    e.op = "get_membership_state";
    e.reqId = reqId;
    e.registryId = registryId;
    e.rlnIdentifier = rlnIdentifier;
    e.timestamp = timestamp;
}

void DeliveryModuleImpl::rlnGetEpochQuotaRequest(int64_t reqId, const std::string& registryId,
                                                 const std::string& rlnIdentifier,
                                                 int64_t epochTimestamp, int64_t timestamp) {
    auto& e = delivery_test_events::g_lastRlnRequest;
    e = {};
    e.op = "get_epoch_quota";
    e.reqId = reqId;
    e.registryId = registryId;
    e.rlnIdentifier = rlnIdentifier;
    e.epochTimestamp = epochTimestamp;
    e.timestamp = timestamp;
}

void DeliveryModuleImpl::rlnGenerateProofRequest(int64_t reqId, const std::string& registryId,
                                                 const std::string& rlnIdentifier,
                                                 const std::string& signalHex,
                                                 int64_t epochTimestamp, int64_t timestamp) {
    auto& e = delivery_test_events::g_lastRlnRequest;
    e = {};
    e.op = "generate_proof";
    e.reqId = reqId;
    e.registryId = registryId;
    e.rlnIdentifier = rlnIdentifier;
    e.signalHex = signalHex;
    e.epochTimestamp = epochTimestamp;
    e.timestamp = timestamp;
}

void DeliveryModuleImpl::rlnValidateProofRequest(int64_t reqId, const std::string& registryId,
                                                 const std::string& rlnIdentifier,
                                                 const std::string& signalHex, int64_t epochTimestamp,
                                                 const std::string& proofJson, int64_t timestamp) {
    auto& e = delivery_test_events::g_lastRlnRequest;
    e = {};
    e.op = "validate_proof";
    e.reqId = reqId;
    e.registryId = registryId;
    e.rlnIdentifier = rlnIdentifier;
    e.signalHex = signalHex;
    e.epochTimestamp = epochTimestamp;
    e.proofJson = proofJson;
    e.timestamp = timestamp;
}
