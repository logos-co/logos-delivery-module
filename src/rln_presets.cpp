#include "rln_presets.h"

#include <algorithm>
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

// Neither fleet runs RLN, so neither names a registry: `enabled` is the whole
// entry until one does.
const std::map<std::string, RlnPresetEntry>& builtinPresets()
{
    static const std::map<std::string, RlnPresetEntry> table = {
        {"", RlnPresetEntry{.enabled = false}},
        {"logos.dev", RlnPresetEntry{.enabled = false}},
        {"logos.test", RlnPresetEntry{.enabled = false}},
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
            return "preset \"" + name + "\" is not a name the delivery library accepts ("
                   + knownPresetNameList() + ")";
        }

        RlnPresetEntry entry;
        auto enabledIt = value.find("enabled");
        entry.enabled = enabledIt != value.end() && enabledIt->is_boolean()
                            ? enabledIt->get<bool>()
                            : false;
        entry.registryId = stringField(value, "registry-id");
        entry.rlnIdentifier = stringField(value, "rln-identifier");
        entry.epochSizeSec = unsignedField(value, "epoch-size-sec");
        entry.maxEpochGap = unsignedField(value, "max-epoch-gap");

        if (entry.enabled) {
            if (entry.registryId.empty()) {
                return "preset \"" + name + "\" needs registry-id";
            }
            if (entry.rlnIdentifier.empty()) {
                return "preset \"" + name + "\" needs rln-identifier";
            }
            // The RLN module rejects a start config without it, and has no default.
            if (entry.epochSizeSec == 0) {
                return "preset \"" + name + "\" needs a positive epoch-size-sec";
            }
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
        error = "preset \"" + preset + "\" is not a name the delivery library accepts ("
                + knownPresetNameList() + ")";
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
