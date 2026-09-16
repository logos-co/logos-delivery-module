#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct lp_client;

/**
 * @brief One channel's cipher target: a method pair on the module that created
 *        the channel.
 *
 * All three fields empty means the channel is not encrypted.
 */
struct ChannelCipherSpec {
    std::string module;
    std::string encryptMethod;
    std::string decryptMethod;

    bool empty() const { return encryptMethod.empty() && decryptMethod.empty(); }
};

/**
 * @brief Parses the `cipherSpec` argument of `channelCreate`.
 *
 * Accepts an empty (or blank) string as "no cipher", otherwise a JSON object
 * with `encrypt`, `decrypt` and an optional `module`.
 *
 * @param text JSON document, or empty for an unencrypted channel.
 * @param out Parsed spec; left empty when @p text is.
 * @return An error message, or an empty string on success.
 */
std::string parseChannelCipherSpec(const std::string& text, ChannelCipherSpec& out);

/**
 * @brief Relays a channel's encrypt/decrypt to the module that owns it.
 *
 * The delivery library takes a `LogosDeliveryCryptoFn` pair plus `user_data`
 * per channel and calls them inline on its event loop. This class supplies
 * that pair: each call is forwarded to the owning module over the
 * logos-protocol C ABI (`lp_invoke_async`) and answered with what comes back,
 * so keys and crypto never live here.
 *
 * Payloads cross as base64 strings — `encrypt(channelId, payloadB64)` returns
 * the sealed payload base64-encoded, `decrypt` the reverse — because a Qt
 * provider cannot return a byte array. An empty return, a transport failure or
 * a timeout fails the message; the library never falls back to plaintext.
 *
 * ### Threading
 *
 * The cipher runs on the delivery library's event-loop thread and blocks it for
 * the round trip, so the owning module must service the call on a thread that
 * is not itself waiting on `delivery_module`. In practice that holds because
 * `delivery_module` declares `concurrency: "multi"`: its own blocking handlers
 * run on pool workers, leaving the host's dispatch thread free to deliver the
 * cipher call. A cipher handler must not call back into `delivery_module`.
 *
 * Registrations live for the relay's lifetime, not the channel's:
 * `user_data` must stay valid until `logosdelivery_destroy` returns.
 */
class ChannelCipherRelay
{
public:
    ChannelCipherRelay();
    ~ChannelCipherRelay();

    ChannelCipherRelay(const ChannelCipherRelay&) = delete;
    ChannelCipherRelay& operator=(const ChannelCipherRelay&) = delete;

    /**
     * @brief Registers @p spec for @p channelId and yields the three fields
     *        `LogosdeliveryChannelCreateReq` wants.
     *
     * The target is not checked here: a module's method list is not reliably
     * readable from another module, so a bad name or a target that publishes
     * no provider surfaces on the first cipher call instead.
     *
     * A registration handed to the library is kept for the relay's life, since
     * the library may still hold its `user_data`; @ref forgetChannel drops one
     * that never reached it.
     *
     * @return An error message, or an empty string on success.
     */
    std::string registerChannel(const std::string& channelId,
                                const ChannelCipherSpec& spec,
                                uint64_t& encryptFn,
                                uint64_t& decryptFn,
                                uint64_t& userData);

    /**
     * @brief Drops a registration the library never received.
     *
     * Only safe when the `channelCreate` that would have handed `user_data`
     * over failed; a registration the library holds must outlive it.
     */
    void forgetChannel(uint64_t userData);

private:
    struct Registration;

    static int encryptTrampoline(void* userData, const uint8_t* in, size_t inLen,
                                 const uint8_t** out, size_t* outLen);
    static int decryptTrampoline(void* userData, const uint8_t* in, size_t inLen,
                                 const uint8_t** out, size_t* outLen);

    std::mutex m_lock;
    std::vector<std::shared_ptr<Registration>> m_registrations;
};
