// Stub of the codegen-generated libp2p_module client, for unit tests.
//
// The real header is emitted into generated_code/include at build time by
// logos-cpp-generator from metadata.json#optional_dependencies — the std/LIDL variant,
// which speaks nlohmann::json over the logos-protocol client and never touches
// Qt. The unit tests build the module sources without that codegen step, so
// this stand-in provides the subset of `Libp2pModule` that
// src/service_discovery_plugin.cpp calls, with inline no-op bodies. The one
// exception is createNode, which a test can make fail at the transport the way
// an absent libp2p_module does (see createNodeErrorCode).
//
// Keep the signatures in sync with the generated header when bumping the
// libp2p_module flake input.

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

    StdLogosResult createNode(const std::string&, logos::CallError* err = nullptr, int = 0)
    {
        if (err && !createNodeErrorCode.empty()) {
            err->code = createNodeErrorCode;
            err->message = "stub transport failure";
        }
        return StdLogosResult{};
    }
    StdLogosResult start(logos::CallError* = nullptr, int = 0) { return StdLogosResult{}; }
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
