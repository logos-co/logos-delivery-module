// Stub of the generated libp2p_module client for unit tests: the subset
// service_discovery_plugin.cpp calls, as no-ops except createNode and start
// (see the knobs below). Keep signatures in sync with the generated header.

#pragma once
#ifndef __libp2p_module_api_stub__
#define __libp2p_module_api_stub__

#include <string>

#include <logos_call_error.h>
#include <logos_result.h>

class Libp2pModule {
public:
    explicit Libp2pModule(const std::string& /*origin*/) {}

    /// When non-empty, createNode fails at the transport with this code, e.g.
    /// "object_unavailable" -- what the real client reports for a module that
    /// is not loaded.
    static inline std::string createNodeErrorCode;
    /// When true, createNode and start succeed; otherwise they answer with the
    /// default, failed, result.
    static inline bool bringUpSucceeds = false;
    /// When non-empty, start answers success=false with this error -- how the
    /// real client reports its own call deadline: "Failed to start libp2p: timeout".
    static inline std::string startError;
    /// When non-empty, start fails at the transport with this code.
    static inline std::string startErrorCode;
    /// Every start call, so a test can tell whether one was issued again.
    static inline int startCalls = 0;

    static void reset()
    {
        createNodeErrorCode.clear();
        bringUpSucceeds = false;
        startError.clear();
        startErrorCode.clear();
        startCalls = 0;
    }

    StdLogosResult createNode(const std::string&, logos::CallError* err = nullptr, int = 0)
    {
        if (err && !createNodeErrorCode.empty()) {
            err->code = createNodeErrorCode;
            err->message = "stub transport failure";
        }
        return StdLogosResult{bringUpSucceeds, {}, ""};
    }
    StdLogosResult start(logos::CallError* err = nullptr, int = 0)
    {
        ++startCalls;
        if (err && !startErrorCode.empty()) {
            err->code = startErrorCode;
            err->message = "call to 'libp2p_module.start' timed out after 20000ms";
            return StdLogosResult{};
        }
        if (!startError.empty()) {
            return StdLogosResult{false, {}, startError};
        }
        return StdLogosResult{bringUpSucceeds, {}, ""};
    }
    StdLogosResult getNodeInfo(const std::string&, logos::CallError* = nullptr, int = 0)
    { return StdLogosResult{}; }

    StdLogosResult discoStart(logos::CallError* = nullptr, int = 0) { return StdLogosResult{}; }
    StdLogosResult discoStop(logos::CallError* = nullptr, int = 0) { return StdLogosResult{}; }
    StdLogosResult discoStartAdvertising(const std::string&, const std::string&,
                                         const std::string&,
                                         logos::CallError* = nullptr, int = 0)
    { return StdLogosResult{}; }
    StdLogosResult discoStopAdvertising(const std::string&, logos::CallError* = nullptr, int = 0)
    { return StdLogosResult{}; }
    StdLogosResult discoRegisterInterest(const std::string&, logos::CallError* = nullptr, int = 0)
    { return StdLogosResult{}; }
    StdLogosResult discoUnregisterInterest(const std::string&, logos::CallError* = nullptr, int = 0)
    { return StdLogosResult{}; }
    StdLogosResult discoLookup(const std::string&, const std::string&,
                               logos::CallError* = nullptr, int = 0)
    { return StdLogosResult{}; }
    StdLogosResult discoRandomLookup(logos::CallError* = nullptr, int = 0)
    { return StdLogosResult{}; }
};

#endif /* __libp2p_module_api_stub__ */
