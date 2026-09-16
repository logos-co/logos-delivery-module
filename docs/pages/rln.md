# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
asks an external RLN module for every RLN operation. Its plugin is
implementation-agnostic: it never names a registry or a membership, carries
no configuration and never starts the backend. All of that lives here. This
module answers those requests in-process: `src/rln_bridge.cpp` adds the
registry id and rln identifier from the node's preset, calls the co-loaded
`liblogos_rln_module` and feeds each reply back unchanged. Every request is
also emitted as an `rln*Request` event for observability; `rlnRespond`
exists to answer a request from outside, but on a bridge-enabled node the
bridge answers first and a second response per reqId is rejected.

Replies cross verbatim — the wire schema is owned by the RLN module and the
delivery library, not modelled here. The only replies the bridge fabricates
are transport failures. If nothing answers a request at all, the library
times it out itself and everything non-RLN keeps working.

## Turning RLN on

**There is no RLN method to call.** RLN comes from the network `preset` in
the `createNode` config, because every value it needs — the registry, the
epoch size, the application identifier — is a property of the deployment
rather than of the caller. A client picks a network and gets whatever rate
limiting that network runs.

Every shipped preset has RLN **off** (see [`networks.md`](./networks.md)).

- Installing the library's RLN plugin is what makes it mount RLN, and it
  reads that at node creation, so `createNode` does it before handing the
  config to the library.
- Bringing the backend up reaches the chain, so `createNode` runs that on its
  own thread and returns without waiting. Follow it with `rlnState` or the
  `rlnStateChanged` event: `Disabled` → `Initializing` → `Ready` | `Failed`.
- This module starts `liblogos_rln_module` itself; the library no longer
  does. A bridge that cannot come up is not fatal: the `rln*Request` events
  plus `rlnRespond` remain, but nothing starts the backend on that path.
- `liblogos_rln_module` is declared in `metadata.json#dependencies`, so the
  host auto-loads it along with its own deps (`liblogos_lez_rln_module`,
  `lez_core`).
- Bring-up fires `start` from this module, then the library's
  `get_membership_state` gate: the node's membership must already be
  `active` or `grace_period` — registration happens out-of-band, through the
  RLN module, not through this library or its plugin.
  Without one (e.g. no chain), `start` fails with the RLN module's own
  error carried verbatim into `nodeStarted`.

`Ready` means the backend started and the bridge answers. It does not mean
the RLN module's valid-root window is warm — that is a background refresh
the RLN module does not currently expose a probe for.

### Presets for a test or local deployment

`LOGOS_DELIVERY_RLN_PRESETS` names a JSON file whose entries are merged over
the built-in table, which is how a rig points a node at its own registry
without any public API for it:

```json
{
  "": {
    "enabled": true,
    "registry-id": "logos:testnet:<64 hex chars — the registration program's config account>",
    "rln-identifier": "<exactly 64 hex chars — validated as 32 bytes>",
    "epoch-size-sec": 120,
    "max-epoch-gap": 1
  }
}
```

Keys are preset names as the `createNode` config spells them, matched
case-insensitively and ignoring dots (`logos.test`, `LogosTest` and
`logostest` are one entry). The empty name above is the preset-less config
the delivery library also accepts.

Entries may only use names the library knows — `""`, `twn`, `logos.dev`,
`logos.test`, `status.prod` — because it resolves the same key and rejects
anything else. A file that cannot be read or parsed, or an enabled entry
missing a required field, fails `createNode` rather than quietly producing a
node without the rate limiting its deployment expects.

## Running the e2e

`tests/e2e/run.sh` boots a logosctl daemon over the four module bundles and
asserts the bring-up chain; its header documents the env knobs. The daemon
log — `<run dir>/session/logs/daemon.log` — carries the library's log lines.

## Time budgets

The library gives each request a budget before synthesizing a TRANSIENT
failure itself: 80 s for the registry reads (`get_membership_state`,
`generate_proof`), 10 s for the rest. The bridge's raw-call timeout for a
read (70 s) sits just under that; the remaining ops go through the generated
typed client, whose reply arrives well inside the 10 s budget. The
module-driven `start` and `stop` have no library clock behind them and
borrow the 80 s read budget.
