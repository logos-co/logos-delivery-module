// lp_* C ABI stubs for the unit tests: no logos host is running, so the
// bridge's client never comes up — enable still works and every op that
// would reach the module synthesizes a transport failure instead.
extern "C" {

struct lp_client;
struct lp_subscription;
typedef void (*lp_result_cb)(int ok, const char* json, void* user_data);
typedef void (*lp_event_cb)(const char* event_name, const char* data_json,
                            void* user_data);

lp_client* lp_client_create(const char*, const char*, const char*, const char*)
{
    return nullptr;
}

void lp_client_destroy(lp_client*) {}

int lp_invoke_async(lp_client*, const char*, const char*, int, lp_result_cb, void*)
{
    return -1;
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
