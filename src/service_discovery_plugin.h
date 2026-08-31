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
     */
    explicit DeliveryServiceDiscoveryPlugin(Libp2pModule* libp2p);

    /**
     * @brief Brings libp2p_module up so the vtable has something to forward to.
     *
     * `libp2pConfig` is passed to libp2p's `createNode` when non-empty; an
     * already-created node is left alone (libp2p reports "node already
     * created", treated as success here -- another module may own it). `start`
     * is then called unconditionally; libp2p's own `start` lazily creates a
     * default context if none exists.
     *
     * @return empty on success, otherwise a human-readable diagnostic.
     */
    std::string initialiseBackend(const std::string& libp2pConfig);

    /// The vtable to hand to logosdelivery_install_service_discovery_plugin.
    const LdServiceDiscoveryPlugin* vtable() const { return &vtable_; }

private:
    /// Criteria keys arrive as "svc:<id>" / "shard:<c>/<s>" / "cap:<x>".
    /// libp2p wants a bare service id, so the "svc:" prefix is stripped; other
    /// kinds pass through verbatim, which keeps advertise and lookup agreeing
    /// on one string without inventing a mapping libp2p could not honour.
    static std::string toServiceId(const char* key);

    Libp2pModule* libp2p_;
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
