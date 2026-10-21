// Shared test-observable state for the mocked nim-ffi reverse-call surface the
// service discovery host registers on (logosdelivery_set_sd_*_impl). Lets a test
// play the node: invoke a registered implementation and wait for its reply.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace delivery_test_reverse {

/// What the host answered one call with.
struct Reply {
    bool answered = false;
    bool ok = false;
    std::string error;
    void* ctx = nullptr;
    std::vector<std::string> peers;        ///< peer ids of a lookup reply
    std::vector<uint8_t> firstServiceData; ///< data of the first peer's first service
    bool onOtherThread = false;            ///< answered off the invoking thread
};

/// Forgets every registration and reply.
void reset();

/// How many wires have an implementation registered.
int registeredCount();

/// Calls the implementation registered for `wire` the way the node would:
/// key-taking wires get `key` (and sd_lookup `limit`) as their arguments.
void invoke(const std::string& wire, void* ctx, uint64_t callId, const std::string& key,
            int64_t limit);

/// Waits up to 5s for the reply to `callId`.
Reply waitReply(uint64_t callId);

} // namespace delivery_test_reverse
