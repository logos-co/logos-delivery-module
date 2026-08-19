// Test double for the provider and for the ABI, in place of src/libp2p_module_transport.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "../../src/net_bridge.h"

namespace delivery_test_net {

struct Answer {
    bool ok = false;
    std::string data;
};

using Handler = std::function<StdLogosResult(const std::string& method, const nlohmann::json& args)>;

// Answers getNodeInfo with `peerId` and fails every other op.
Handler providerWithPeerId(const std::string& peerId);

void reset();
void setHandler(Handler handler);

size_t callCount();

// Empty until the bridge registers; registration lasts for the process.
std::string registeredName();

// Feeds one request through the registered table, as liblogosdelivery would.
bool submit(uint64_t requestId, const std::string& opJson);

bool waitForAnswer(uint64_t requestId, Answer& out, int timeoutMs);

} // namespace delivery_test_net
