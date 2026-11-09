#pragma once

// Turns the node's discovery requirements into libp2p_module's createNode
// options, laid over LIBP2P_MODULE_CONFIG: a call-time createNode replaces
// libp2p's options wholesale, so the operator's settings are kept.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace delivery_discovery {

struct PluginRequest {
    /// The node asked for a plugin, whether or not its data is usable.
    bool requested{false};
    bool enabled{false};
    /// Complete JSON object text for libp2p_module's createNode, whether or
    /// not `enabled`: the allocator path runs on the same node.
    std::string libp2pConfig{"{}"};
};

namespace detail {

/// Checks the {peerId, addrs[]} shape up front: nim-libp2p asserts on a bad
/// entry instead of returning an error.
inline std::string checkBootstrapNodes(const nlohmann::json& nodes)
{
    if (!nodes.is_array()) {
        return "bootstrapNodes must be an array";
    }
    for (const auto& node : nodes) {
        if (!node.is_object() || !node.contains("peerId") || !node["peerId"].is_string()
            || node["peerId"].get<std::string>().empty() || !node.contains("addrs")
            || !node["addrs"].is_array() || node["addrs"].empty()) {
            return "each bootstrapNodes entry needs a peerId string and a non-empty addrs array";
        }
        for (const auto& addr : node["addrs"]) {
            if (!addr.is_string() || addr.get<std::string>().empty()) {
                return "bootstrapNodes addrs must be non-empty strings";
            }
        }
    }
    return {};
}

} // namespace detail

/// LIBP2P_MODULE_CONFIG as libp2p_module reads it (inline JSON or a file
/// path); an empty object when unset or unusable.
inline nlohmann::json libp2pEnvConfig()
{
    const char* cfg = std::getenv("LIBP2P_MODULE_CONFIG");
    if (!cfg || !*cfg) {
        return nlohmann::json::object();
    }
    std::string raw(cfg);
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || raw[first] != '{') {
        std::ifstream f(raw);
        if (!f) {
            fprintf(stderr, "DeliveryModuleImpl: cannot read LIBP2P_MODULE_CONFIG file %s\n",
                    raw.c_str());
            return nlohmann::json::object();
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        raw = ss.str();
    }
    const auto parsed = nlohmann::json::parse(raw, nullptr, false);
    if (!parsed.is_object()) {
        fprintf(stderr, "DeliveryModuleImpl: ignoring invalid LIBP2P_MODULE_CONFIG\n");
        return nlohmann::json::object();
    }
    return parsed;
}

/// Splits a "/.../p2p/<peerId>" multiaddr into libp2p's {peerId, addrs[]}
/// bootstrap entry. False when there is no peer id to split off.
inline bool splitBootstrapAddress(const std::string& multiaddr, nlohmann::json& out)
{
    constexpr const char* kMarker = "/p2p/";
    const auto pos = multiaddr.rfind(kMarker);
    if (pos == std::string::npos || pos == 0) {
        return false;
    }
    const std::string peerId = multiaddr.substr(pos + 5);
    if (peerId.empty() || peerId.find('/') != std::string::npos) {
        return false;
    }
    out = nlohmann::json{{"peerId", peerId},
                         {"addrs", nlohmann::json::array({multiaddr.substr(0, pos)})}};
    return true;
}

/// The {peerId, addrs[]} bootstrap entry for a "/.../p2p/<peerId>"
/// multiaddr; the failure reason, empty on success.
inline std::string bootstrapNodeFrom(const std::string& multiaddr, nlohmann::json& out)
{
    if (!splitBootstrapAddress(multiaddr, out)) {
        return "bootstrap node is not a /p2p/ multiaddr: " + multiaddr;
    }
    return detail::checkBootstrapNodes(nlohmann::json::array({out}));
}

/// Turns {"externalServiceDiscovery": bool, "bootstrapNodes": [...]} into the
/// plugin request. `libp2pConfig` is `base` with kad and service discovery
/// mounted and `bootstrapNodes` the node's peers, then `extraPeers`
/// ("/.../p2p/<peerId>" multiaddrs), then those of `base`, each peerId once,
/// at its first place; `enabled` is `externalServiceDiscovery`. Returns the
/// failure reason, empty on success; on failure `libp2pConfig` is an empty
/// object.
inline std::string fromRequirements(const std::string& reply, const nlohmann::json& base,
                                    PluginRequest& out,
                                    const std::vector<std::string>& extraPeers = {})
{
    out = PluginRequest{};

    const nlohmann::json req = nlohmann::json::parse(reply, nullptr, false);
    if (!req.is_object() || !req.contains("externalServiceDiscovery")
        || !req["externalServiceDiscovery"].is_boolean()) {
        return "discovery requirements reply is not the expected JSON object";
    }
    out.requested = req["externalServiceDiscovery"].get<bool>();

    nlohmann::json nodes = nlohmann::json::array();
    if (req.contains("bootstrapNodes")) {
        if (!req["bootstrapNodes"].is_array()) {
            return "discovery requirements: bootstrapNodes is not an array";
        }
        for (const auto& entry : req["bootstrapNodes"]) {
            nlohmann::json node;
            if (!entry.is_string() || !splitBootstrapAddress(entry.get<std::string>(), node)) {
                return "discovery requirements: bootstrap node is not a /p2p/ multiaddr: "
                       + (entry.is_string() ? entry.get<std::string>() : entry.dump());
            }
            nodes.push_back(node);
        }
    }
    std::string bad = detail::checkBootstrapNodes(nodes);
    if (!bad.empty()) {
        return bad;
    }

    const auto addOnce = [&nodes](const nlohmann::json& node) {
        const bool named = std::any_of(nodes.begin(), nodes.end(), [&node](const nlohmann::json& n) {
            return n["peerId"] == node["peerId"];
        });
        if (!named) {
            nodes.push_back(node);
        }
    };
    for (const auto& multiaddr : extraPeers) {
        nlohmann::json node;
        bad = bootstrapNodeFrom(multiaddr, node);
        if (!bad.empty()) {
            return bad;
        }
        addOnce(node);
    }

    nlohmann::json full = base.is_object() ? base : nlohmann::json::object();
    if (full.contains("bootstrapNodes")) {
        bad = detail::checkBootstrapNodes(full["bootstrapNodes"]);
        if (!bad.empty()) {
            return "LIBP2P_MODULE_CONFIG: " + bad;
        }
        for (const auto& own : full["bootstrapNodes"]) {
            addOnce(own);
        }
    }
    full["bootstrapNodes"] = nodes;
    full["mountKad"] = true;
    full["mountServiceDiscovery"] = true;

    out.enabled = out.requested;
    out.libp2pConfig = full.dump();
    return {};
}

} // namespace delivery_discovery
