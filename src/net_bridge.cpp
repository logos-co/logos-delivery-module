// _GNU_SOURCE must precede every libc header, or dlfcn.h hides RTLD_DEFAULT.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>

#include "net_bridge.h"

#include <chrono>
#include <thread>
#include <utility>

namespace {

constexpr size_t kMaxWorkers = 16;
// A wedged provider blocks every worker, and the library keeps submitting.
constexpr size_t kMaxQueuedJobs = 4096;
constexpr std::chrono::seconds kWorkerIdleTimeout{30};

// The global scope is searched last, so a test double can stand in for the library.
void* deliverySymbol(const char* symbol) {
    for (const char* soname : {"liblogosdelivery.so", "liblogosdelivery.dylib"}) {
        void* lib = dlopen(soname, RTLD_LAZY | RTLD_NOLOAD);
        if (!lib) continue;
        void* found = dlsym(lib, symbol);
        dlclose(lib);
        if (found) return found;
    }
    return dlsym(RTLD_DEFAULT, symbol);
}

LogosDeliveryRegisterNetBackendFn registerFn() {
    static auto fn = reinterpret_cast<LogosDeliveryRegisterNetBackendFn>(
        deliverySymbol("logosdelivery_register_net_backend"));
    return fn;
}

LogosDeliveryNetBackendRespondFn respondFn() {
    static auto fn = reinterpret_cast<LogosDeliveryNetBackendRespondFn>(
        deliverySymbol("logosdelivery_net_backend_respond"));
    return fn;
}

} // namespace

Libp2pNetBridge::Libp2pNetBridge()
{
    m_table.version = LOGOSDELIVERY_NET_BACKEND_ABI_VERSION;
    m_table.submit = &Libp2pNetBridge::onSubmit;
}

std::shared_ptr<Libp2pNetBridge> Libp2pNetBridge::instance()
{
    static const auto bridge = std::shared_ptr<Libp2pNetBridge>(new Libp2pNetBridge());
    return bridge;
}

// Answers with the provider's peer id, so the caller can check the shared identity.
StdLogosResult Libp2pNetBridge::start(std::shared_ptr<Libp2pTransport> transport)
{
    if (!transport) {
        return {false, {}, "no libp2p transport"};
    }
    if (!registerFn() || !respondFn()) {
        return {false, {}, "liblogosdelivery has no net backend ABI: libp2pProvider needs a newer library"};
    }

    const std::string provider = kLibp2pModuleName;
    auto peerId = transport->call("getNodeInfo", nlohmann::json{{"field", "PeerId"}});
    if (!peerId.success) {
        return {false, {}, provider + " is not reachable: " + peerId.error};
    }
    if (!peerId.value.is_string() || peerId.value.get<std::string>().empty()) {
        return {false, {}, provider + " returned no peer id"};
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_transport) {
        return {false, {}, provider + " already drives another delivery node"};
    }
    if (!m_registered) {
        if (registerFn()(kLibp2pModuleName, &m_table, this) != 0) {
            return {false, {}, "failed to register net backend " + provider};
        }
        m_registered = true;
    }
    m_transport = std::move(transport);
    return {true, peerId.value};
}

// The bridge outlives every node that uses it, so only its own node may stop it.
void Libp2pNetBridge::stop(const std::shared_ptr<Libp2pTransport>& transport)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (transport && m_transport == transport) {
        m_transport.reset();
    }
}

StdLogosResult Libp2pNetBridge::call(const std::string& method, const nlohmann::json& args)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    auto transport = m_transport;
    lock.unlock();

    if (!transport) {
        return {false, {}, "libp2p net bridge is stopped"};
    }
    return transport->call(method, args);
}

void Libp2pNetBridge::onSubmit(uint64_t requestId, const char* opJson, size_t opLen, void* userData)
{
    auto* self = static_cast<Libp2pNetBridge*>(userData);
    if (!self) return;
    self->enqueue(Job{requestId, (opJson && opLen > 0) ? std::string(opJson, opLen) : std::string()});
}

void Libp2pNetBridge::enqueue(Job job)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_queue.size() >= kMaxQueuedJobs) {
        lock.unlock();
        respond(job.requestId, false, "libp2p net bridge queue is full");
        return;
    }
    m_queue.push_back(std::move(job));
    m_wakeup.notify_one();

    // An idle worker still counts as idle until it wakes, so earlier jobs claim it first.
    if (m_queue.size() <= m_idle || m_threads >= kMaxWorkers) {
        return;
    }

    ++m_threads;
    lock.unlock();
    std::thread([this] { workerLoop(); }).detach();
}

void Libp2pNetBridge::workerLoop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_queue.empty()) {
                ++m_idle;
                m_wakeup.wait_for(lock, kWorkerIdleTimeout, [this] { return !m_queue.empty(); });
                --m_idle;
            }
            if (m_queue.empty()) {
                --m_threads;
                return;
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        runJob(job);
    }
}

// Every op gets exactly one answer, and an escaping exception would end the worker.
void Libp2pNetBridge::runJob(const Job& job)
{
    try {
        // find() on a discarded or non-object value returns end(), so one guard covers both.
        const auto op = nlohmann::json::parse(job.opJson, nullptr, false);
        const auto method = op.find("op");
        if (method == op.end() || !method->is_string()) {
            respond(job.requestId, false, "op is missing or the JSON is invalid");
            return;
        }

        const auto args = op.find("args");
        const auto res = call(method->get<std::string>(),
                              (args != op.end() && args->is_object()) ? *args : nlohmann::json::object());
        respond(job.requestId, res.success, res.success ? res.value.dump() : res.error);
    } catch (const std::exception& e) {
        respond(job.requestId, false, std::string("op failed: ") + e.what());
    } catch (...) {
        respond(job.requestId, false, "op failed");
    }
}

void Libp2pNetBridge::respond(uint64_t requestId, bool ok, const std::string& data)
{
    if (auto fn = respondFn()) fn(requestId, ok ? 1 : 0, data.c_str(), data.size());
}
