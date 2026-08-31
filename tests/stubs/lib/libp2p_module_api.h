// Stub of the codegen-generated libp2p_module client, for unit tests.
//
// The real header is emitted into generated_code/include at build time by
// logos-cpp-generator from metadata.json#dependencies — the std/LIDL variant,
// which speaks nlohmann::json over the logos-protocol client and never touches
// Qt. The unit tests build the module sources without that codegen step, so
// this stand-in provides the subset of `Libp2pModule` that
// src/service_discovery_plugin.cpp calls, with inline no-op bodies: nothing
// here is exercised, it only has to compile and link.
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

    StdLogosResult createNode(const std::string&, logos::CallError* = nullptr)
    { return StdLogosResult{}; }
    StdLogosResult start(logos::CallError* = nullptr) { return StdLogosResult{}; }

    StdLogosResult discoStart(logos::CallError* = nullptr) { return StdLogosResult{}; }
    StdLogosResult discoStop(logos::CallError* = nullptr) { return StdLogosResult{}; }
    StdLogosResult discoStartAdvertising(const std::string&, const std::string&,
                                         const std::string&,
                                         logos::CallError* = nullptr)
    { return StdLogosResult{}; }
    StdLogosResult discoStopAdvertising(const std::string&, logos::CallError* = nullptr)
    { return StdLogosResult{}; }
    StdLogosResult discoRegisterInterest(const std::string&, logos::CallError* = nullptr)
    { return StdLogosResult{}; }
    StdLogosResult discoUnregisterInterest(const std::string&, logos::CallError* = nullptr)
    { return StdLogosResult{}; }
    StdLogosResult discoLookup(const std::string&, const std::string&,
                               logos::CallError* = nullptr)
    { return StdLogosResult{}; }
    StdLogosResult discoRandomLookup(logos::CallError* = nullptr)
    { return StdLogosResult{}; }
};

#endif /* __libp2p_module_api_stub__ */
