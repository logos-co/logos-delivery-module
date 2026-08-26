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

struct RlnRequestEvent {
    int64_t reqId = 0;
    std::string op;
    std::string payloadJson;
    int64_t timestamp = 0;
    bool fired = false;  // set true once the event has been emitted at least once
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
