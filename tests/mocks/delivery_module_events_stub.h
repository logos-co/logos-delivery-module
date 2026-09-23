// Shared test-observable state for the stubbed logos_events: methods.
// Lets tests check that start()/stop() emitted their completion events
// (nodeStarted / nodeStopped).
#pragma once

#include <cstdint>
#include <mutex>
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
// ("start", ..., "validate_proof"); empty means no RLN event fired. Only the
// fields the firing event carries are set.
struct RlnRequestEvent {
    std::string op;
    int64_t reqId = 0;
    std::string registryId;
    std::string rlnIdentifier;
    std::string signalHex;
    std::string configJson;
    std::string optionsJson;
    std::string proofJson;
    int64_t epochTimestamp = 0;
    int64_t timestamp = 0;
};

extern RlnRequestEvent g_lastRlnRequest;

// Last rlnStateChanged payload; `transitions` counts every emission, so a
// test can tell a repeated state from a re-entered one. Emitted from the RLN
// bring-up thread, so read it through lastRlnState(), never directly.
struct RlnStateEvent {
    std::string state;
    std::string message;
    int transitions = 0;
};

extern RlnStateEvent g_lastRlnState;
extern std::mutex g_rlnStateMutex;

inline RlnStateEvent lastRlnState() {
    std::lock_guard<std::mutex> lock(g_rlnStateMutex);
    return g_lastRlnState;
}

inline void resetNodeLifecycleEvents() {
    g_lastNodeStarted = NodeLifecycleEvent{};
    g_lastNodeStopped = NodeLifecycleEvent{};
}

inline void resetRlnRequestEvent() {
    g_lastRlnRequest = RlnRequestEvent{};
}

inline void resetRlnStateEvent() {
    std::lock_guard<std::mutex> lock(g_rlnStateMutex);
    g_lastRlnState = RlnStateEvent{};
}

} // namespace delivery_test_events
