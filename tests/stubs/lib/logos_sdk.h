// Stub of the codegen-generated umbrella header, for unit tests.
//
// The real one is emitted into generated_code/ at build time and aggregates
// one typed client per metadata.json#optional_dependencies entry into `LogosModules`,
// which LogosModuleContext::modules() returns.

#pragma once
#ifndef __logos_sdk_stub__
#define __logos_sdk_stub__

#include "rln_module_api.h"
#include "libp2p_module_api.h"

struct LogosModules {
    LogosModules()
        : rln_module("delivery_module")
        , libp2p_module("delivery_module")
    {}
    RlnModule rln_module;
    Libp2pModule libp2p_module;
};

#endif /* __logos_sdk_stub__ */
