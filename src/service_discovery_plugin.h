#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

class Libp2pModule;
class LogosDeliveryCtx; // liblogosdelivery.hpp

/**
 * @brief Hosts logos-delivery's external service discovery on libp2p_module.
 *
 * The node calls its host through nim-ffi reverse calls, one per verb
 * (`sd_start`, `sd_lookup`, ...): registerOn() installs an implementation of
 * each with the binding's `setSd<Verb>Impl`, and every one forwards to
 * libp2p_module's `disco*` API. libp2p_module is an optional dependency: a
 * node configured for external discovery without it fails its start (see
 * ensureBackend).
 *
 * The replies are asynchronous. An implementation runs on a nim-ffi reverse
 * worker; it queues the verb, with its decoded arguments and its call token,
 * for this object's executor thread and returns at once. The executor runs the
 * verbs one at a time, in order, and answers each through its token. The SDK's
 * synchronous calls need no Qt event loop there; the `*Async` variants are
 * unused because they complete on the Qt main thread.
 */
class DeliveryServiceDiscoveryPlugin
{
public:
    /// The result of one verb. `records` is set by the lookups only: libp2p's
    /// peer records, `[{peerId, seqNo, addrs, services:[{id, data}]}]` with
    /// each service's `data` base64.
    struct Outcome {
        bool ok = true;
        std::string error;
        nlohmann::json records;
    };

    /**
     * @param libp2p Borrowed from `modules().libp2p_module`; null without a
     *        framework context.
     * @param libp2pConfig JSON object for libp2p_module's createNode (see
     *        discovery_config.h).
     */
    DeliveryServiceDiscoveryPlugin(Libp2pModule* libp2p, std::string libp2pConfig);
    ~DeliveryServiceDiscoveryPlugin();

    DeliveryServiceDiscoveryPlugin(const DeliveryServiceDiscoveryPlugin&) = delete;
    DeliveryServiceDiscoveryPlugin& operator=(const DeliveryServiceDiscoveryPlugin&) = delete;

    /**
     * @brief Registers an implementation of every `sd_*` reverse call on the
     *        node's context, and starts the executor.
     *
     * After `ctx_create`, before `start_node`: the node checks at start that
     * every one is there.
     *
     * @param node The node's context; must outlive this object's executor.
     * @return empty on success, otherwise which registration was refused.
     */
    std::string registerOn(LogosDeliveryCtx& node);

    /**
     * @brief Stops the executor; queued verbs are answered with an error.
     *
     * Called after `ctx_destroy`, which unregisters the implementations, so no
     * new verb can arrive. A verb still inside libp2p past @p timeout keeps the
     * executor thread; the owner then leaks this object rather than free it
     * under that thread.
     *
     * @return true when the executor exited, false on timeout.
     */
    bool quiesce(std::chrono::milliseconds timeout);

    // --- the verbs, run on the executor; public for the unit tests ----------
    Outcome start();
    Outcome stop();
    Outcome lookup(const std::string& key, int64_t limit);
    Outcome randomLookup();
    Outcome startAdvertising(const std::string& key, const std::vector<uint8_t>& data,
                             const std::vector<uint8_t>& record);
    Outcome stopAdvertising(const std::string& key);
    Outcome registerInterest(const std::string& key);
    Outcome unregisterInterest(const std::string& key);

private:
    /// One verb the node is waiting on: the verb with its arguments bound,
    /// and how to answer it. `reply` holds the binding's call token, which is
    /// safe to use from any thread, also after the context is gone.
    struct Call {
        std::function<Outcome()> run;
        std::function<void(const Outcome&)> reply;
    };

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
     * @brief Runs ensureBackend and maps a failure to a failed Outcome.
     *
     * Every verb calls it: one reaching libp2p first would leave it on a
     * default node with no bootstrap peers, and lookups would silently find
     * nothing.
     */
    bool requireBackend(Outcome& out);

    /// Strips the "service:" prefix libp2p does not expect; other criteria
    /// kinds pass through unchanged.
    static std::string toServiceId(const std::string& key);

    /// Queues a decoded call; false once the executor is stopping.
    bool enqueue(Call call);
    void runExecutor();


    Libp2pModule* libp2p_;
    std::string libp2pConfig_;
    bool backendReady_;
    bool nodeCreated_;
    bool startIssued_; ///< libp2p start succeeded or timed out; never issue it again.

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Call> queue_;
    bool stopping_ = false;
    std::atomic<bool> executorExited_{false};
    std::thread executor_;
};
