// Mock of liblogosdelivery's nim-ffi reverse-call surface for the service
// discovery host: the per-wire registration exports, the reply exports, and
// the generated decode/reply helpers. Upstream's helpers are static inline
// CBOR codecs; here an invocation's `args` point at a MockArgs instead of CBOR
// bytes, and replies are recorded rather than encoded.

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <logos_clib_mock.h>

extern "C" {
#include <liblogosdelivery.h>
}

#include "mock_reverse_state.h"

namespace {

/// What a test's invoke() hands the implementation as `args`.
struct MockArgs {
    const char* key;
    int64_t limit;
};

struct Registration {
    FFIReverseImpl impl = nullptr;
    void* userData = nullptr;
};

std::mutex g_mutex;
std::condition_variable g_replied;
std::map<std::string, Registration> g_impls;
std::map<uint64_t, delivery_test_reverse::Reply> g_replies;
std::thread::id g_invoker;

int registerImpl(const char* wire, FFIReverseImpl impl, void* userData)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_impls[wire] = Registration{impl, userData};
    return NIMFFI_REVERSE_ACCEPTED;
}

int record(void* ctx, uint64_t callId, bool ok, const char* error, const SdPeerRecords* peers)
{
    delivery_test_reverse::Reply r;
    r.answered = true;
    r.ok = ok;
    r.error = error ? error : "";
    r.ctx = ctx;
    r.onOtherThread = std::this_thread::get_id() != g_invoker;
    if (peers) {
        for (size_t i = 0; i < peers->peers.len; ++i) {
            const SdPeerRecord& p = peers->peers.data[i];
            r.peers.push_back(p.peerId ? p.peerId : "");
            if (i == 0 && p.services.len > 0) {
                const NimFfiBytes& d = p.services.data[0].data;
                r.firstServiceData.assign(d.data, d.data + d.len);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_replies[callId] = std::move(r);
    }
    g_replied.notify_all();
    return NIMFFI_REVERSE_ACCEPTED;
}

int decodeKey(const uint8_t* args, const char** out)
{
    const MockArgs* a = reinterpret_cast<const MockArgs*>(args);
    *out = strdup(a && a->key ? a->key : "");
    return 0;
}

} // namespace

namespace delivery_test_reverse {

void reset()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_impls.clear();
    g_replies.clear();
}

int registeredCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    int n = 0;
    for (const auto& [wire, reg] : g_impls) {
        if (reg.impl) ++n;
    }
    return n;
}

void invoke(const std::string& wire, void* ctx, uint64_t callId, const std::string& key,
            int64_t limit)
{
    Registration reg;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        reg = g_impls[wire];
        g_invoker = std::this_thread::get_id();
    }
    if (!reg.impl) {
        return;
    }
    // Valid only for the call, as the library's CBOR args are.
    const MockArgs args{key.c_str(), limit};
    reg.impl(ctx, callId, reinterpret_cast<const uint8_t*>(&args), sizeof(args), reg.userData);
}

Reply waitReply(uint64_t callId)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_replied.wait_for(lock, std::chrono::seconds(5),
                       [&] { return g_replies.count(callId) > 0; });
    const auto it = g_replies.find(callId);
    return it == g_replies.end() ? Reply{} : it->second;
}

} // namespace delivery_test_reverse

extern "C" {

#define DEFINE_SET_IMPL(wire)                                                              \
    int logosdelivery_set_##wire##_impl(void* /*ctx*/, FFIReverseImpl impl, void* userData, \
                                        FFIReverseRelease /*release*/)                     \
    {                                                                                      \
        LOGOS_CMOCK_RECORD("logosdelivery_set_" #wire "_impl");                           \
        return registerImpl(#wire, impl, userData);                                        \
    }

DEFINE_SET_IMPL(sd_start)
DEFINE_SET_IMPL(sd_stop)
DEFINE_SET_IMPL(sd_lookup)
DEFINE_SET_IMPL(sd_random_lookup)
DEFINE_SET_IMPL(sd_start_advertising)
DEFINE_SET_IMPL(sd_stop_advertising)
DEFINE_SET_IMPL(sd_register_interest)
DEFINE_SET_IMPL(sd_unregister_interest)

#undef DEFINE_SET_IMPL

int logosdelivery_decode_sd_lookup_args(const uint8_t* args, size_t /*len*/, SdLookupArgs* out,
                                        char** /*err*/)
{
    const MockArgs* a = reinterpret_cast<const MockArgs*>(args);
    out->key = strdup(a && a->key ? a->key : "");
    out->limit = a ? a->limit : 0;
    return 0;
}

int logosdelivery_decode_sd_start_advertising_args(const uint8_t* args, size_t /*len*/,
                                                   SdStartAdvertisingArgs* out, char** /*err*/)
{
    const MockArgs* a = reinterpret_cast<const MockArgs*>(args);
    out->key = strdup(a && a->key ? a->key : "");
    out->data = NimFfiBytes{nullptr, 0};
    out->record = NimFfiBytes{nullptr, 0};
    return 0;
}

int logosdelivery_decode_sd_stop_advertising_args(const uint8_t* args, size_t, const char** out, char**)
{
    return decodeKey(args, out);
}

int logosdelivery_decode_sd_register_interest_args(const uint8_t* args, size_t, const char** out, char**)
{
    return decodeKey(args, out);
}

int logosdelivery_decode_sd_unregister_interest_args(const uint8_t* args, size_t, const char** out, char**)
{
    return decodeKey(args, out);
}

void logosdelivery_free_SdLookupArgs(SdLookupArgs* v)
{
    free(const_cast<char*>(v->key));
}

void logosdelivery_free_SdStartAdvertisingArgs(SdStartAdvertisingArgs* v)
{
    free(const_cast<char*>(v->key));
}

int logosdelivery_reverse_reply_err(void* ctx, uint64_t callId, const char* msg)
{
    return record(ctx, callId, false, msg, nullptr);
}

int logosdelivery_reverse_reply_sd_start(void* ctx, uint64_t callId) { return record(ctx, callId, true, nullptr, nullptr); }
int logosdelivery_reverse_reply_sd_stop(void* ctx, uint64_t callId) { return record(ctx, callId, true, nullptr, nullptr); }
int logosdelivery_reverse_reply_sd_start_advertising(void* ctx, uint64_t callId) { return record(ctx, callId, true, nullptr, nullptr); }
int logosdelivery_reverse_reply_sd_stop_advertising(void* ctx, uint64_t callId) { return record(ctx, callId, true, nullptr, nullptr); }
int logosdelivery_reverse_reply_sd_register_interest(void* ctx, uint64_t callId) { return record(ctx, callId, true, nullptr, nullptr); }
int logosdelivery_reverse_reply_sd_unregister_interest(void* ctx, uint64_t callId) { return record(ctx, callId, true, nullptr, nullptr); }

int logosdelivery_reverse_reply_sd_lookup(void* ctx, uint64_t callId, const SdPeerRecords* reply)
{
    return record(ctx, callId, true, nullptr, reply);
}

int logosdelivery_reverse_reply_sd_random_lookup(void* ctx, uint64_t callId, const SdPeerRecords* reply)
{
    return record(ctx, callId, true, nullptr, reply);
}

} // extern "C"
