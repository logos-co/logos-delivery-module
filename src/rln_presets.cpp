#include "rln_presets.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace {

const std::map<std::string, RlnPresetEntry>& builtinPresets()
{
    static const std::map<std::string, RlnPresetEntry> table = {
        {"", RlnPresetEntry{}},
        {"twn", RlnPresetEntry{}},
        {"logosdev", RlnPresetEntry{}},
        {"logostest", RlnPresetEntry{}},
        {"statusprod", RlnPresetEntry{}},
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

std::string normalizeRlnPresetName(const std::string& preset)
{
    std::string key;
    key.reserve(preset.size());
    for (unsigned char c : preset) {
        if (c == '.') {
            continue;
        }
        key.push_back(static_cast<char>(std::tolower(c)));
    }
    return key;
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
        parsed[normalizeRlnPresetName(name)] = entry;
    }

    out = std::move(parsed);
    return {};
}

RlnPresetEntry resolveRlnPreset(const std::string& preset, std::string& error)
{
    error.clear();

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

    auto it = table.find(normalizeRlnPresetName(preset));
    if (it == table.end()) {
        return {};
    }
    return it->second;
}
