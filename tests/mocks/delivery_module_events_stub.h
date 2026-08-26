// Shared test-observable state for the stubbed logos_events: methods.
// Lets tests check that start()/stop() emitted their completion events
// (nodeStarted / nodeStopped).
#pragma once

#include <cstdint>
#include <string>

namespace delivery_test_events {

struct NodeLifecycleEvent {
    bool success = false;
    std::string message;
    int64_t timestamp = 0;
    bool fired = false;  // set true once the event has been emitted at least once
};

extern NodeLifecycleEvent g_lastNodeStarted;
extern NodeLifecycleEvent g_lastNodeStopped;

// Last rln*Request event, whichever fired. `op` carries the ABI function name
// ("start", ..., "verify_proof"); empty means no RLN event fired. Only the
// fields the firing event carries are set.
struct RlnRequestEvent {
    std::string op;
    int64_t reqId = 0;
    std::string registryId;
    std::string rlnIdentifier;
    std::string signalHex;
    std::string optionsJson;
    std::string proofJson;
    int64_t epochTimestamp = 0;
    int64_t timestamp = 0;
};

extern RlnRequestEvent g_lastRlnRequest;

inline void resetNodeLifecycleEvents() {
    g_lastNodeStarted = NodeLifecycleEvent{};
    g_lastNodeStopped = NodeLifecycleEvent{};
}

inline void resetRlnRequestEvent() {
    g_lastRlnRequest = RlnRequestEvent{};
}

} // namespace delivery_test_events
