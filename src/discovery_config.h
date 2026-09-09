#pragma once

// Plugin-discovery setup from the node's own answer.
//
// The createNode config is logos-delivery's alone; this module forwards it and
// reads nothing discovery-related out of it. After createNode the node is
// asked (logosdelivery_get_discovery_requirements) whether service discovery
// is to come from a plugin and which DHT peers its configuration, presets
// included, resolved. This header turns that reply into the options the
// plugin hands to libp2p_module's createNode.
//
// libp2p_module keeps its own configuration channel, the LIBP2P_MODULE_CONFIG
// environment variable (inline JSON or a file path; see its README). A
// call-time createNode replaces those options wholesale on the libp2p side, so
// the plugin overlays the node's answer on that config rather than discarding
// it: listen addresses, transport and key stay the operator's.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace delivery_discovery {

struct PluginRequest {
    bool enabled{false};
    /// Complete JSON object text for libp2p_module's createNode. Empty unless
    /// `enabled`.
    std::string libp2pConfig;
};

namespace detail {

/// libp2p wants the peer id separate from the transport addresses
/// ({peerId, addrs[]}); nim-libp2p raiseAsserts on an unparseable entry rather
/// than returning an error, so the shape is checked before anything is sent.
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

/// libp2p_module's own configuration, read the way the module reads it
/// (LIBP2P_MODULE_CONFIG: inline JSON when it starts with '{', otherwise a
/// file path). An empty object when unset, unreadable or not a JSON object.
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

/// Turns the node's requirements reply
///   {"externalServiceDiscovery": bool, "bootstrapNodes": ["/dns4/.../p2p/..."]}
/// into the plugin request: `base` (libp2p_module's own config, see
/// libp2pEnvConfig) with the node's bootstrap peers and the two mount flags
/// set on top. Returns the failure reason, empty on success.
inline std::string fromRequirements(const std::string& reply, const nlohmann::json& base,
                                    PluginRequest& out)
{
    out = PluginRequest{};

    const nlohmann::json req = nlohmann::json::parse(reply, nullptr, false);
    if (!req.is_object() || !req.contains("externalServiceDiscovery")
        || !req["externalServiceDiscovery"].is_boolean()) {
        return "discovery requirements reply is not the expected JSON object";
    }
    if (!req["externalServiceDiscovery"].get<bool>()) {
        return {};
    }

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
    const std::string bad = detail::checkBootstrapNodes(nodes);
    if (!bad.empty()) {
        return bad;
    }

    nlohmann::json full = base.is_object() ? base : nlohmann::json::object();
    full["bootstrapNodes"] = nodes;
    full["mountKad"] = true;
    full["mountServiceDiscovery"] = true;

    out.enabled = true;
    out.libp2pConfig = full.dump();
    return {};
}

} // namespace delivery_discovery
