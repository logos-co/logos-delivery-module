#pragma once

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
 * ## The dependency is declared, so the calls are typed
 *
 * `libp2p_module` is listed in `metadata.json#dependencies`, which is the only
 * way to get a typed client: at build time logos-module-builder resolves the
 * dependency from the like-named flake input, and logos-cpp-generator emits
 * `libp2p_module_api.h` (a `Libp2pModule` class) plus the `LogosModules`
 * aggregate behind `LogosModuleContext::modules()`. logos-core itself generates
 * nothing -- it only loads the built plugins at runtime.
 *
 * What declaring it costs, and why the sibling branch
 * `poc-apply-discovery-plugin` does not: logos-core then auto-loads
 * libp2p_module alongside this one on every run, and `LogosModules` constructs
 * each declared dependency's client eagerly in its constructor. There is no
 * "declared but skipped" state, so discovery stops being decidable per node
 * config. Declaring it buys codegen, auto-load and load ordering -- and
 * authorizes nothing, since capability_module mints a token for any requesting
 * pair without consulting either list.
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
     * @param libp2pConfig Optional JSON object merged over the default libp2p
     *        options; see @ref ensureBackend.
     */
    DeliveryServiceDiscoveryPlugin(Libp2pModule* libp2p, std::string libp2pConfig);

    /// The vtable to hand to logosdelivery_install_service_discovery_plugin.
    const LdServiceDiscoveryPlugin* vtable() const { return &vtable_; }

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
     * answer a lookup. The config is the hardcoded logos.dev entry nodes with
     * `libp2pConfig` merged over the top, so a node config can override or clear
     * them. An already-created node is left alone -- libp2p reports "node
     * already created" and another module may legitimately own it.
     *
     * @return empty on success, otherwise a human-readable diagnostic.
     */
    std::string ensureBackend();

    /// Criteria keys arrive as "svc:<id>" / "shard:<c>/<s>" / "cap:<x>".
    /// libp2p wants a bare service id, so the "svc:" prefix is stripped; other
    /// kinds pass through verbatim, which keeps advertise and lookup agreeing
    /// on one string without inventing a mapping libp2p could not honour.
    static std::string toServiceId(const char* key);

    Libp2pModule* libp2p_;
    std::string libp2pConfig_;
    bool backendReady_;
    bool nodeCreated_;
    std::string backendFailure_;
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
