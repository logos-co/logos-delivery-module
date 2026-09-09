# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
asks an external RLN module for RLN operations. This module answers those
requests in-process: `src/rln_bridge.cpp` calls the co-loaded
`liblogos_rln_module` and feeds each reply back unchanged. Every request is
also emitted as an `rln*Request` event for observability; `rlnRespond`
exists to answer a request from outside, but on a bridge-enabled node the
bridge answers first and a second response per reqId is rejected.

The RLN module's lifecycle is the exception: this module drives it directly,
not through the library's request path. `start` calls the RLN module
synchronously (config hardcoded for now) and only dispatches node start once
the RLN module is up — a failure fails `start` itself, before the node is
touched. Teardown stops the RLN module after the node is destroyed, so
in-flight validations still have a responder. The library's start/stop
callback slots are left unregistered (it null-checks each slot), so its own
bring-up attempt fails instantly with "RLN module not registered" — a notice
in its log, not a node-start gate — instead of waiting out a 10 s timeout.
There are no `rlnStartRequest` / `rlnStopRequest` events.

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

- `rln-lez: true` is the switch: the library outsources RLN, and this
  module enables its bridge to answer (a bridge setup failure fails
  `createNode`). The `rlnBridgeEnable` method does the same without config —
  mainly for tests.
- `liblogos_rln_module` is declared in `metadata.json#dependencies`, so the
  host auto-loads it along with its own deps (`liblogos_lez_rln_module`,
  `lez_core`).
- Bring-up: the module starts the RLN module first (synchronous, verified —
  a failure fails `start` before the node is dispatched).

## Running the e2e

`tests/e2e/run.sh` boots a logosctl daemon over the four module bundles and
asserts the bring-up chain; its header documents the env knobs. The daemon
log — `<run dir>/session/logs/daemon.log` — carries the library's log lines.

## Time budgets

The library gives each request a budget before synthesizing a TRANSIENT
failure itself: 200 s for `register_membership`, 80 s for the other registry
reads (`get_membership_state`, `generate_proof`), 10 s for the rest. The
bridge's raw-call timeouts (190 s register, 70 s reads) sit just under
those; `get_epoch_quota` and `validate_proof` go through the generated typed
client, whose reply arrives well inside the 10 s budget. The module-driven
`start`/`stop` also use the typed client, blocking the caller until the RLN
module answers.
