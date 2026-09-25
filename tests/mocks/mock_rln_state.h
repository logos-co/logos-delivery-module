// Shared test-observable state for the mocked liblogosdelivery poll ABI.
// Lets tests queue messages the library would send (events, RLN questions)
// and inspect what the module answered through logosdelivery_reverse_reply.
#pragma once

#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace delivery_test_rln {

// Queues a REVERSE_CALL as the library would ask it: `wire` is the reverse
// proc's wire name ("rln_generate_proof"), `args` its argument map. Returns
// the call id. The module sees it on its next pump (any method call drains
// the queue; so does a queued message arriving while a call waits).
uint64_t pushReverseCall(const char* wire, const nlohmann::json& args);

// Queues an EVENT with the given wire name and JSON payload.
void pushEvent(const char* wire, const nlohmann::json& payload);

// When set, the next method export queues this REVERSE_CALL *before* its own
// reply, so the reply can only be reached by serving the question first.
void interposeReverseCall(const char* wire, const nlohmann::json& args);

// The last request map any method export received, decoded from its CBOR.
extern nlohmann::json g_lastRequest;
extern std::string g_lastRequestMethod;

// The config JSON of the last logosdelivery_create_node, and whether the
// module declared it answers RLN questions (the request's `rlnPlugin`).
extern std::string g_lastCreateConfigJson;
extern bool g_lastCreateRlnPlugin;

// Last logosdelivery_reverse_reply.
extern uint64_t g_lastReplyCallId;
extern int g_lastReplyRet;
extern std::string g_lastReplyJson; // the CBOR text decoded, or the error text
extern int g_replyCount;

void resetRlnMockState();

} // namespace delivery_test_rln
