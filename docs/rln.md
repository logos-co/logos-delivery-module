# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
asks an external RLN module for every RLN operation. This module answers
those requests in-process: `src/rln_bridge.cpp` calls the co-loaded
`liblogos_rln_module` and feeds each reply back unchanged. Every request is
also emitted as an `rln*Request` event for observability; `rlnRespond`
exists to answer a request from outside, but on a bridge-enabled node the
bridge answers first and a second response per reqId is rejected.

Replies cross verbatim — the wire schema is owned by the RLN module and the
delivery library, not modelled here. The only replies the bridge fabricates
are transport failures. If nothing answers a request at all, the library
times it out itself and everything non-RLN keeps working.

## Configuring a node for RLN testing

RLN rides `createNode`'s flat config:

```json
{
  "relay": true,
  "rln-relay": true,
  "rln-lez": true,
  "rln-registry-id": "logos:testnet:0",
  "rln-identifier": "<exactly 64 hex chars — validated as 32 bytes>",
  "rln-relay-user-message-limit": 100,
  "rln-relay-epoch-sec": 120
}
```

- `rln-relay-lez: true` is the switch: the library outsources RLN, and this
  module enables its bridge to answer (a bridge setup failure fails
  `createNode`). The `rlnBridgeEnable` method does the same without config —
  mainly for tests.
- `liblogos_rln_module` is declared in `metadata.json#dependencies`, so the
  host auto-loads it along with its own deps (`liblogos_lez_rln_module`,
  `lez_core`).
- Bring-up fires `start`, then a `get_membership_state` gate: the node's
  membership must already be `active` or `grace_period` — registration
  happens out-of-band, through the RLN module, not through this library.
  Without one (e.g. no chain), `start` fails with the RLN module's own
  error carried verbatim into `nodeStarted`.

## Running the e2e

`tests/e2e/run.sh` boots a logosctl daemon over the four module bundles and
asserts the bring-up chain; its header documents the env knobs. The daemon
log — `<run dir>/session/logs/daemon.log` — carries the library's log lines.

## Time budgets

The library gives each request a budget before synthesizing a TRANSIENT
failure itself: 200 s for `register_membership`, 80 s for the other registry
reads (`get_membership_state`, `generate_proof`), 10 s for the rest. The
bridge's raw-call timeouts (190 s register, 70 s reads) sit just under
those; the remaining ops go through the generated typed client, whose reply
arrives well inside the 10 s budget.
