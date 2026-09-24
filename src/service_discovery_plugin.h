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
 * logos-delivery can delegate peer/service discovery to an external provider
 * through the C vtable declared in `logosdelivery_service_discovery.h`. This
 * class implements that vtable by forwarding each verb to `libp2p_module`'s
 * `disco*` API.
 *
 * ## The dependency is optional, and the calls are typed
 *
 * `libp2p_module` is listed in `metadata.json#optional_dependencies`: at build
 * time logos-module-builder generates the typed `libp2p_module_api.h` (a
 * `Libp2pModule` class) and its `LogosModules` member from libp2p's contract,
 * but the loader never requires the module. So a node without external service
 * discovery -- and every node on Windows, where libp2p_module has no build --
 * loads and runs without it.
 *
 * The contract comes from a `dependency_overrides` entry naming libp2p's own
 * impl header, not from its published `packages.<system>.lidl`: the header is
 * the same on every platform, so Windows builds get the same typed client.
 *
 * A node that IS configured for external service discovery needs the module,
 * and finds out on first contact: ensureBackend bounds that call and turns
 * `object_unavailable` into a start failure saying so.
 *
 * ## Threading
 *
 * Every entry point below is invoked on logos-delivery's own discovery worker
 * thread, never on the module's Qt main thread. That is safe: the SDK's
 * synchronous calls bottom out in a `std::future` wait against a connection
 * driven by its own Asio thread, need no Qt event loop on the calling thread,
 * and are internally serialized (atomic request ids, a mutex-guarded pending
 * map, a strand for writes). The generated `*Async` variants are deliberately
 * unused: they post their completion to the Qt main thread, which this worker
 * does not run.
 *
 * Calls from one node are serialized by that single worker thread, so this
 * object sees one verb at a time.
 */
class DeliveryServiceDiscoveryPlugin
{
public:
    /**
     * @param libp2p Borrowed from `modules().libp2p_module`; owned by the
     *        `LogosModules` aggregate, which outlives this object.
     * @param libp2pConfig JSON object text for libp2p_module's createNode, as
     *        resolved from the node config (see discovery_config.h).
     */
    DeliveryServiceDiscoveryPlugin(Libp2pModule* libp2p, std::string libp2pConfig);

    /// The vtable to hand to logosdelivery_install_service_discovery_plugin.
    const LdServiceDiscoveryPlugin* vtable() const { return &vtable_; }

    /**
     * @brief Waits for entry points to drain before the object may be freed.
     *
     * A call that outran the node's timeout leaves its thread abandoned but
     * still inside this object (see the calling model in
     * logosdelivery_service_discovery.h). Destroying it then would pull
     * pluginCtx out from under a live thread, so the owner asks first and
     * leaks the object rather than freeing it when the answer is no.
     *
     * @return true when no entry point is in flight, false on timeout.
     */
    bool quiesce(std::chrono::milliseconds timeout);

private:
    /**
     * @brief Brings libp2p up, once, on first use.
     *
     * Deliberately NOT done while registering the plugin. Registration happens
     * inside this module's `createNode`, which logos-core dispatches on the Qt
     * main thread -- and an outbound call from there cannot complete, because
     * acquiring a token makes capability_module call `informModuleToken` back
     * into this module, an inbound call needing the very thread we are
     * occupying. Every call made from there is rejected at dispatch (verified:
     * 80 probes over 20s, all rejected; the same calls succeed moments later
     * from the discovery worker thread).
     *
     * So this runs from the plugin's `start` verb instead, which logos-delivery
     * invokes on its own discovery thread with our main thread free -- and which
     * is also exactly when libp2p is first needed.
     *
     * Calls libp2p's `createNode`, because that is the only point at which its
     * kademlia can be given bootstrap peers: there is no call to add them
     * afterwards, and without peers it can neither store a provider record nor
     * answer a lookup. The config is the node's discovery requirements laid
     * over libp2p_module's own LIBP2P_MODULE_CONFIG (see discovery_config.h);
     * only the first kMaxBootstrapNodes peers are handed over.
     *
     * @return empty on success, otherwise a human-readable diagnostic.
     */
    std::string ensureBackend();

    /**
     * @brief Precondition for every entry point that talks to libp2p.
     *
     * Runs ensureBackend and turns a failure into the LD_DISCO_ERROR the ABI
     * wants, with the diagnostic in @p errBuf. Cheap once the backend is up --
     * ensureBackend returns on a bool -- so every verb can afford to ask.
     *
     * Every verb has to, because libp2p brings up a default node on its own
     * when it is called before ours exists, and bootstrap peers can only be
     * given at createNode. A verb that skipped this and arrived first would
     * leave libp2p with a kademlia that has no peers and no way to be given
     * any, which fails silently: lookups simply return nothing.
     *
     * @return LD_DISCO_OK when the backend is usable, LD_DISCO_ERROR otherwise.
     */
    int requireBackend(char* errBuf, size_t errBufLen);

    /// Criteria keys arrive as "service:<id>" / "topic:<pubsubTopic>" / "cap:<x>".
    /// libp2p wants a bare service id, so the "service:" prefix is stripped; other
    /// kinds pass through verbatim, which keeps advertise and lookup agreeing
    /// on one string without inventing a mapping libp2p could not honour.
    static std::string toServiceId(const char* key);
    Libp2pModule* libp2p_;
    std::string libp2pConfig_;
    bool backendReady_;
    bool nodeCreated_;
    std::atomic<int> inFlight_{0};
        ///< Entry points currently executing. Guarded by InFlightGuard so a
        ///< thread cannot leave without decrementing, and read by quiesce.
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
