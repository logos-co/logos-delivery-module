// Stub of the codegen-generated umbrella header, for unit tests.
//
// The real one is emitted into generated_code/ at build time and aggregates
// one typed client per metadata.json#dependencies entry into `LogosModules`,
// which LogosModuleContext::modules() returns.

#pragma once
#ifndef __logos_sdk_stub__
#define __logos_sdk_stub__

#include "liblogos_rln_module_api.h"

struct LogosModules {
    LogosModules() : liblogos_rln_module("delivery_module") {}
    LiblogosRlnModule liblogos_rln_module;
};

#endif /* __logos_sdk_stub__ */
