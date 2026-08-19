#pragma once

// Mirrors logos-libp2p-module src/plugin.h. One line per declaration, std types only: the parser drops the rest silently.

#include <cstdint>
#include <string>
#include <vector>

#include <logos_module_context.h>
#include <logos_result.h>

class ILibp2p {
public:
    StdLogosResult getNodeInfo(const std::string& field);
    StdLogosResult connectPeer(const std::string& peerId, const std::vector<std::string>& multiaddrs, int64_t timeoutMs);
    StdLogosResult connectedPeers(int64_t direction);
    StdLogosResult dial(const std::string& peerId, const std::string& proto);
    StdLogosResult pingPeer(const std::string& peerId, int64_t timeoutMs);

    StdLogosResult mountProtocol(const std::string& proto);
    StdLogosResult protocolRequest(const std::string& argsJson);
    StdLogosResult protocolAcceptStream(const std::string& argsJson);
    StdLogosResult streamReadLpJson(const std::string& argsJson);
    StdLogosResult streamWriteLpJson(const std::string& argsJson);
    StdLogosResult streamCloseJson(const std::string& argsJson);
    StdLogosResult streamReleaseJson(const std::string& argsJson);
};
