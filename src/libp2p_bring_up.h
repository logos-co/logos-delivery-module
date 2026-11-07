#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

class Libp2pModule;

/**
 * @brief Brings this node's `libp2p_module` node up, once, for whichever
 * feature needs it first: plugin-hosted service discovery, or the RLN
 * membership allocator (`rln_gifter_module` dials the allocator through
 * `libp2p_module`'s `protocolRequest`, which needs a started node).
 *
 * One object per delivery node, shared by both: libp2p's `createNode` is
 * refused once a node exists, and a second `start` beside a running one runs
 * a second switch start and crashes libp2p_module on the next inbound
 * connection, so neither call may be issued twice.
 *
 * Never used on the Qt main thread, where outbound module calls are rejected.
 */
class Libp2pBringUp
{
public:
    /// What `ensure` reports for a `libp2p_module` that is not loaded.
    static const char* const kUnavailable;

    /**
     * @param libp2p Borrowed from `modules().libp2p_module`; null without a
     *        framework context.
     * @param config JSON object for libp2p_module's createNode (an empty
     *        object takes libp2p's defaults).
     * @param log Where the bring-up narrates itself (one line per call).
     */
    Libp2pBringUp(Libp2pModule* libp2p, std::string config,
                  std::function<void(const std::string&)> log);

    /**
     * @brief createNode (once) then start (once); a timed-out start counts as
     * issued, since libp2p's kademlia bootstrap outlives the call deadline. A
     * createNode libp2p refuses because a node already exists (the module
     * reloaded, another consumer brought it up) is probed with getNodeInfo
     * and, when it answers, taken as created and started: a second start on
     * a running node crashes libp2p_module.
     *
     * The libp2p calls run without the lock: a caller arriving meanwhile
     * waits a bounded time for the verdict in flight (the discovery plugin's
     * own call budget is 15 s) instead of blocking on the mutex past it.
     *
     * @return empty once the node is up; @ref kUnavailable when the module is
     *         absent; otherwise the diagnostics of the step that failed.
     */
    std::string ensure();

    /// Whether a call to `ensure` has succeeded.
    bool ready();

    /// The client the node is driven through; null without a framework context.
    Libp2pModule* module() const { return libp2p_; }

private:
    /// The libp2p calls, on local copies of the flags; `ensure` publishes them.
    std::string bringUp(bool& nodeCreated, bool& startIssued);

    std::mutex mutex_;
    std::condition_variable settled_;
    Libp2pModule* libp2p_;
    std::string config_;
    std::function<void(const std::string&)> log_;
    bool ready_{false};
    bool nodeCreated_{false};
    bool startIssued_{false};
    bool inProgress_{false};
};
