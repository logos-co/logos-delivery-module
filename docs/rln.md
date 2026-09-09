# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
asks an external RLN module for every RLN operation. Its plugin is
implementation-agnostic: it never names a registry or a membership, carries
no configuration and never starts the backend. All of that lives here. This
module answers those requests in-process: `src/rln_bridge.cpp` adds the
configured registry id and rln identifier, calls the co-loaded
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

- `rln-lez: true` is the switch. These keys are consumed here and stripped
  from the config before it reaches the library, which rejects them —
  `rln-relay` goes too, since with a plugin installed it would ask the
  library for its embedded EVM backend instead. Installing the plugin is
  what makes the library mount RLN over it.
- This module starts `liblogos_rln_module` itself, before `createNode`
  returns; the library no longer does. A start failure fails `createNode`.
  A bridge that cannot come up is not fatal: the `rln*Request` events plus
  `rlnRespond` remain, but nothing starts the backend on that path.
- `liblogos_rln_module` is declared in `metadata.json#dependencies`, so the
  host auto-loads it along with its own deps (`liblogos_lez_rln_module`,
  `lez_core`).
- Bring-up fires `start` from this module, then the library's
  `get_membership_state` gate: the node's membership must already be
  `active` or `grace_period` — registration happens out-of-band, through the
  RLN module, not through this library or its plugin.
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
