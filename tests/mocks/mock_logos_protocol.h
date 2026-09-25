// Test-observable state of the lp_* stubs (mock_logos_protocol.cpp).
#pragma once
#include <string>

namespace delivery_test_lp {
extern bool g_ok;              // the next async call completes ok with g_reply
extern std::string g_reply;    // the lp result text handed to the completion
extern std::string g_lastMethod;
extern std::string g_lastArgs;
extern int g_calls;
inline void reset()
{
    g_ok = false;
    g_reply.clear();
    g_lastMethod.clear();
    g_lastArgs.clear();
    g_calls = 0;
}
} // namespace delivery_test_lp
