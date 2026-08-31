// Stub of the codegen-generated umbrella header, for unit tests.
//
// The real one is emitted into generated_code/ at build time and aggregates one
// typed client per metadata.json#dependencies entry into `LogosModules`, which
// LogosModuleContext::modules() returns. Note there is no `LogosAPI* api`
// member on this (std/LIDL) codegen path — each client is constructed with the
// origin module name instead — which is why a module that declares no
// dependencies gets an aggregate it cannot reach anything through.

#pragma once
#ifndef __logos_sdk_stub__
#define __logos_sdk_stub__

#include "libp2p_module_api.h"

struct LogosModules {
    LogosModules() : libp2p_module("delivery_module") {}
    Libp2pModule libp2p_module;
};

#endif /* __logos_sdk_stub__ */
