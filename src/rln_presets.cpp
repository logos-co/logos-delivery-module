#include "rln_presets.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
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
// size belong to a deployment. logos.test runs relay RLN on the LEZ testnet
// registry: the CAIP-10 account is the registration program's config account
// (base58 J3NXPWUspX2zqJU3tUbR6MHmm83aJc5AzvidEaBBKnat) on the LEZ v0.3.1 zone
// behind sequencer https://testnet.lez.logos.co/, channel
// 0101010101010101010101010101010101010101010101010101010101010101.
// Needs rln_module >= 0.14.2 / rln_lez_module >= 6.0.0, which resolve
// `logos:testnet` to that sequencer themselves. maxEpochGap stays 0 so `start`
// omits it and the RLN module's own default applies.
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
                        .rates = kDefaultRlnModeRates}},
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

// Reads and validates one preset as written; `who` opens every error. Its
// rates are both modes': `modes` is parseModes' business. Returns the error,
// empty when `entry` is usable.
std::string parseEntry(const nlohmann::json& value, const std::string& who, RlnPresetEntry& entry)
{
    EntryReader read{value, who, {}};
    entry.enabled = read.flag("enabled", false);
    entry.enableValidation = read.flag("enable-validation", true);
    entry.manageBackend = read.flag("manage-backend", true);
    entry.registryId = read.str("registry-id");
    entry.rlnIdentifier = read.str("rln-identifier");
    entry.epochSizeSec = read.number("epoch-size-sec");
    entry.maxEpochGap = read.number("max-epoch-gap");
    entry.mixRegistryId = read.str("mix-registry-id");
    entry.mixRlnIdentifier = read.str("mix-rln-identifier");
    entry.mixMetadataTopic = read.str("mix-metadata-topic");
    // A rate key sets both modes' rate; one left out keeps each mode's default.
    entry.rates = kDefaultRlnModeRates;
    for (RlnMode mode : {RlnMode::Core, RlnMode::Edge}) {
        RlnRates& rates = entry.rates.of(mode);
        rates.rateLimit = read.number("rate-limit", rates.rateLimit);
        rates.mixRateLimit = read.number("mix-rate-limit", rates.mixRateLimit);
    }
    if (!read.error.empty()) {
        return read.error;
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
        // Mix RLN is independent of relay RLN: a preset may name either.
        // With both, they prove on two registries: the backend is started for
        // the mix one at Mix's own epoch size and gap. Ids that differ only
        // in letter case would be started as two registries, which the RLN
        // module may take for one.
        if (entry.enabled && equalIgnoringAsciiCase(entry.mixRegistryId, entry.registryId)) {
            return who + " mix-registry-id must name another registry than registry-id";
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
    return {};
}

// The keys a mode may set; everything else is the deployment's, one value for
// every node.
bool isModeKey(const std::string& key)
{
    static const std::vector<std::string> keys = {"rate-limit", "mix-rate-limit"};
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// Reads a preset's `modes` into `rates`, which holds the entry's own rates for
// both modes: a key a mode sets, 0 included, replaces that mode's rate.
std::string parseModes(const nlohmann::json& value, const std::string& name, RlnModeRates& rates)
{
    const auto modesIt = value.find("modes");
    if (modesIt == value.end()) {
        return {};
    }
    const std::string who = "preset \"" + name + "\"";
    if (!modesIt->is_object()) {
        return who + " modes must be an object";
    }
    std::set<RlnMode> written;
    for (const auto& item : modesIt->items()) {
        std::string modeName = item.key();
        for (char& c : modeName) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (modeName != "core" && modeName != "edge") {
            return who + " modes: unknown mode \"" + item.key() + "\"";
        }
        const RlnMode mode = modeName == "edge" ? RlnMode::Edge : RlnMode::Core;
        // Two spellings of one mode ("edge" and "Edge").
        if (!written.insert(mode).second) {
            return who + " modes: mode " + modeName + " is written twice";
        }
        if (!item.value().is_object()) {
            return who + " mode " + modeName + " must be an object";
        }
        const std::string inMode = who + " mode " + modeName + ":";
        for (const auto& key : item.value().items()) {
            if (!isModeKey(key.key())) {
                return inMode + " " + key.key() + " cannot be set in a mode";
            }
        }
        RlnRates& modeRates = rates.of(mode);
        EntryReader read{item.value(), inMode, {}};
        modeRates.rateLimit = read.number("rate-limit", modeRates.rateLimit);
        modeRates.mixRateLimit = read.number("mix-rate-limit", modeRates.mixRateLimit);
        if (!read.error.empty()) {
            return read.error;
        }
    }
    return {};
}

} // namespace

const char* rlnModeName(RlnMode mode)
{
    return mode == RlnMode::Edge ? "Edge" : "Core";
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
        if (std::string failure = parseEntry(value, "preset \"" + name + "\"", entry);
            !failure.empty()) {
            return failure;
        }
        if (std::string failure = parseModes(value, name, entry.rates); !failure.empty()) {
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
