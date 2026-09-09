#pragma once

// Plugin-discovery setup from the node's own answer.
//
// The config file is one document for both libraries. Everything in it
// belongs to logos-delivery except the top-level `libp2pConfig` object, this
// module's overrides for libp2p_module's createNode; logos-delivery's parser
// rejects keys it does not know, so that object is taken out before the
// config is forwarded.
//
// Whether a plugin is wanted, and which DHT peers to bootstrap from, is not
// read from the config here: after createNode the node is asked
// (logosdelivery_get_discovery_requirements), so presets and every config
// shape resolve on the side that owns them. This header turns that reply plus
// the overrides into the libp2p config the plugin hands over.

#include <cctype>
#include <initializer_list>
#include <optional>
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

inline std::string lower(std::string s)
{
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// Case-insensitive key lookup; returns the key as spelled in the config.
inline std::optional<std::string> findKey(const nlohmann::json& obj,
                                          std::initializer_list<const char*> names)
{
    if (!obj.is_object()) {
        return std::nullopt;
    }
    for (const auto& entry : obj.items()) {
        const std::string key = lower(entry.key());
        for (const char* name : names) {
            if (key == name) {
                return entry.key();
            }
        }
    }
    return std::nullopt;
}

/// libp2p wants the peer id separate from the transport addresses
/// ({peerId, addrs[]}); nim-libp2p raiseAsserts on an unparseable entry rather
/// than returning an error, so the shape is checked before anything is sent.
inline std::string checkBootstrapNodes(const nlohmann::json& nodes)
{
    if (!nodes.is_array()) {
        return "libp2pConfig.bootstrapNodes must be an array";
    }
    for (const auto& node : nodes) {
        if (!node.is_object() || !node.contains("peerId") || !node["peerId"].is_string()
            || node["peerId"].get<std::string>().empty() || !node.contains("addrs")
            || !node["addrs"].is_array() || node["addrs"].empty()) {
            return "each libp2pConfig.bootstrapNodes entry needs a peerId string and a "
                   "non-empty addrs array";
        }
        for (const auto& addr : node["addrs"]) {
            if (!addr.is_string() || addr.get<std::string>().empty()) {
                return "libp2pConfig.bootstrapNodes addrs must be non-empty strings";
            }
        }
    }
    return {};
}

} // namespace detail

/// Takes the top-level `libp2pConfig` object out of `cfg` into `overrides`
/// (an empty object when absent). Returns the failure reason, empty on
/// success.
inline std::string takeLibp2pConfig(nlohmann::json& cfg, nlohmann::json& overrides)
{
    overrides = nlohmann::json::object();
    if (const auto key = detail::findKey(cfg, {"libp2pconfig"})) {
        if (!cfg[*key].is_object()) {
            return "libp2pConfig must be a JSON object";
        }
        overrides = cfg[*key];
        cfg.erase(*key);
    }
    return {};
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
/// plus the operator's `libp2pConfig` overrides into the plugin request: the
/// defaults (`mountKad`, `mountServiceDiscovery`) and the node's bootstrap
/// peers, with every override applied on top -- an explicit `bootstrapNodes`
/// there replaces the node's list, an empty one makes a seed. Returns the
/// failure reason, empty on success.
inline std::string fromRequirements(const std::string& reply, const nlohmann::json& overrides,
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

    nlohmann::json full = {
        {"mountKad", true}, {"mountServiceDiscovery", true}, {"bootstrapNodes", nodes}};
    if (overrides.is_object()) {
        full.update(overrides);
    }
    const std::string bad = detail::checkBootstrapNodes(full["bootstrapNodes"]);
    if (!bad.empty()) {
        return bad;
    }

    out.enabled = true;
    out.libp2pConfig = full.dump();
    return {};
}

} // namespace delivery_discovery
