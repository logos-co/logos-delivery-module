// lp_* C ABI stubs for the unit tests: no logos host is running. The client
// comes up (so the bridge can be enabled) and every async call completes at
// once with whatever a test staged in `delivery_test_lp`, or a transport error
// when nothing was.
#include "mock_logos_protocol.h"

namespace delivery_test_lp {
bool g_ok = false;
std::string g_reply;
std::string g_lastMethod;
std::string g_lastArgs;
int g_calls = 0;
} // namespace delivery_test_lp

extern "C" {

struct lp_client { int unused; };
struct lp_subscription;
typedef void (*lp_result_cb)(int ok, const char* json, void* user_data);
typedef void (*lp_event_cb)(const char* event_name, const char* data_json, void* user_data);

lp_client* lp_client_create(const char*, const char*, const char*, const char*)
{
    static lp_client client{0};
    return &client;
}
void lp_client_destroy(lp_client*) {}
int lp_invoke_async(lp_client*, const char* method, const char* args, int, lp_result_cb cb, void* ud)
{
    ++delivery_test_lp::g_calls;
    delivery_test_lp::g_lastMethod = method ? method : "";
    delivery_test_lp::g_lastArgs = args ? args : "";
    if (cb) {
        if (delivery_test_lp::g_ok) {
            cb(1, delivery_test_lp::g_reply.c_str(), ud);
        } else {
            cb(0, R"({"code":"mock","message":"no logos host","origin":"test"})", ud);
        }
    }
    return 0;
}
int lp_invoke(lp_client*, const char*, const char*, int, char**, char**)
{
    return -1;
}
void lp_string_free(char*) {}
lp_subscription* lp_subscribe(lp_client*, const char*, lp_event_cb, void*)
{
    return nullptr;
}
void lp_unsubscribe(lp_subscription*) {}

} // extern "C"
