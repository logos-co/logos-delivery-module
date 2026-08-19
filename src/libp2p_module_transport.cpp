#include "net_bridge.h"

#include <vector>

#include "logos_sdk.h"

namespace {

std::vector<std::string> stringList(const nlohmann::json& value)
{
    std::vector<std::string> out;
    if (!value.is_array()) {
        return out;
    }

    for (const auto& item : value) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

class Libp2pModuleTransport : public Libp2pTransport
{
public:
    // The aggregate belongs to the provider, not to the impl a late worker outlives.
    Libp2pModuleTransport(const LogosModuleContext& context, std::string provider)
        : m_modules(context.isContextReady() ? &context.modules() : nullptr),
          m_provider(std::move(provider)) {}

    StdLogosResult call(const std::string& method, const nlohmann::json& args) override
    {
        if (!alive.load() || m_modules == nullptr) {
            return {false, {}, "libp2p transport is not available"};
        }
        auto peer = m_modules->bind_libp2p(m_provider);

        const std::string peerId = args.value("peerId", std::string{});
        const std::string proto = args.value("proto", std::string{});
        const int64_t timeoutMs = args.value("timeoutMs", int64_t{0});

        // This list is the allow-list: nothing else reaches the provider.
        if (method == "getNodeInfo") return peer.getNodeInfo(args.value("field", std::string{}));
        if (method == "connectPeer") return peer.connectPeer(peerId, stringList(args.value("multiaddrs", nlohmann::json::array())), timeoutMs);
        if (method == "connectedPeers") return peer.connectedPeers(args.value("direction", int64_t{0}));
        if (method == "dial") return peer.dial(peerId, proto);
        if (method == "pingPeer") return peer.pingPeer(peerId, timeoutMs);
        if (method == "mountProtocol") return peer.mountProtocol(proto);
        if (method == "protocolRequest") return peer.protocolRequest(args.dump());
        if (method == "protocolAcceptStream") return peer.protocolAcceptStream(args.dump());
        if (method == "streamReadLp") return peer.streamReadLpJson(args.dump());
        if (method == "streamWriteLp") return peer.streamWriteLpJson(args.dump());
        if (method == "streamClose") return peer.streamCloseJson(args.dump());
        if (method == "streamRelease") return peer.streamReleaseJson(args.dump());
        return {false, {}, "unknown libp2p op: " + method};
    }

private:
    LogosModules* m_modules;
    std::string m_provider;
};

} // namespace

std::shared_ptr<Libp2pTransport> makeLibp2pTransport(const LogosModuleContext& context,
                                                     const std::string& provider)
{
    return std::make_shared<Libp2pModuleTransport>(context, provider);
}
