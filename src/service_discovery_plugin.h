#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <logosdelivery_service_discovery.h>
}

class LogosAPIClient;

/**
 * @brief Hosts logos-delivery's service-discovery plugin on top of libp2p_module.
 *
 * logos-delivery can delegate peer/service discovery to an external provider
 * through the C vtable declared in `logosdelivery_service_discovery.h`. This
 * class implements that vtable by forwarding each verb to `libp2p_module`'s
 * `disco*` API over logos-core-sdk.
 *
 * ## Why libp2p_module is not a metadata.json dependency
 *
 * Declaring it would make logos-core auto-load libp2p alongside delivery on
 * every run, and the generated `LogosModules` aggregate builds each declared
 * dependency's client eagerly in its constructor — there is no "declared but
 * skipped" state. Since discovery is opt-in per node config, the client is
 * resolved here at runtime instead. `LogosModuleContext::modules()` cannot help
 * with that -- the generated `LogosModules` holds one wrapper per declared
 * dependency and nothing else, so with none declared it exposes no `LogosAPI`
 * at all -- so this class builds its own `LogosAPIClient` against the
 * process-global transport, exactly as a generated wrapper would.
 *
 * A module may call any loaded module regardless of what it declares:
 * `dependencies` drives codegen, auto-load and load ordering, and authorizes
 * nothing (capability_module mints a token for any requesting pair without
 * consulting either list).
 *
 * The cost is real: calls are untyped, so method names are strings and the
 * `{success, value, error}` reply is decoded by hand, with nothing checking
 * either against libp2p's actual surface at build time. The sibling branch
 * `poc-apply-discovery-plugin-with-dep` declares the dependency and shows what
 * that buys — see its version of this file for the comparison.
 *
 * ## Threading
 *
 * Every entry point below is invoked on logos-delivery's own discovery worker
 * thread, never on the module's Qt main thread. That is safe: the SDK's
 * synchronous `invokeRemoteMethod` bottoms out in a `std::future` wait against
 * a connection driven by its own Asio thread, needs no Qt event loop on the
 * calling thread, and is internally serialized (atomic request ids, a
 * mutex-guarded pending map, a strand for writes). The asynchronous SDK lane
 * is deliberately *not* used: its completion is posted to the Qt main thread,
 * which this thread does not run.
 *
 * Calls from one node are serialized by that single worker thread, so this
 * object sees one verb at a time.
 */
class DeliveryServiceDiscoveryPlugin
{
public:
    /// Builds the libp2p_module client. Construct on the module's own thread
    /// (createNode): LogosAPIClient is a QObject whose constructor dials the
    /// target, while the vtable entry points below only ever use it.
    DeliveryServiceDiscoveryPlugin();
    ~DeliveryServiceDiscoveryPlugin();

    /**
     * @brief Brings libp2p_module up so the vtable has something to forward to.
     *
     * `libp2pConfig` is passed to libp2p's `createNode` when non-empty; an
     * already-created node is left alone (libp2p reports "node already
     * created", which is treated as success here — another module may own it).
     * `start` is then called unconditionally; libp2p's own `start` lazily
     * creates a default context if none exists.
     *
     * Failures are reported but not fatal: if libp2p is genuinely unusable,
     * the plugin's `start` verb fails and logos-delivery refuses to start the
     * node, which is one clear failure point instead of two.
     *
     * @return empty on success, otherwise a human-readable diagnostic.
     */
    std::string initialiseBackend(const std::string& libp2pConfig);

    /// The vtable to hand to logosdelivery_install_service_discovery_plugin.
    const LdServiceDiscoveryPlugin* vtable() const { return &vtable_; }

private:
    /// Forwards one `disco*` call and maps its reply onto an LD_DISCO_* code.
    /// On failure writes the diagnostic into errBuf. `outJson`, when non-null,
    /// receives a freshly allocated copy of the reply's JSON array.
    int forward(const char* method,
                const std::vector<std::string>& args,
                char** outJson,
                char* errBuf,
                size_t errBufLen);

    /// The libp2p_module client, built in the constructor. Never null.
    LogosAPIClient* client() { return client_.get(); }

    /// Criteria keys arrive as "svc:<id>" / "shard:<c>/<s>" / "cap:<x>".
    /// libp2p wants a bare service id, so the "svc:" prefix is stripped; other
    /// kinds pass through verbatim, which keeps advertise and lookup agreeing
    /// on one string without inventing a mapping libp2p could not honour.
    static std::string toServiceId(const char* key);

    std::unique_ptr<LogosAPIClient> client_;
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
