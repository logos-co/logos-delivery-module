# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
outsources every RLN operation to an external RLN module across a C callback
surface (`liblogosdelivery_rln.h`). This module is the bridge: it registers the
callbacks with the library and exposes the request/response loop on the module
surface, so the host can route requests to whichever module implements RLN.

## Request/response loop

1. The delivery library needs an RLN operation and fires the registered
   callback. This module emits the `rlnRequest` event:

   ```
   rlnRequest(reqId, op, payloadJson, timestamp)
   ```

   `op` is one of `start`, `stop`, `register_membership`,
   `get_membership_state`, `get_epoch_quota`, `generate_proof`, `verify_proof`.

2. Whoever handles the event (the RLN module, or a router in front of it)
   performs the operation and answers with the same `reqId`:

   ```
   rlnRespond(reqId, resultJson)
   ```

Callbacks are registered during `createNode` (before node start, as the
library requires) and cleared on module destruction, which fails any in-flight
requests.

## Payloads are opaque JSON

This module never parses `payloadJson` or `resultJson` — both pass through
verbatim. The schema is the contract between the delivery library and the RLN
module (see the RLN Module API spec, logos-lips #376, and the wire-schema notes
in logos-delivery). The essentials a responder must honor:

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

Registration is unconditional. If no module handles `rlnRequest`, the
library's own timeout answers each request with a `TRANSIENT` failure;
everything non-RLN keeps working.
