// Shared test-observable state for the mocked liblogosdelivery RLN surface.
// Lets tests fire the callbacks the module registered (simulating the library
// requesting an RLN op) and inspect what rlnRespond forwarded.
#pragma once

#include <cstdint>
#include <string>

#include <liblogosdelivery_rln.h>

namespace delivery_test_rln {

// Last struct registered via logosdelivery_rln_set_callbacks. All slots are
// nullptr after a NULL (clear) registration.
extern LogosDeliveryRlnCallbacks g_callbacks;
extern void* g_userData;
extern bool g_callbacksSet;    // true once set_callbacks was called at least once
extern int g_setCallbacksCalls;

// Last logosdelivery_rln_response arguments.
extern uint64_t g_lastResponseReqId;
extern std::string g_lastResponseJson;
extern bool g_responseFired;

inline void resetRlnMockState() {
    g_callbacks = LogosDeliveryRlnCallbacks{};
    g_userData = nullptr;
    g_callbacksSet = false;
    g_setCallbacksCalls = 0;
    g_lastResponseReqId = 0;
    g_lastResponseJson.clear();
    g_responseFired = false;
}

} // namespace delivery_test_rln
