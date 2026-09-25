# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
asks an external RLN module for every RLN operation. Its plugin is
implementation-agnostic: it never names a registry or a membership, carries
no configuration and never starts the backend. All of that lives here. This
module answers those requests in-process: `src/rln_bridge.cpp` adds the
registry id and rln identifier from the node's preset, calls the co-loaded
`liblogos_rln_module` through its generated typed client and feeds each reply
back. Every request is also emitted as an `rln*Request` event for
observability; `rlnRespond` exists to answer a request from outside, but on a
bridge-enabled node the bridge answers first and a second response per reqId
is rejected.

A `get_membership_state` reply crosses verbatim. The other three answer the
result envelope, which the typed client decodes and the bridge re-emits field
for field — the schema is still the RLN module's and the delivery library's,
not modelled here. The only replies the bridge fabricates are failures: a
transport problem is TRANSIENT, and a provider refusal — the module declining
the call itself — is PERMANENT, because retrying a contract mismatch cannot
fix it. If nothing answers a request at all, the library times it out itself
and everything non-RLN keeps working.

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
- `liblogos_rln_module` is an `optional_dependency`, so the host neither
  loads it nor requires it: a node whose preset has RLN off runs without the
  RLN stack installed at all. A node on an RLN-enabled preset loads it — and
  its own dep, `liblogos_lez_rln_module` — before
  `createNode`, or bring-up ends in `Failed`.
- Bring-up fires `start` from this module, then the library's
  `get_membership_state` gate: the node's membership must already be
  `active` or `grace_period` — registration happens out-of-band, through the
  RLN module, not through this library or its plugin.
  Without one (e.g. no chain), `start` fails with the RLN module's own
  error carried verbatim into `nodeStarted`.

`Ready` means the backend started and the bridge answers. It does not mean
the RLN module's valid-root window is warm — that is a background refresh
the RLN module does not currently expose a probe for.

Every send queries `get_epoch_quota` and is queued while `remaining` is 0.

### Presets for a test or local deployment

`LOGOS_DELIVERY_RLN_PRESETS` names a JSON file whose entries are merged over
the built-in table, which is how a rig points a node at its own registry
without any public API for it:

```json
{
  "": {
    "enabled": true,
    "registry-id": "logos:testnet:<64 hex chars — the registration program's config account>",
    "epoch-size-sec": 120,
    "max-epoch-gap": 1
  }
}
```

`rln-identifier` is optional and defaults to this application's scope,
`sha256("rln/logos-delivery/v0.0.1")`. Name one only for a deployment that
needs a scope of its own — every node that must validate another's proofs has
to use the same value, and two that disagree reject each other's messages as
invalid rather than reporting a misconfiguration. The 32 bytes are arbitrary
to the protocol: the circuit path reduces them with `hash_to_field_le`
(Keccak-256) however they were chosen.

Keys are preset names spelled exactly as the delivery library spells them —
`""`, `twn`, `logos.dev`, `logos.test`, `status.prod` — and matched exactly.
The empty name above is the preset-less config the library also accepts.

A variant spelling is an error, not a miss: `logostest` in this file, or in a
node's `preset`, fails rather than resolving to `logos.test`. The library
accepts some of those variants for its own network config, so a node could
otherwise come up on the right network with RLN silently off.

A file that cannot be read or parsed, an unknown preset name, or an enabled
entry missing a required field all fail `createNode` rather than quietly
producing a node without the rate limiting its deployment expects.

## Running the e2e

`tests/e2e/run.sh` boots a logosctl daemon over the four module bundles and
asserts the bring-up chain; its header documents the env knobs. The daemon
log — `<run dir>/session/logs/daemon.log` — carries the library's log lines.

## Time budgets

The library gives each request a budget before synthesizing a TRANSIENT
failure itself: 80 s for the registry reads (`get_membership_state`,
`generate_proof`), 10 s for the rest. Each request op is one
`<name>AsyncResult` call on the generated client carrying its own deadline —
70 s for the reads (just under the library's budget, so the bridge's answer
lands first), 10 s for `get_epoch_quota` and `validate_proof` — and the reply
arrives on the client's completion callback. `start` and `stop` are
synchronous typed-client calls with no library clock behind them; they carry
20 s, which is what the protocol default already gave them.
