# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
outsources every RLN operation to an external RLN module across a C callback
surface (`liblogosdelivery_rln.h`, one typed callback per RLN function). This
module is the bridge: it registers the callbacks with the library and exposes
the request/response loop on the module surface, so the host can route
requests to whichever module implements RLN. The header is mirrored at
`tests/stubs/lib/liblogosdelivery_rln.h`, pinned to the logos-delivery flake
input rev — keep the two in sync when bumping the input.

## Request/response loop

1. The delivery library needs an RLN operation and fires that operation's
   registered callback. This module emits the matching typed event:

   ```
   rlnStartRequest(reqId, timestamp)
   rlnStopRequest(reqId, timestamp)
   rlnRegisterRequest(reqId, registryId, rlnIdentifier, optionsJson, timestamp)
   rlnGetMembershipStateRequest(reqId, registryId, rlnIdentifier, timestamp)
   rlnGetEpochQuotaRequest(reqId, registryId, rlnIdentifier, epochTimestamp, timestamp)
   rlnGenerateProofRequest(reqId, registryId, rlnIdentifier, signalHex, epochTimestamp, timestamp)
   rlnVerifyProofRequest(reqId, registryId, rlnIdentifier, signalHex, epochTimestamp, proofJson, timestamp)
   ```

   `epochTimestamp` is the ABI's epoch/quota timestamp; the trailing
   `timestamp` is the local emission time, as on every other event.

2. Whoever handles the event (the RLN module, or a router in front of it)
   performs the operation and answers with the same `reqId`:

   ```
   rlnRespond(reqId, resultJson)
   ```

   `reqId` values >= 2^63 appear negative (int64 view of the library's uint64
   id); echo them back unchanged — they round-trip bit-exactly.

Callbacks are registered during `createNode` (before node start, as the
library requires) and cleared on module destruction, which fails any in-flight
requests.

## Option, proof and result payloads are opaque JSON

Scalar arguments (`registryId`, `rlnIdentifier`, `signalHex`,
`epochTimestamp`) are typed event parameters. The JSON arguments —
`optionsJson`, `proofJson` and every `resultJson` — pass through this module
verbatim, never parsed. Their schema is the contract between the delivery
library and the RLN module (see the RLN Module API spec, logos-lips #376, and
the wire-schema notes in logos-delivery). The essentials a responder must
honor:

- Every `resultJson` is a reply envelope with exactly one of:
  - `{"ok": <op-specific result>}`
  - `{"err": {"kind": "<KIND>", "message": "<detail>"}}`
- `kind` is one of `NOT_READY`, `TRANSIENT`, `BUDGET_EXHAUSTED`, `PERMANENT`.
- An invalid proof is **not** an `err` — it is an `ok` with an `INVALID`
  verdict. `err` means the module failed to answer.

## Timeouts

There is no response deadline to manage on this side. If no `rlnRespond`
arrives in time, the delivery library synthesizes a `TRANSIENT` failure
itself; a late `rlnRespond` for that request then fails with
"unknown or already-completed reqId". The RLN module never sends "timeout".

## No RLN module present

Registration is unconditional. If no module handles the `rln*Request` events,
the library's own timeout answers each request with a `TRANSIENT` failure;
everything non-RLN keeps working.
