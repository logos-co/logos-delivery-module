#include "channel_cipher.h"

#include <chrono>
#include <cstdio>
#include <semaphore>
#include <utility>

#include <boost/beast/core/detail/base64.hpp>
#include <nlohmann/json.hpp>

#include <logos_protocol.h> // lp_* C ABI

// Not in logos-cpp-sdk 0.2.0 as this module pins it; the target-is-the-caller
// check below turns on with the builder bump that brings it.
#if __has_include(<logos_caller.h>)
#include <logos_caller.h>
#define DELIVERY_HAVE_LOGOS_CALLER 1
#endif

using nlohmann::json;

namespace {
namespace b64 = boost::beast::detail::base64;

constexpr const char* kOrigin = "delivery_module";

// Budget for one cipher round trip. It stalls the delivery library's event
// loop for its whole duration, and the target is an in-process module, so it
// sits far below the 30s a module API call gets.
constexpr int kCipherTimeoutMs = 5'000;

std::string base64Encode(const uint8_t* data, size_t len)
{
    std::string out;
    out.resize(b64::encoded_size(len));
    out.resize(b64::encode(out.data(), data, len));
    return out;
}

bool base64Decode(const std::string& encoded, std::vector<uint8_t>& out)
{
    out.resize(b64::decoded_size(encoded.size()));
    auto [written, read] = b64::decode(out.data(), encoded.data(), encoded.size());
    // decode() stops at the padding, so only '=' may follow what it consumed.
    if (encoded.find_first_not_of('=', read) != std::string::npos) {
        return false;
    }
    out.resize(written);
    return true;
}

struct ReplyBox {
    std::binary_semaphore sem{0};
    bool ok = false;
    std::string json;
};

void replyTrampoline(int ok, const char* jsonText, void* userData)
{
    std::unique_ptr<std::shared_ptr<ReplyBox>> boxPtr(
        static_cast<std::shared_ptr<ReplyBox>*>(userData));
    auto& box = **boxPtr;
    box.ok = ok != 0;
    if (jsonText) {
        box.json = jsonText;
    }
    box.sem.release();
}

} // namespace

struct ChannelCipherRelay::Registration {
    std::string channelId;
    std::string encryptMethod;
    std::string decryptMethod;
    lp_client* client = nullptr;
    // Handed to the library as `out`, which it copies before returning. One
    // buffer per direction: the library calls the pair inline on a single
    // thread, so two calls never share a buffer.
    std::vector<uint8_t> encryptBuf;
    std::vector<uint8_t> decryptBuf;

    ~Registration()
    {
        if (client) {
            lp_client_destroy(client);
        }
    }

    bool invoke(const std::string& method, const uint8_t* in, size_t inLen,
                std::vector<uint8_t>& out)
    {
        if (!client) {
            return false;
        }
        json args = json::array();
        args.push_back(channelId);
        args.push_back(base64Encode(in, inLen));
        const std::string argsJson = args.dump();

        auto box = std::make_shared<ReplyBox>();
        auto* handoff = new std::shared_ptr<ReplyBox>(box);
        const int rc = lp_invoke_async(client, method.c_str(), argsJson.c_str(),
                                       kCipherTimeoutMs, &replyTrampoline, handoff);
        if (rc != LP_OK) {
            delete handoff;
            fprintf(stderr, "delivery_module: channel %s %s dispatch failed rc=%d\n",
                    channelId.c_str(), method.c_str(), rc);
            return false;
        }

        const auto wait = std::chrono::milliseconds(kCipherTimeoutMs) + std::chrono::seconds(5);
        if (!box->sem.try_acquire_for(wait)) {
            fprintf(stderr, "delivery_module: channel %s %s did not answer in %dms\n",
                    channelId.c_str(), method.c_str(), kCipherTimeoutMs);
            return false;
        }
        if (!box->ok) {
            fprintf(stderr, "delivery_module: channel %s %s failed: %s\n",
                    channelId.c_str(), method.c_str(), box->json.c_str());
            return false;
        }

        const json parsed = json::parse(box->json, nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_string()) {
            // A null here is what an unreachable target answers once the call
            // has burned its timeout, so name that rather than the JSON type.
            fprintf(stderr,
                    "delivery_module: channel %s %s answered %s, not a base64 string. A "
                    "null means nothing served the call: the target publishes no "
                    "provider (a `ui_qml` module never does) or names no such method.\n",
                    channelId.c_str(), method.c_str(), box->json.c_str());
            return false;
        }
        const std::string encoded = parsed.get<std::string>();
        if (encoded.empty()) {
            fprintf(stderr, "delivery_module: channel %s %s declined the payload\n",
                    channelId.c_str(), method.c_str());
            return false;
        }
        if (!base64Decode(encoded, out)) {
            fprintf(stderr, "delivery_module: channel %s %s returned invalid base64\n",
                    channelId.c_str(), method.c_str());
            return false;
        }
        return true;
    }
};

ChannelCipherRelay::ChannelCipherRelay() = default;
ChannelCipherRelay::~ChannelCipherRelay() = default;

std::string parseChannelCipherSpec(const std::string& text, ChannelCipherSpec& out)
{
    out = {};
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return {};
    }

    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        return "cipherSpec must be a JSON object";
    }

    const auto readString = [&doc](const char* key) -> std::string {
        const auto it = doc.find(key);
        return (it != doc.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };

    out.module = readString("module");
    out.encryptMethod = readString("encrypt");
    out.decryptMethod = readString("decrypt");

    if (out.encryptMethod.empty() || out.decryptMethod.empty()) {
        return "cipherSpec needs both `encrypt` and `decrypt` method names";
    }
    return {};
}

std::string ChannelCipherRelay::registerChannel(const std::string& channelId,
                                                const ChannelCipherSpec& spec,
                                                uint64_t& encryptFn,
                                                uint64_t& decryptFn,
                                                uint64_t& userData)
{
    encryptFn = 0;
    decryptFn = 0;
    userData = 0;

    if (spec.empty()) {
        return {};
    }

    std::string target = spec.module;
#ifdef DELIVERY_HAVE_LOGOS_CALLER
    const logos::LogosCaller& caller = logos::currentCaller();
    if (caller.isModule()) {
        if (target.empty()) {
            target = caller.name;
        } else if (target != caller.name) {
            return "cipher target " + target + " is not the caller (" + caller.name +
                   "); only the module that creates a channel may be its cipher";
        }
    }
#endif
    if (target.empty()) {
        return "cipherSpec needs an explicit `module`: the caller could not be named";
    }

    auto registration = std::make_shared<Registration>();
    registration->channelId = channelId;
    registration->encryptMethod = spec.encryptMethod;
    registration->decryptMethod = spec.decryptMethod;
    registration->client = lp_client_create(target.c_str(), kOrigin, nullptr, nullptr);
    if (!registration->client) {
        return "could not reach cipher target " + target;
    }


    {
        std::lock_guard<std::mutex> lock(m_lock);
        // Kept forever: `user_data` must outlive logosdelivery_destroy, which is
        // the only call that drains in-flight sends.
        m_registrations.push_back(registration);
    }

    encryptFn = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(&ChannelCipherRelay::encryptTrampoline));
    decryptFn = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(&ChannelCipherRelay::decryptTrampoline));
    userData = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(registration.get()));
    return {};
}

// The two trampolines are C callbacks invoked from the Nim runtime on its event
// loop thread: a C++ exception escaping here would unwind into Nim frames and
// terminate the process, so both bodies are fenced.
int ChannelCipherRelay::encryptTrampoline(void* userData, const uint8_t* in, size_t inLen,
                                          const uint8_t** out, size_t* outLen)
{
    auto* registration = static_cast<Registration*>(userData);
    if (!registration || !out || !outLen) {
        return 1;
    }
    try {
        if (!registration->invoke(registration->encryptMethod, in, inLen,
                                  registration->encryptBuf)) {
            return 1;
        }
        *out = registration->encryptBuf.data();
        *outLen = registration->encryptBuf.size();
        return 0;
    } catch (...) {
        return 1;
    }
}

int ChannelCipherRelay::decryptTrampoline(void* userData, const uint8_t* in, size_t inLen,
                                          const uint8_t** out, size_t* outLen)
{
    auto* registration = static_cast<Registration*>(userData);
    if (!registration || !out || !outLen) {
        return 1;
    }
    try {
        if (!registration->invoke(registration->decryptMethod, in, inLen,
                                  registration->decryptBuf)) {
            return 1;
        }
        *out = registration->decryptBuf.data();
        *outLen = registration->decryptBuf.size();
        return 0;
    } catch (...) {
        return 1;
    }
}
