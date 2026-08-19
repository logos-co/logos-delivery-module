#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include <logos_module_context.h>
#include <logos_result.h>

// C ABI of library/network_bridge_api.nim; see docs/libp2p-provider.md.
#define LOGOSDELIVERY_NET_BACKEND_ABI_VERSION 1
#ifdef __cplusplus
extern "C" {
#endif

// Runs on a chronos thread, returns at once; respond() may answer inside it.
typedef void (*LogosDeliveryNetSubmitFn)(uint64_t requestId,
                                         const char* opJson,
                                         size_t opLen,
                                         void* userData);

typedef struct {
    uint32_t version;
    LogosDeliveryNetSubmitFn submit;
} LogosDeliveryNetBackendTable;

typedef int (*LogosDeliveryRegisterNetBackendFn)(const char* name,
                                                 const LogosDeliveryNetBackendTable* table,
                                                 void* userData);

// ok != 0 carries the result JSON, ok == 0 the error text.
typedef int (*LogosDeliveryNetBackendRespondFn)(uint64_t requestId,
                                                int ok,
                                                const char* data,
                                                size_t len);

#ifdef __cplusplus
} // extern "C"
#endif

// The module build implements this over the interface dependency, tests in-process.
class Libp2pTransport {
public:
    virtual ~Libp2pTransport() = default;
    virtual StdLogosResult call(const std::string& method, const nlohmann::json& args) = 0;

    // Cleared before the owner dies: a worker parked in a long poll may outlive it.
    std::atomic<bool> alive{true};
};

// The one module that provides the libp2p interface; logos-core routes by this name.
inline constexpr const char* kLibp2pModuleName = "libp2p_module";

// The context carries modules(), which is where the interface binding lives.
std::shared_ptr<Libp2pTransport> makeLibp2pTransport(const LogosModuleContext& context);

class Libp2pNetBridge {
public:
    // Lives for the process: the C ABI has no unregister, so a callback can land late.
    static std::shared_ptr<Libp2pNetBridge> instance();

    // Answers with the provider's peer id, and refuses a provider another node already drives.
    StdLogosResult start(std::shared_ptr<Libp2pTransport> transport);
    void stop(const std::shared_ptr<Libp2pTransport>& transport);

private:
    Libp2pNetBridge();

    StdLogosResult call(const std::string& method, const nlohmann::json& args);

    struct Job {
        uint64_t requestId;
        std::string opJson;
    };

    static void onSubmit(uint64_t requestId, const char* opJson, size_t opLen, void* userData);

    void enqueue(Job job);
    void workerLoop();
    void runJob(const Job& job);
    void respond(uint64_t requestId, bool ok, const std::string& data);

    LogosDeliveryNetBackendTable m_table{};

    std::mutex m_mutex;
    std::condition_variable m_wakeup;
    std::deque<Job> m_queue;
    std::shared_ptr<Libp2pTransport> m_transport;
    size_t m_threads = 0;
    size_t m_idle = 0;
    bool m_registered = false;
};
