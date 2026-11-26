#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
 * @brief Native Mix's RLN scope: the ASCII `mix-rln-spam-protection/v1`,
 * zero-padded to 32 bytes — the delivery library's default for
 * `mix-rln-identifier-hex`. Mix proofs and relay proofs live in separate
 * scopes, so a mix membership's budget is its own.
 */
inline constexpr const char* kMixRlnIdentifierV1 =
    "6d69782d726c6e2d7370616d2d70726f74656374696f6e2f7631000000000000";

/** @brief The epoch size (s) and max epoch gap native Mix's proof plugin runs at. */
inline constexpr uint64_t kMixEpochSizeSec = 10;
inline constexpr uint64_t kMixMaxEpochGap = 3;

/**
 * @brief The node's mode, as the `createNode` config's `mode` names it. A
 * preset may give each mode its own rates (see RlnModeRates).
 */
enum class RlnMode { Core, Edge };

/** @brief `"Core"` or `"Edge"`. */
const char* rlnModeName(RlnMode mode);

/**
 * @brief The rates a node's memberships are asked at. 0 leaves the RLN
 * module's own default.
 */
struct RlnRates {
    uint64_t rateLimit = 0;
    uint64_t mixRateLimit = 0;
};

/**
 * @brief A deployment's rates for each mode. A preset's own `rate-limit` and
 * `mix-rate-limit` are both modes'; a key under `modes` replaces one for that
 * mode.
 */
struct RlnModeRates {
    RlnRates core;
    RlnRates edge;

    RlnRates& of(RlnMode mode) { return mode == RlnMode::Edge ? edge : core; }
    const RlnRates& of(RlnMode mode) const { return mode == RlnMode::Edge ? edge : core; }
};

/**
 * @brief One deployment's RLN settings, selected by a `createNode` preset.
 *
 * `registryId`, `rlnIdentifier`, `epochSizeSec` and `maxEpochGap` are
 * deployment properties, not client ones: every node of a deployment shares
 * them, and `rlnIdentifier` in particular must match across every proof
 * generator and verifier or each message is rejected as invalid.
 *
 * The `mix*` fields describe the registry native Mix proves on: another
 * registry than the relay one, always run at @ref kMixEpochSizeSec and
 * @ref kMixMaxEpochGap. The rates are the one thing that may differ by mode
 * (@ref rates). How a membership is obtained (an allocator, a funded
 * registration) is the RLN module's business and has no key here.
 */
struct RlnPresetEntry {
    bool enabled = false;
    bool manageBackend = true;
    bool enableValidation = true;
    std::string registryId;
    std::string rlnIdentifier;
    uint64_t epochSizeSec = 0;
    uint64_t maxEpochGap = 0;
    std::string mixRegistryId;
    std::string mixRlnIdentifier;
    std::string mixMetadataTopic;
    RlnModeRates rates;

    /** @brief Whether the deployment names a registry for native Mix. */
    bool hasMix() const { return !mixRegistryId.empty(); }
};

/**
 * @brief Parses a preset table.
 *
 * The document maps a preset name to an object with `enabled` plus, when
 * enabled, `registry-id` and `epoch-size-sec`, and the optional
 * `rln-identifier`, `max-epoch-gap`, `rate-limit` and `enable-validation`.
 * An omitted `rln-identifier` defaults to @ref kLogosDeliveryRlnIdentifier;
 * an omitted `enable-validation` defaults to true. An enabled entry missing
 * a required field is rejected here rather than at node creation. A key that
 * is present with the wrong JSON type (numbers are unsigned integers) is
 * rejected too, naming the preset and the key; it is never read as its
 * default.
 *
 * Optional key groups: `mix-*` (a registry for native Mix, which has to be
 * another registry than `registry-id`) and `modes` (`edge` / `core` objects
 * whose `rate-limit` and `mix-rate-limit` replace the entry's for nodes of
 * that mode; a mode written twice, as `edge` and `Edge`, is an error). The
 * file format is in docs/pages/rln.md.
 *
 * Names are matched exactly, and must be ones the delivery library accepts —
 * `""`, `twn`, `logos.dev`, `logos.test`, `status.prod`, `status.staging`. A variant spelling
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
 * Of the built-in presets only `logos.test` has RLN on (the LEZ testnet
 * registry, 600 s epochs, validation off); `""` and `logos.dev` ship with it off. The file named by
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
