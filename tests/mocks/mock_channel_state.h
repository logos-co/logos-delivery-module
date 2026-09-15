#pragma once

#include <cstdint>
#include <functional>
#include <string>

// What the mocks record for the channel-cipher tests: the three cipher fields
// the module handed to logosdelivery_channel_create, and a programmable stand-in
// for the module the relay calls out to.
namespace delivery_test_cipher {

extern uint64_t g_encryptFn;
extern uint64_t g_decryptFn;
extern uint64_t g_userData;

// Answers one lp_invoke_async. Return false to fail the call; otherwise fill
// `outJson` with the reply JSON (a JSON string for a cipher method).
using LpHandler = std::function<bool(const std::string& method,
                                     const std::string& argsJson,
                                     std::string& outJson)>;

extern LpHandler g_lpHandler;
// What lp_get_methods answers. Empty means "cannot introspect".
extern std::string g_methodsJson;
extern bool g_clientCreateFails;
extern std::string g_lastTarget;
extern std::string g_lastOrigin;

void reset();

} // namespace delivery_test_cipher
