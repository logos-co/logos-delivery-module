# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
outsources every RLN operation to an external RLN module across a C callback
surface (`liblogosdelivery_rln.h`, one typed callback per RLN function). This
module is the bridge: it registers the callbacks with the library and serves
each request in-process by calling the co-loaded `liblogos_rln_module`
(`src/rln_bridge.cpp`), feeding the reply back through
`logosdelivery_rln_response`. The header is mirrored at
`tests/stubs/lib/liblogosdelivery_rln.h`, pinned to the logos-delivery flake
input rev — keep the two in sync when bumping the input.

The surface speaks the RLN module's own wire format (logos-rln-modules branch
`feat/lip-alignment`, `liblogos_rln_module.lidl`): argument shapes and result
payloads cross the bridge verbatim in the module's format, so it needs no
translation beyond turning the numeric timestamps into the module's string
args.

## Request/response loop

1. The delivery library needs an RLN operation and fires that operation's
   registered callback. The bridge, when enabled, serves the operation; the
   module also emits the matching typed event for observability:

   ```
   rlnStartRequest(reqId, configJson, timestamp)
   rlnStopRequest(reqId, timestamp)
   rlnRegisterRequest(reqId, registryId, rlnIdentifier, optionsJson, timestamp)
   rlnGetMembershipStateRequest(reqId, registryId, rlnIdentifier, timestamp)
   rlnGetEpochQuotaRequest(reqId, registryId, rlnIdentifier, epochTimestamp, timestamp)
   rlnGenerateProofRequest(reqId, registryId, rlnIdentifier, signalHex, epochTimestamp, timestamp)
   rlnValidateProofRequest(reqId, registryId, rlnIdentifier, signalHex, epochTimestamp, proofJson, timestamp)
   ```

   `configJson` is the module's `start()` config, built by the library from
   its RLN conf (at minimum `{"epoch_size_sec":N,"registries":[…]}` — the
   epoch size every proof generator and validator of a deployment must
   share). `optionsJson` is the module's RegistryOptions array
   `[{"key":"…","value":"…"}, …]`, built by the library from its conf: the
   rate limit (`rln-relay-user-message-limit`) as the `"rate_limit"` entry
   (decimal string) plus the operator's extra options
   (`rln-relay-registry-options`, a flat JSON object). `epochTimestamp` is
   the ABI's epoch/quota timestamp in Unix seconds; the trailing `timestamp`
   is the local emission time, as on every other event.

2. The bridge answers with the same `reqId` via `logosdelivery_rln_response`.
   The module surface also exposes `rlnRespond(reqId, resultJson)` for
   completing a request externally, but on a bridge-enabled node the bridge
   answers first and a second response per `reqId` is rejected — treat the
   events as read-only observability.

   `reqId` values >= 2^63 appear negative on the event surface (int64 view of
   the library's uint64 id); they round-trip bit-exactly.

Callbacks are registered during `createNode` (before node start, as the
library requires) and cleared on module destruction, which fails any in-flight
requests.

## Result payloads are the RLN module's replies, verbatim

`resultJson` passes through this module unparsed and must be the RLN module's
own reply, in the dialect of the method answered:

- **result-dialect methods** — `start`, `stop`, `generate_proof`,
  `validate_proof`, `get_epoch_quota` — reply with the module's LogosResult
  envelope `{"success":bool,"value":<reply>,"error":<string>}`, where on
  failure `error` is the JSON-encoded typed object
  `{"class":…,"kind":…,"message":…}`.
- **tstr-dialect methods** — `register_membership`, `get_membership_state` —
  reply with the module's compact JSON object; failures are the in-band
  envelope `{"error":{"class":…,"kind":…,"message":…}}`.

`class` is the spec's RlnErrorKind, lowercase: `not_ready` (retry once
ready), `transient` (MAY retry), `budget_exhausted` (retry next epoch),
`permanent` (retrying as-is cannot succeed). An invalid proof is **not** an
error — it is a success whose value carries the verdict object
(`{"verdict":"invalid"}`; verdicts are lowercase: `valid` | `invalid` |
`duplicate` | `rate_limit_violation`). Error means the module failed to
answer.

## Timeouts

There is no response deadline to manage on this side. The delivery library
awaits each response per the RLN module's documented time budgets — 95 s for
the registry-reading calls (`register_membership`, `generate_proof`,
`get_membership_state`), 10 s for the local ones — before synthesizing a
`TRANSIENT` failure itself; a response arriving after that fails with
"unknown or already-completed reqId". The RLN module never sends "timeout".

## Enabling: `"rln-relay-lez": true`

The bridge is enabled by the node config itself: when `createNode` sees
`rln-relay-lez` set true (sniffed read-only from wherever the config shape
carries kernel/messaging settings — `kernelConf`, `messagingOverrides`, or
the flat top level), the library will outsource RLN, so the module enables
the bridge to serve it; a bridge setup failure fails `createNode`. The
`rlnBridgeEnable` module method also enables it directly — mainly for tests,
since without that key the library never issues RLN requests. Each op is
served by calling the co-loaded `liblogos_rln_module` —
declared in `metadata.json#dependencies`, so logos-core loads it
automatically; its own dependency `liblogos_lez_rln_module` must be
installed too — and the reply is fed back verbatim.

Two worker lanes keep slow registry operations off the message hot path:

| lane | operations | transport | timeout |
|---|---|---|---|
| slow | `register_membership` | raw lp | 190 s |
| slow | `get_membership_state`, `generate_proof` | raw lp | 70 s |
| fast | `start`, `stop`, `get_epoch_quota`, `validate_proof` | generated typed client | the library's own 10 s budget expires first |

The only replies the bridge fabricates are transport failures (class
`transient`, or `permanent` for a bridge bug), shaped per the answered
method's error format; a refusal that carries no error text is a dispatch
rejection, reported as a `transient` transport failure rather than forwarded.

The `rln*Request` events keep emitting for observability, but an external
responder must not also answer an enabled node — its second response per
reqId is rejected and logged.

## Bridge disabled

Callback registration is unconditional, but a config without `rln-relay-lez`
never triggers RLN operations. If a request does fire with the bridge
disabled, the library's own timeout answers it with a `TRANSIENT` failure;
everything non-RLN keeps working.
