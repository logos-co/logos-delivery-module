#pragma once

#include <cstdint>
#include <map>
#include <string>

/** @brief Env var naming a JSON file of additional RLN presets. */
inline constexpr const char* kRlnPresetsEnvVar = "LOGOS_DELIVERY_RLN_PRESETS";

/**
 * @brief This application's RLN scope: `sha256("rln/logos-delivery/v0.0.1")`.
 *
 * An application constant, not a per-deployment one. It is bound into the
 * external nullifier both ends of a message derive, so two nodes that do not
 * share it reject each other's proofs as invalid — which reads as a delivery
 * fault rather than a misconfiguration. A preset may still carry its own when
 * a deployment needs a separate scope.
 *
 * The 32 bytes are arbitrary as far as the protocol is concerned: the circuit
 * path reduces them with `hash_to_field_le` (Keccak-256) regardless of how
 * they were chosen. SHA-256 of a readable name is a reproducible way to pick
 * them, not a protocol requirement.
 */
inline constexpr const char* kLogosDeliveryRlnIdentifier =
    "5e269b6a19fce081f5808b13442dcbc3522197638dd38df5a28bc4e55236b977";

/**
 * @brief One deployment's RLN settings, selected by a `createNode` preset.
 *
 * `registryId`, `rlnIdentifier`, `epochSizeSec` and `maxEpochGap` are
 * deployment properties, not client ones: every node of a deployment shares
 * them, and `rlnIdentifier` in particular must match across every proof
 * generator and verifier or each message is rejected as invalid.
 */
struct RlnPresetEntry {
    bool enabled = false;
    bool manageBackend = true;
    bool enableValidation = true;
    std::string registryId;
    std::string rlnIdentifier;
    uint64_t epochSizeSec = 0;
    uint64_t maxEpochGap = 0;
};

/**
 * @brief Parses a preset table.
 *
 * The document maps a preset name to an object with `enabled` plus, when
 * enabled, `registry-id` and `epoch-size-sec`, and the optional
 * `rln-identifier`, `max-epoch-gap` and `enable-validation`. An omitted
 * `rln-identifier` defaults to @ref kLogosDeliveryRlnIdentifier; an omitted
 * `enable-validation` defaults to true. An enabled entry missing a required
 * field is rejected here rather than at node creation.
 *
 * Names are matched exactly, and must be ones the delivery library accepts —
 * `""`, `twn`, `logos.dev`, `logos.test`, `status.prod`. A variant spelling
 * such as `logostest` is an error rather than a match, so a typo cannot
 * silently leave a node without the rate limiting it asked for.
 *
 * @param json Table document.
 * @param out Receives the entries, keyed by preset name.
 * @return Empty on success, a description of the problem otherwise.
 */
std::string parseRlnPresetTable(const std::string& json,
                                std::map<std::string, RlnPresetEntry>& out);

/**
 * @brief RLN settings for a `createNode` preset.
 *
 * Built-in presets ship with RLN off. The file named by
 * @ref kRlnPresetsEnvVar is merged over them, which is how a test or local
 * deployment supplies its own registry with no public API for it. That file
 * may only key entries by preset names the delivery library accepts — it
 * cannot introduce a new network.
 *
 * @param preset Preset name as it appears in the `createNode` config, spelled
 *        exactly as the delivery library spells it.
 * @param error Receives a description when the name is not one of those
 *        spellings or the env-var table cannot be used; the caller should
 *        treat either as fatal rather than run without RLN.
 * @return The preset's settings; RLN off for a name the table does not carry.
 */
RlnPresetEntry resolveRlnPreset(const std::string& preset, std::string& error);
