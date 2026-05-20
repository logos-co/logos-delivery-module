// Stub implementations for logos_events: methods.
// In the real build, the codegen generates delivery_module_events.cpp with
// bodies that route through LogosModuleContext::emitEventImpl_. For unit tests,
// the codegen doesn't run so we provide no-op stubs.

#include "delivery_module_plugin.h"

void DeliveryModuleImpl::messageSent(const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::messageError(const std::string&, const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::messagePropagated(const std::string&, const std::string&, int64_t) {}
void DeliveryModuleImpl::messageReceived(const std::string&, const std::string&, const std::vector<uint8_t>&, int64_t) {}
void DeliveryModuleImpl::connectionStateChanged(const std::string&, int64_t) {}
