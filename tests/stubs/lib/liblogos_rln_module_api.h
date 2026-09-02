// Stub of the codegen-generated liblogos_rln_module client, for unit tests.
//
// The real header is emitted at build time by logos-cpp-generator from
// metadata.json#dependencies (the std/LIDL variant). The unit tests build the
// module sources without that codegen step, so this stand-in provides the
// subset of `LiblogosRlnModule` that src/rln_bridge.cpp calls, with inline
// no-op bodies: a default StdLogosResult is success=false with no error text,
// which the bridge treats as a transport failure.
//
// Keep the signatures in sync with the generated header when bumping the
// liblogos_rln_module flake input.

#pragma once
#ifndef __liblogos_rln_module_api_stub__
#define __liblogos_rln_module_api_stub__

#include <string>

#include <logos_call_error.h>
#include <logos_result.h>

class LiblogosRlnModule {
public:
    explicit LiblogosRlnModule(const std::string&) {}

    StdLogosResult start(const std::string&, logos::CallError* = nullptr)
    {
        return {};
    }
    StdLogosResult stop(logos::CallError* = nullptr) { return {}; }
    StdLogosResult get_epoch_quota(const std::string&, const std::string&,
                                   const std::string&, logos::CallError* = nullptr)
    {
        return {};
    }
    StdLogosResult validate_proof(const std::string&, const std::string&,
                                  const std::string&, const std::string&,
                                  const std::string&, logos::CallError* = nullptr)
    {
        return {};
    }
};

#endif /* __liblogos_rln_module_api_stub__ */
