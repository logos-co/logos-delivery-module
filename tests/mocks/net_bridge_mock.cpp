#include "net_bridge_mock.h"

#include <condition_variable>
#include <map>
#include <mutex>

#include "../../src/net_bridge.h"

namespace {

struct MockState {
    std::mutex mutex;
    std::condition_variable answered;
    delivery_test_net::Handler handler;
    size_t calls = 0;
    std::string registeredName;
    const LogosDeliveryNetBackendTable* table = nullptr;
    void* userData = nullptr;
    std::map<uint64_t, delivery_test_net::Answer> answers;
};

MockState& state()
{
    static MockState s;
    return s;
}

class MockTransport : public Libp2pTransport {
public:
    StdLogosResult call(const std::string& method, const nlohmann::json& args) override
    {
        delivery_test_net::Handler handler;
        {
            std::lock_guard<std::mutex> lock(state().mutex);
            ++state().calls;
            handler = state().handler;
        }
        if (!handler) {
            return {false, {}, "no mock handler for " + method};
        }
        return handler(method, args);
    }
};

} // namespace

std::shared_ptr<Libp2pTransport> makeLibp2pTransport(const LogosModuleContext& /*context*/,
                                                     const std::string& /*provider*/)
{
    return std::make_shared<MockTransport>();
}

extern "C" int logosdelivery_register_net_backend(const char* name,
                                                  const LogosDeliveryNetBackendTable* table,
                                                  void* userData)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().registeredName = name ? name : "";
    state().table = table;
    state().userData = userData;
    return 0;
}

extern "C" int logosdelivery_net_backend_respond(uint64_t requestId, int ok, const char* data, size_t len)
{
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().answers[requestId] =
            delivery_test_net::Answer{ok != 0, (data && len > 0) ? std::string(data, len) : std::string()};
    }
    state().answered.notify_all();
    return 0;
}

namespace delivery_test_net {

Handler providerWithPeerId(const std::string& peerId)
{
    return [peerId](const std::string& method, const nlohmann::json&) -> StdLogosResult {
        if (method == "getNodeInfo") {
            return {true, peerId, ""};
        }
        return {false, {}, "unexpected op: " + method};
    };
}

void reset()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().handler = nullptr;
    state().calls = 0;
    state().answers.clear();
}

void setHandler(Handler handler)
{
    std::lock_guard<std::mutex> lock(state().mutex);
    state().handler = std::move(handler);
}

size_t callCount()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().calls;
}

std::string registeredName()
{
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().registeredName;
}

bool submit(uint64_t requestId, const std::string& opJson)
{
    std::unique_lock<std::mutex> lock(state().mutex);
    const auto* table = state().table;
    void* userData = state().userData;
    lock.unlock();

    if (!table || !table->submit) {
        return false;
    }
    table->submit(requestId, opJson.c_str(), opJson.size(), userData);
    return true;
}

bool waitForAnswer(uint64_t requestId, Answer& out, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(state().mutex);
    const bool got = state().answered.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [requestId] { return state().answers.count(requestId) > 0; });
    if (!got) {
        return false;
    }
    out = state().answers[requestId];
    return true;
}

} // namespace delivery_test_net
