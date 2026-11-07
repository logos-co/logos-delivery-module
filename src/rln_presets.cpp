#include "rln_presets.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

// Spelled as the delivery library spells them (tools/confutils/cli_args.nim):
// it resolves the same key, so a name it rejects can never reach RLN anyway.
const std::vector<std::string>& knownPresetNames()
{
    static const std::vector<std::string> names = {"", "twn", "logos.dev", "logos.test",
                                                   "status.prod"};
    return names;
}

bool isKnownPresetName(const std::string& preset)
{
    const auto& names = knownPresetNames();
    return std::find(names.begin(), names.end(), preset) != names.end();
}

std::string knownPresetNameList()
{
    std::string out;
    for (const std::string& name : knownPresetNames()) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name.empty() ? "\"\"" : name;
    }
    return out;
}

std::string unknownPresetError(const std::string& name)
{
    return "preset \"" + name + "\" is not a name the delivery library accepts ("
           + knownPresetNameList() + ")";
}

// The identifier is this application's and is known; the registry and epoch
// size belong to a deployment. Only logos.test runs RLN, on the LEZ testnet
// registry: the CAIP-10 account is the registration program's config account
// (base58 J3NXPWUspX2zqJU3tUbR6MHmm83aJc5AzvidEaBBKnat) on the LEZ v0.3.1 zone
// behind sequencer https://testnet.lez.logos.co/, channel
// 0101010101010101010101010101010101010101010101010101010101010101.
// liblogos_rln_module >= 0.14.2 / liblogos_lez_rln_module >= 6.0.0 resolve
// `logos:testnet` to that sequencer themselves; older lez modules link an
// older LEZ wallet that cannot sync this zone. maxEpochGap stays 0 so `start`
// omits it and the RLN module's own default applies. The entry moves to an
// LEZ v0.4 testnet registry once one is deployed; none exists yet.
const std::map<std::string, RlnPresetEntry>& builtinPresets()
{
    static const std::map<std::string, RlnPresetEntry> table = {
        {"", RlnPresetEntry{.enabled = false, .rlnIdentifier = kLogosDeliveryRlnIdentifier}},
        {"logos.dev", RlnPresetEntry{.enabled = false, .rlnIdentifier = kLogosDeliveryRlnIdentifier}},
        {"logos.test",
         RlnPresetEntry{.enabled = true,
                        .enableValidation = false,
                        .registryId = "logos:testnet:"
                                      "fd322d32a46ddf57eece9db8d68e9b722262d1f770760fca63b8cb9e7775b971",
                        .rlnIdentifier = kLogosDeliveryRlnIdentifier,
                        .epochSizeSec = 600,
                        .allocatorDiscover = true,
                        .allocatorAuthType = kAllocatorAuthTypeDefault}},
    };
    return table;
}

std::string readWholeFile(const std::string& path, std::string& contents)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "cannot open " + path;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    if (in.bad()) {
        return "cannot read " + path;
    }
    contents = buf.str();
    return {};
}

std::string stringField(const nlohmann::json& obj, const char* key)
{
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

uint64_t unsignedField(const nlohmann::json& obj, const char* key)
{
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_unsigned()) {
        return 0;
    }
    return it->get<uint64_t>();
}

// `fallback` when the key is absent; the value, 0 included, when it is set.
uint64_t unsignedFieldOr(const nlohmann::json& obj, const char* key, uint64_t fallback)
{
    auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    return it->is_number_unsigned() ? it->get<uint64_t>() : 0;
}

// The delivery library reads the mix identifier as exactly 32 bytes of hex.
bool isHex32Bytes(const std::string& s)
{
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

} // namespace

std::string parseRlnPresetTable(const std::string& json,
                                std::map<std::string, RlnPresetEntry>& out)
{
    nlohmann::json doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        return "not valid JSON";
    }
    if (!doc.is_object()) {
        return "not a JSON object";
    }

    std::map<std::string, RlnPresetEntry> parsed;
    for (const auto& item : doc.items()) {
        const std::string name = item.key();
        const nlohmann::json& value = item.value();
        if (!value.is_object()) {
            return "preset \"" + name + "\" is not an object";
        }

        if (!isKnownPresetName(name)) {
            return unknownPresetError(name);
        }

        RlnPresetEntry entry;
        auto enabledIt = value.find("enabled");
        entry.enabled = enabledIt != value.end() && enabledIt->is_boolean()
                            ? enabledIt->get<bool>()
                            : false;
        // Every RLN preset asks the allocators discovery finds unless it says
        // otherwise below.
        entry.allocatorDiscover = entry.enabled;
        auto enableValidationIt = value.find("enable-validation");
        entry.enableValidation = enableValidationIt != value.end() && enableValidationIt->is_boolean()
                                     ? enableValidationIt->get<bool>()
                                     : true;
        auto manageIt = value.find("manage-backend");
        if (manageIt != value.end()) {
            if (!manageIt->is_boolean()) {
                return "manage-backend must be a boolean";
            }
            entry.manageBackend = manageIt->get<bool>();
        }
        entry.registryId = stringField(value, "registry-id");
        entry.rlnIdentifier = stringField(value, "rln-identifier");
        entry.epochSizeSec = unsignedField(value, "epoch-size-sec");
        entry.maxEpochGap = unsignedField(value, "max-epoch-gap");
        entry.rateLimit = unsignedField(value, "rate-limit");
        entry.mixRegistryId = stringField(value, "mix-registry-id");
        entry.mixRlnIdentifier = stringField(value, "mix-rln-identifier");
        entry.mixEpochSizeSec = unsignedFieldOr(value, "mix-epoch-size-sec", kMixEpochSizeSec);
        entry.mixMaxEpochGap = unsignedFieldOr(value, "mix-max-epoch-gap", kMixMaxEpochGap);
        entry.mixRateLimit = unsignedField(value, "mix-rate-limit");
        entry.mixMetadataTopic = stringField(value, "mix-metadata-topic");
        auto discoverIt = value.find("allocator-discover");
        if (discoverIt != value.end()) {
            if (!discoverIt->is_boolean()) {
                return "preset \"" + name + "\" allocator-discover must be a boolean";
            }
            entry.allocatorDiscover = discoverIt->get<bool>();
        }
        entry.allocatorPeerId = stringField(value, "allocator-peer-id");
        entry.allocatorMultiaddr = stringField(value, "allocator-multiaddr");
        entry.allocatorAuthType = stringField(value, "allocator-auth-type");
        entry.allocatorAuthPayload = stringField(value, "allocator-auth-payload");
        entry.allocatorAuthProvider = stringField(value, "allocator-auth-provider");
        entry.allocatorAuthArgs = stringField(value, "allocator-auth-args");

        // The application's own scope unless the deployment names another.
        if (entry.rlnIdentifier.empty()) {
            entry.rlnIdentifier = kLogosDeliveryRlnIdentifier;
        }
        if (entry.mixRlnIdentifier.empty()) {
            entry.mixRlnIdentifier = kMixRlnIdentifierV1;
        }

        if (entry.enabled) {
            if (entry.registryId.empty()) {
                return "preset \"" + name + "\" needs registry-id";
            }
            // The RLN module rejects a start config without it, and has no default.
            if (entry.epochSizeSec == 0) {
                return "preset \"" + name + "\" needs a positive epoch-size-sec";
            }
        }
        if (entry.hasMix()) {
            // Mix proofs ride the relay bridge; the library refuses mix
            // without relay RLN.
            if (!entry.enabled) {
                return "preset \"" + name + "\" has mix-registry-id but RLN is not enabled";
            }
            // One registry runs one epoch size, and mix runs a different one.
            if (entry.mixRegistryId == entry.registryId) {
                return "preset \"" + name + "\" names the same registry for relay and mix";
            }
            if (entry.mixEpochSizeSec == 0) {
                return "preset \"" + name + "\" needs a positive mix-epoch-size-sec";
            }
            if (!isHex32Bytes(entry.mixRlnIdentifier)) {
                return "preset \"" + name + "\" mix-rln-identifier is not 32 bytes of hex";
            }
            // The library refuses a mix registry without the topic its proof
            // metadata is published on.
            if (entry.mixMetadataTopic.empty()) {
                return "preset \"" + name + "\" needs mix-metadata-topic";
            }
        }
        // The allocator: a peer id and a multiaddr go together; the auth
        // vector defaults to peer-any, whose payload this module derives, and
        // any other needs exactly one payload source. Every RLN preset asks
        // an allocator unless it turns discovery off and names no peer.
        const bool anyAllocatorKey = discoverIt != value.end() || !entry.allocatorPeerId.empty()
            || !entry.allocatorMultiaddr.empty() || !entry.allocatorAuthType.empty()
            || !entry.allocatorAuthPayload.empty() || !entry.allocatorAuthProvider.empty()
            || !entry.allocatorAuthArgs.empty();
        if (anyAllocatorKey && !entry.enabled) {
            return "preset \"" + name + "\" names an allocator but RLN is not enabled";
        }
        if (entry.allocatorPeerId.empty() != entry.allocatorMultiaddr.empty()) {
            return "preset \"" + name + "\" needs both allocator-peer-id and allocator-multiaddr";
        }
        if (entry.hasAllocator()) {
            if (entry.allocatorAuthType.empty()) {
                entry.allocatorAuthType = kAllocatorAuthTypeDefault;
            }
            if (!entry.allocatorAuthPayload.empty() && !entry.allocatorAuthProvider.empty()) {
                return "preset \"" + name + "\" sets both allocator-auth-payload and allocator-auth-provider";
            }
            if (entry.allocatorAuthType != kAllocatorAuthTypeDefault && entry.allocatorAuthPayload.empty()
                && entry.allocatorAuthProvider.empty()) {
                return "preset \"" + name + "\" allocator-auth-type " + entry.allocatorAuthType
                       + " needs allocator-auth-payload or allocator-auth-provider";
            }
        }
        parsed[name] = entry;
    }

    out = std::move(parsed);
    return {};
}

std::string peerAnyAuthPayload(uint64_t rateLimit)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "02";
    for (int i = 0; i < 8; ++i) {
        const unsigned byte = static_cast<unsigned>((rateLimit >> (8 * i)) & 0xffu);
        out += kHex[byte >> 4];
        out += kHex[byte & 0xfu];
    }
    return out;
}

RlnPresetEntry resolveRlnPreset(const std::string& preset, std::string& error)
{
    error.clear();

    if (!isKnownPresetName(preset)) {
        error = unknownPresetError(preset);
        return {};
    }

    std::map<std::string, RlnPresetEntry> table = builtinPresets();

    if (const char* path = std::getenv(kRlnPresetsEnvVar); path != nullptr && *path != '\0') {
        std::string contents;
        if (std::string failure = readWholeFile(path, contents); !failure.empty()) {
            error = std::string(kRlnPresetsEnvVar) + ": " + failure;
            return {};
        }
        std::map<std::string, RlnPresetEntry> extra;
        if (std::string failure = parseRlnPresetTable(contents, extra); !failure.empty()) {
            error = std::string(kRlnPresetsEnvVar) + " (" + path + "): " + failure;
            return {};
        }
        for (auto& [name, entry] : extra) {
            table[name] = std::move(entry);
        }
    }

    auto it = table.find(preset);
    if (it == table.end()) {
        return {};
    }
    return it->second;
}
