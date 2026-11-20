#include "rln_presets.h"

#include "discovery_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

// Spelled as the delivery library spells them (tools/confutils/cli_args.nim):
// it resolves the same key, so a name it rejects can never reach RLN anyway.
const std::vector<std::string>& knownPresetNames()
{
    static const std::vector<std::string> names = {"", "twn", "logos.dev", "logos.test",
                                                   "status.prod", "status.staging"};
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
// Needs rln_module >= 0.14.2 / rln_lez_module >= 6.0.0, which resolve
// `logos:testnet` to that sequencer themselves. maxEpochGap stays 0 so `start`
// omits it and the RLN module's own default applies. allocatorDiscover is set
// explicitly here: a file entry gets it only by naming an allocator key.
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
                        .allocatorAuthType = kAllocatorAuthTypeDefault,
                        .allocatorBootstrapNodes = {
                            "/dns4/node-01.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmQ9X2xDfPG3uL77V9piYDhjq14JhKCtcmNYsTMKNqrKCj",
                            "/dns4/node-02.do-ams3.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmB8NYprrfQrgWVzsJtYWkfjsXbmJEGNMG6othXsQ53BwG",
                            "/dns4/node-01.gc-us-central1-a.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmF8WtwGPmeGHgYAX2277jHgy5cW9F7zsB8EqUjBZQAZQ3",
                            "/dns4/node-02.gc-us-central1-a.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmUuXhUW9bdJpzN1kfDziFiUZo4bszTk66cvr7uuyCHXR7",
                            "/dns4/node-01.ac-cn-hongkong-c.logos.test.status.im/tcp/30303/p2p/16Uiu2HAmL3oU95jh1BZHozn3uNhx8HEneirgr8M1jEAapzXGDqRF",
                            "/dns4/node-02.ac-cn-hongkong-c.logos.test.status.im/tcp/30303/p2p/16Uiu2HAm28CoBZjpyxsanC8tQpbvZ7bZJnVYuB1EgFzb571qpWsV",
                        }}},
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

// Typed reads of one preset's keys: a present key of the wrong JSON type is
// an error (the first is kept in `error`), never its default.
struct EntryReader {
    const nlohmann::json& obj;
    const std::string& who;
    std::string error;

    bool has(const char* key) const { return obj.find(key) != obj.end(); }

    void wrongType(const char* key, const char* expected)
    {
        if (error.empty()) {
            error = who + " " + key + " must be " + expected;
        }
    }

    std::string str(const char* key)
    {
        auto it = obj.find(key);
        if (it == obj.end()) {
            return {};
        }
        if (!it->is_string()) {
            wrongType(key, "a string");
            return {};
        }
        return it->get<std::string>();
    }

    // `fallback` when the key is absent; the value, 0 included, when it is set.
    uint64_t number(const char* key, uint64_t fallback = 0)
    {
        auto it = obj.find(key);
        if (it == obj.end()) {
            return fallback;
        }
        if (!it->is_number_unsigned()) {
            wrongType(key, "an unsigned integer");
            return fallback;
        }
        return it->get<uint64_t>();
    }

    bool flag(const char* key, bool fallback)
    {
        auto it = obj.find(key);
        if (it == obj.end()) {
            return fallback;
        }
        if (!it->is_boolean()) {
            wrongType(key, "a boolean");
            return fallback;
        }
        return it->get<bool>();
    }
};

bool equalIgnoringAsciiCase(const std::string& a, const std::string& b)
{
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

// The delivery library reads the mix identifier as exactly 32 bytes of hex.
bool isHex32Bytes(const std::string& s)
{
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

// Reads and validates one entry: a preset as written, or a preset with one
// mode's keys laid over it (`inMode`). `who` opens every error: the preset,
// and the mode when there is one. `relay-membership` is read only for a mode;
// `modes` itself is parseModes' business. Returns the error, empty when
// `entry` is usable.
std::string parseEntry(const nlohmann::json& value, const std::string& who, bool inMode,
                       RlnPresetEntry& entry)
{
    EntryReader read{value, who, {}};
    entry.enabled = read.flag("enabled", false);
    entry.enableValidation = read.flag("enable-validation", true);
    entry.manageBackend = read.flag("manage-backend", true);
    entry.registryId = read.str("registry-id");
    entry.rlnIdentifier = read.str("rln-identifier");
    entry.epochSizeSec = read.number("epoch-size-sec");
    entry.maxEpochGap = read.number("max-epoch-gap");
    entry.rateLimit = read.number("rate-limit");
    entry.mixRegistryId = read.str("mix-registry-id");
    entry.mixRlnIdentifier = read.str("mix-rln-identifier");
    entry.mixEpochSizeSec = read.number("mix-epoch-size-sec", kMixEpochSizeSec);
    entry.mixMaxEpochGap = read.number("mix-max-epoch-gap", kMixMaxEpochGap);
    entry.mixRateLimit = read.number("mix-rate-limit");
    entry.mixMetadataTopic = read.str("mix-metadata-topic");
    const bool discoverSet = read.has("allocator-discover");
    const bool discover = read.flag("allocator-discover", false);
    entry.allocatorPeerId = read.str("allocator-peer-id");
    entry.allocatorMultiaddr = read.str("allocator-multiaddr");
    entry.allocatorAuthType = read.str("allocator-auth-type");
    entry.allocatorAuthPayload = read.str("allocator-auth-payload");
    entry.allocatorAuthProvider = read.str("allocator-auth-provider");
    entry.allocatorAuthArgs = read.str("allocator-auth-args");
    if (inMode) {
        entry.relayMembership = read.flag("relay-membership", true);
    }
    if (!read.error.empty()) {
        return read.error;
    }
    if (!inMode && read.has("relay-membership")) {
        return who + " relay-membership can be set only in a mode, under modes";
    }
    auto bootstrapIt = value.find("allocator-bootstrap-nodes");
    if (bootstrapIt != value.end()) {
        if (!bootstrapIt->is_array()) {
            return who + " allocator-bootstrap-nodes must be an array";
        }
        for (const auto& addr : *bootstrapIt) {
            nlohmann::json node;
            const std::string bad = addr.is_string()
                ? delivery_discovery::bootstrapNodeFrom(addr.get<std::string>(), node)
                : "bootstrap node is not a string: " + addr.dump();
            if (!bad.empty()) {
                return who + " allocator-bootstrap-nodes: " + bad;
            }
            entry.allocatorBootstrapNodes.push_back(addr.get<std::string>());
        }
    }

    if (entry.rlnIdentifier.empty()) {
        entry.rlnIdentifier = kLogosDeliveryRlnIdentifier;
    }
    if (entry.mixRlnIdentifier.empty()) {
        entry.mixRlnIdentifier = kMixRlnIdentifierV1;
    }

    if (entry.enabled) {
        if (entry.registryId.empty()) {
            return who + " needs registry-id";
        }
        // The RLN module rejects a start config without it, and has no default.
        if (entry.epochSizeSec == 0) {
            return who + " needs a positive epoch-size-sec";
        }
    }
    if (entry.hasMix()) {
        // Mix proofs ride the relay bridge; the library refuses mix
        // without relay RLN.
        if (!entry.enabled) {
            return who + " has mix-registry-id but RLN is not enabled";
        }
        // Native Mix's proof plugin refuses to start against a backend
        // that reports any other epoch size or gap for its registry.
        if (entry.mixEpochSizeSec != kMixEpochSizeSec) {
            return who + " mix-epoch-size-sec must be "
                   + std::to_string(kMixEpochSizeSec);
        }
        if (entry.mixMaxEpochGap != kMixMaxEpochGap) {
            return who + " mix-max-epoch-gap must be "
                   + std::to_string(kMixMaxEpochGap);
        }
        // Ids that differ only in letter case would be started as two
        // registries, which the RLN module may take for one.
        if (entry.mixRegistryId != entry.registryId
            && equalIgnoringAsciiCase(entry.mixRegistryId, entry.registryId)) {
            return who + " registry-id and mix-registry-id differ only in"
                   " letter case; one registry must be spelled identically in both";
        }
        // One registry runs one epoch size, one gap and one rate, so relay
        // and mix share a registry only when relay runs at Mix's values.
        // An unset max-epoch-gap is the RLN module's default of 1, not 3.
        if (entry.sharesRegistry()) {
            const std::string shared =
                who + " names the same registry for relay and mix, which needs ";
            if (entry.epochSizeSec != kMixEpochSizeSec) {
                return shared + "epoch-size-sec " + std::to_string(kMixEpochSizeSec);
            }
            if (entry.maxEpochGap != kMixMaxEpochGap) {
                return shared + "max-epoch-gap " + std::to_string(kMixMaxEpochGap);
            }
            if (entry.rateLimit != 0 && entry.mixRateLimit != 0
                && entry.rateLimit != entry.mixRateLimit) {
                return shared + "rate-limit and mix-rate-limit to be equal, or one of them unset";
            }
        }
        if (!isHex32Bytes(entry.mixRlnIdentifier)) {
            return who + " mix-rln-identifier is not 32 bytes of hex";
        }
        // The library refuses a mix registry without the topic its proof
        // metadata is published on.
        if (entry.mixMetadataTopic.empty()) {
            return who + " needs mix-metadata-topic";
        }
    }
    // Without its Relay membership a node proves on the mix registry only,
    // which has to be there and has to be another registry: a shared one is
    // the Relay membership.
    if (!entry.relayMembership && !(entry.hasMix() && !entry.sharesRegistry())) {
        return who + " relay-membership false needs a mix registry of its own";
    }
    // allocator-peer-id and allocator-multiaddr go together. A `peer-` auth
    // vector needs no payload source, any other exactly one. Discovery
    // defaults on only when the entry names an allocator key.
    const bool namesAllocator = !entry.allocatorPeerId.empty()
        || !entry.allocatorMultiaddr.empty() || !entry.allocatorAuthType.empty()
        || !entry.allocatorAuthPayload.empty() || !entry.allocatorAuthProvider.empty()
        || !entry.allocatorAuthArgs.empty() || bootstrapIt != value.end();
    entry.allocatorDiscover = discoverSet ? discover : namesAllocator;
    const bool anyAllocatorKey = discoverSet || namesAllocator;
    if (anyAllocatorKey && !entry.enabled) {
        return who + " names an allocator but RLN is not enabled";
    }
    if (entry.allocatorPeerId.empty() != entry.allocatorMultiaddr.empty()) {
        return who + " needs both allocator-peer-id and allocator-multiaddr";
    }
    if (entry.hasAllocator()) {
        if (entry.allocatorAuthType.empty()) {
            entry.allocatorAuthType = kAllocatorAuthTypeDefault;
        }
        if (!entry.allocatorAuthPayload.empty() && !entry.allocatorAuthProvider.empty()) {
            return who + " sets both allocator-auth-payload and allocator-auth-provider";
        }
        const bool peerScheme = entry.allocatorAuthType.rfind("peer-", 0) == 0;
        if (!peerScheme && entry.allocatorAuthPayload.empty()
            && entry.allocatorAuthProvider.empty()) {
            return who + " allocator-auth-type " + entry.allocatorAuthType
                   + " needs allocator-auth-payload or allocator-auth-provider";
        }
    }
    return {};
}

// The keys a mode may set; everything else is the deployment's, one value for
// every node.
bool isModeKey(const std::string& key)
{
    static const std::vector<std::string> keys = {
        "rate-limit",          "mix-rate-limit",         "relay-membership",
        "allocator-discover",  "allocator-peer-id",      "allocator-multiaddr",
        "allocator-auth-type", "allocator-auth-payload", "allocator-auth-provider",
        "allocator-auth-args",
    };
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// Resolves a preset's `modes`: each mode's keys are laid over the entry as
// written and the result is read and validated like any entry, so every rule
// (and the allocator-discover default) holds for what a node of that mode
// runs with. `entry` is the preset as written, already parsed.
std::string parseModes(const nlohmann::json& value, const std::string& name, RlnPresetEntry& entry)
{
    const auto modesIt = value.find("modes");
    if (modesIt == value.end()) {
        return {};
    }
    const std::string who = "preset \"" + name + "\"";
    if (!modesIt->is_object()) {
        return who + " modes must be an object";
    }
    nlohmann::json base = value;
    base.erase("modes");
    for (const auto& item : modesIt->items()) {
        std::string modeName = item.key();
        for (char& c : modeName) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (modeName != "core" && modeName != "edge") {
            return who + " modes: unknown mode \"" + item.key() + "\"";
        }
        std::shared_ptr<const RlnPresetEntry>& slot =
            modeName == "edge" ? entry.edgeMode : entry.coreMode;
        // Two spellings of one mode ("edge" and "Edge").
        if (slot) {
            return who + " modes: mode " + modeName + " is written twice";
        }
        if (!item.value().is_object()) {
            return who + " mode " + modeName + " must be an object";
        }
        const std::string inMode = who + " mode " + modeName + ":";
        nlohmann::json merged = base;
        for (const auto& key : item.value().items()) {
            if (!isModeKey(key.key())) {
                return inMode + " " + key.key() + " cannot be set in a mode";
            }
            merged[key.key()] = key.value();
        }
        RlnPresetEntry resolved;
        if (std::string failure = parseEntry(merged, inMode, /*inMode=*/true, resolved);
            !failure.empty()) {
            return failure;
        }
        slot = std::make_shared<const RlnPresetEntry>(std::move(resolved));
    }
    return {};
}

} // namespace

const char* rlnModeName(RlnMode mode)
{
    return mode == RlnMode::Edge ? "Edge" : "Core";
}

RlnPresetEntry RlnPresetEntry::forMode(RlnMode mode) const
{
    const std::shared_ptr<const RlnPresetEntry>& resolved =
        mode == RlnMode::Edge ? edgeMode : coreMode;
    RlnPresetEntry out = resolved ? *resolved : *this;
    out.coreMode.reset();
    out.edgeMode.reset();
    return out;
}

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
        if (std::string failure =
                parseEntry(value, "preset \"" + name + "\"", /*inMode=*/false, entry);
            !failure.empty()) {
            return failure;
        }
        if (std::string failure = parseModes(value, name, entry); !failure.empty()) {
            return failure;
        }
        parsed[name] = entry;
    }

    out = std::move(parsed);
    return {};
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
