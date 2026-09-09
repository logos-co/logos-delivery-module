#pragma once

// Plugin-discovery request of a createNode config.
//
// The config file is one document for both libraries: everything in it belongs
// to logos-delivery except the top-level `libp2pConfig` object, which is this
// module's and is handed to libp2p_module's createNode. logos-delivery's parser
// rejects keys it does not know, so `libp2pConfig` is stripped here before the
// config is forwarded.
//
// Whether the plugin is wanted is read from the same switch logos-delivery
// acts on, `pluginKadDiscovery`, in the section the shape carries it in
// (`messagingOverrides` for the layered shape, `kernelConf` for a kernel-only
// node). Nothing else in the config is interpreted or rewritten; in particular
// the exclusivity with the in-process kademlia is logos-delivery's to enforce.

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

inline bool flagSet(const nlohmann::json& section)
{
    const auto key = findKey(section, {"pluginkaddiscovery", "plugin-kad-discovery"});
    return key && section[*key].is_boolean() && section[*key].get<bool>();
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

/// Strips `libp2pConfig` from `cfg` and fills `out`. Returns the failure
/// reason, empty on success. On failure `cfg` may be partially modified and
/// must not be used.
inline std::string resolve(nlohmann::json& cfg, PluginRequest& out)
{
    out = PluginRequest{};

    nlohmann::json libp2p = nlohmann::json::object();
    if (const auto key = detail::findKey(cfg, {"libp2pconfig"})) {
        if (!cfg[*key].is_object()) {
            return "libp2pConfig must be a JSON object";
        }
        libp2p = cfg[*key];
        cfg.erase(*key);
    }

    bool enabled = false;
    for (const char* section : {"messagingoverrides", "kernelconf"}) {
        const auto key = detail::findKey(cfg, {section});
        if (key && detail::flagSet(cfg[*key])) {
            enabled = true;
        }
    }
    if (!enabled) {
        return {};
    }

    // Bootstrap peers can only be given at libp2p's createNode, and a kademlia
    // without peers can neither store a provider record nor answer a lookup,
    // so the list is mandatory. A seed node says so with an empty array.
    if (!libp2p.contains("bootstrapNodes")) {
        return "plugin discovery needs libp2pConfig.bootstrapNodes "
               "(an empty array for a seed node)";
    }
    const std::string bad = detail::checkBootstrapNodes(libp2p["bootstrapNodes"]);
    if (!bad.empty()) {
        return bad;
    }

    nlohmann::json full = {{"mountKad", true}, {"mountServiceDiscovery", true}};
    full.update(libp2p);
    out.enabled = true;
    out.libp2pConfig = full.dump();
    return {};
}

} // namespace delivery_discovery
