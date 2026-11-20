// Shared test-observable state for the stubbed logos_events: methods.
// Lets tests check that start()/stop() emitted their completion events
// (nodeStarted / nodeStopped).
#pragma once

#include <chrono>
#include <condition_variable>
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
// test can tell a repeated state from a re-entered one. Emitted from the
// module's lifecycle worker, so read it through lastRlnState(), never directly.
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

// Holds the thread that emits rlnStateChanged inside the event: the next
// emission of the armed state is recorded, then blocks until release(). That
// thread is the module's lifecycle worker, so a test can keep an RLN bring-up
// in flight (armed on "Initializing", it stops the worker right before the
// backend start) and check what the API does meanwhile. One emission only.
struct RlnStateGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::string armedState; // empty: not armed
    bool holding = false;
    bool released = false;

    void arm(const std::string& state) {
        std::lock_guard<std::mutex> lock(mutex);
        armedState = state;
        holding = false;
        released = false;
    }
    // Called by the rlnStateChanged stub.
    void pass(const std::string& state) {
        std::unique_lock<std::mutex> lock(mutex);
        if (armedState.empty() || armedState != state) {
            return;
        }
        armedState.clear();
        holding = true;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
        holding = false;
    }
    // Whether an emission is being held, waiting up to `timeout` for one.
    bool awaitHolding(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [this] { return holding; });
    }
    bool isHolding() {
        std::lock_guard<std::mutex> lock(mutex);
        return holding;
    }
    // Disarms and lets a held emission go.
    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            armedState.clear();
            released = true;
        }
        cv.notify_all();
    }
};

extern RlnStateGate g_rlnStateGate;

} // namespace delivery_test_events
