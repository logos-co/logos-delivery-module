#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <logosdelivery_service_discovery.h>
}

class Libp2pModule;

/**
 * @brief Hosts logos-delivery's service-discovery plugin on top of libp2p_module.
 *
 * Implements the C vtable from `logosdelivery_service_discovery.h` by
 * forwarding each verb to `libp2p_module`'s `disco*` API. libp2p_module is an
 * optional dependency: a node configured for external discovery without it
 * fails its start (see ensureBackend).
 *
 * Every entry point runs on logos-delivery's discovery worker thread, one verb
 * at a time. The SDK's synchronous calls need no Qt event loop there; the
 * `*Async` variants are unused because they complete on the Qt main thread.
 */
class DeliveryServiceDiscoveryPlugin
{
public:
    /**
     * @param libp2p Borrowed from `modules().libp2p_module`; null without a
     *        framework context.
     * @param libp2pConfig JSON object for libp2p_module's createNode (see
     *        discovery_config.h).
     */
    DeliveryServiceDiscoveryPlugin(Libp2pModule* libp2p, std::string libp2pConfig);

    /// The vtable to hand to logosdelivery_install_service_discovery_plugin.
    const LdServiceDiscoveryPlugin* vtable() const { return &vtable_; }

    /**
     * @brief Waits for entry points to drain before the object may be freed.
     *
     * A call that outran the node's timeout leaves its thread inside this
     * object; the owner leaks it rather than free it under that thread.
     *
     * @return true when no entry point is in flight, false on timeout.
     */
    bool quiesce(std::chrono::milliseconds timeout);

private:
    /**
     * @brief Brings libp2p up, once, on first use.
     *
     * Not done at registration: that runs in createNode on the Qt main thread,
     * where outbound calls are rejected. libp2p's createNode is the only place
     * its kademlia gets bootstrap peers.
     *
     * @return empty on success, otherwise a human-readable diagnostic.
     */
    std::string ensureBackend();

    /**
     * @brief Runs ensureBackend and maps a failure to LD_DISCO_ERROR.
     *
     * Every verb calls it: one reaching libp2p first would leave it on a
     * default node with no bootstrap peers, and lookups would silently find
     * nothing.
     */
    int requireBackend(char* errBuf, size_t errBufLen);

    /// Strips the "service:" prefix libp2p does not expect; other criteria
    /// kinds pass through unchanged.
    static std::string toServiceId(const char* key);
    Libp2pModule* libp2p_;
    std::string libp2pConfig_;
    bool backendReady_;
    bool nodeCreated_;
    std::atomic<int> inFlight_{0}; ///< Entry points executing; read by quiesce.
    LdServiceDiscoveryPlugin vtable_;

    // --- vtable trampolines; pluginCtx is always `this` ---------------------
    static int cStart(void* ctx, char* errBuf, size_t errBufLen);
    static int cStop(void* ctx, char* errBuf, size_t errBufLen);
    static int cLookup(void* ctx, const char* key, int64_t limit,
                       char** outJson, char* errBuf, size_t errBufLen);
    static int cRandomLookup(void* ctx, char** outJson, char* errBuf, size_t errBufLen);
    static void cFreeString(void* ctx, char* s);
    static int cStartAdvertising(void* ctx, const char* key,
                                 const uint8_t* data, size_t dataLen,
                                 const uint8_t* record, size_t recordLen,
                                 char* errBuf, size_t errBufLen);
    static int cStopAdvertising(void* ctx, const char* key, char* errBuf, size_t errBufLen);
    static int cRegisterInterest(void* ctx, const char* key, char* errBuf, size_t errBufLen);
    static int cUnregisterInterest(void* ctx, const char* key, char* errBuf, size_t errBufLen);
};
