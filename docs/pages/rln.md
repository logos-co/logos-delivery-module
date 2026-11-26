# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
asks an external RLN module for every RLN operation. Its plugin is
implementation-agnostic: it never names a registry or a membership, carries
no configuration and never starts the backend. All of that lives here. This
module answers those requests in-process: it adds the registry id and rln
identifier from the node's preset, calls the co-loaded `rln_module` through
its generated typed client (`src/rln_bridge.cpp`) and feeds each reply back.
Every request of the library's RLN plugin is also emitted as an `rln*Request`
event for observability. `rlnRespond` does not let an outside responder
answer instead: the bridge answers every request first, and a second response
per reqId is rejected.

A `get_membership_state` reply crosses verbatim. The other three answer the
result envelope, which the typed client decodes and the bridge re-emits field
for field — the schema is still the RLN module's and the delivery library's,
not modelled here. The only replies the bridge fabricates are failures: a
request before the bridge is enabled is NOT_READY, a transport problem is
TRANSIENT, and a provider refusal — the module declining the call itself —
is PERMANENT, because retrying a contract mismatch cannot fix it. If nothing answers a request at all, the library times it out itself
and everything non-RLN keeps working.

## Turning RLN on

**There is no RLN method to call.** RLN comes from the network `preset` in
the `createNode` config, because every value it needs — the registry, the
epoch size, the application identifier — is a property of the deployment
rather than of the caller. A client picks a network and gets whatever rate
limiting that network runs.

Of the shipped presets `logos.test` runs relay RLN and `logos.dev` runs relay
RLN and Mix RLN; the preset-less `""` has it off (see
[`networks.md`](./networks.md)). The `logos.dev` registries, epoch size and
metadata topic are placeholders until its LEZ deployment is chosen: a node on
them cannot register yet.

Relay (messaging) RLN and Mix RLN are independent memberships. The preset
decides which exist; the `createNode` config can only drop one, never add one:

| Membership | The preset has it with | The config drops it with |
|---|---|---|
| relay RLN | `enabled: true` | `rln-relay: false`, or an `rln-relay-eth-contract-address` (the library's on-chain backend instead) |
| Mix RLN | `mix-registry-id` | anything that leaves Mix off, by the library's rule: an anonymity level of `None`, else `mix: false`, else a network preset that runs no Mix (Mix runs by default on `logos.dev` and `logos.test`) |

Either key may be in any layer the library reads (top level, `kernelConf`,
`messagingOverrides`). With relay RLN dropped, or with a preset that has only a
mix registry, the library's relay plugin is not installed: Mix proofs reach the
backend over the mix bridge, and Core nodes publish the proof metadata without
a relay proof.

| `logos.test` | |
|---|---|
| Registry | `logos:testnet:fd322d32a46ddf57eece9db8d68e9b722262d1f770760fca63b8cb9e7775b971` — the LEZ testnet registration program's config account (base58 `J3NXPWUspX2zqJU3tUbR6MHmm83aJc5AzvidEaBBKnat`) |
| LEZ zone | LEZ v0.3.1, sequencer `https://testnet.lez.logos.co/`, channel `0101010101010101010101010101010101010101010101010101010101010101` |
| Epoch size | 600 s |
| Max epoch gap | the RLN module's default (the preset sets none) |
| RLN identifier | this application's, `sha256("rln/logos-delivery/v0.0.1")` |
| Validation | off — nodes attach proofs but do not validate incoming ones |

The RLN stack is three modules: `rln_module`, its registry provider
`rln_lez_module` and its allocator client
`membership_allocation_client_module`, which reaches allocators through a
libp2p node of its own.
`flake.lock` pins the revisions this module is built and tested with.
`rln_lez_module` maps `logos:testnet` to that sequencer through its built-in
network table, so a `logos.test` node needs no LEZ environment.

Sending needs an **active RLN membership** in that registry. Receiving
doesn't. A node without a membership starts, subscribes, relays and can
validate proofs. Only its sends fail. How the node gets one is the RLN
module's business, not the preset's (see
[How a membership is obtained](#how-a-membership-is-obtained)).

**Upgrading from 0.3.2 or earlier.** Before 0.3.3 `logos.test` named
`logos:testnet:841312e989c77e3f6f58a5d880a8e25b950b8b5ffba2f39748fa44622c20c893`,
a registry 0.3.3 no longer uses. A membership registered there does not carry
over: under the new registry
`get_membership_state` reports `unknown`, and sends fail until the node holds a
membership in `logos:testnet:fd322d32…7775b971`. Register a new one from a
funded payer on that registry's zone.

- Installing the library's RLN plugin is what makes it mount RLN, and it
  reads that at node creation, so `createNode` does it before handing the
  config to the library.
- Bringing the backend up calls another module, so `createNode` runs that on
  its own thread and returns without waiting. Follow it with `rlnState` or the
  `rlnStateChanged` event: `Disabled` → `Initializing` → `Ready` | `Failed`.
- RLN comes up once, with the node context, and stays up: `stop()` and a later
  `start()` do not touch it. A backend this module started is stopped when
  the node context is released. `Failed` is final for that node.
- A node start that fails is cleaned up by the module: it stops the
  half-started node.
- With Mix on, wait for `rlnState` `Ready` before the first `start()`: the
  library starts the Mix proof plugin with the node, and a plugin that cannot
  reach the backend fails the start.
- This module starts `rln_module` itself; the library does not. With
  `manage-backend: false` the host does.
- `rln_module` is an `optional_dependency`, so the host loads it with this
  module when it is installed and skips it when it is not: a node whose
  preset has RLN off runs without the RLN stack installed at all. A node on
  an RLN-enabled preset needs it — and its own deps, `rln_lez_module` and
  `membership_allocation_client_module` —
  installed next to this module, or bring-up ends in `Failed`.
- Bring-up fires one `start` from this module, naming every registry the
  node takes a membership on (the relay one, and the mix one when the preset
  has it) with its epoch and rate. If it fails, `rlnState` goes to `Failed`
  with the RLN module's error. A rate outside a registry's bounds does not
  fail bring-up: the RLN module refuses it when it registers, and
  `rln_module.get_membership_state` shows `provisioning.step` `refused`
  while `rlnState` stays `Ready`.
- Pre-hashed proof methods (`generate_proof_prehashed`,
  `validate_proof_prehashed`) are not used: the library's plugin hands this
  module the signal, not its hash.
- At startup the library also checks the node's membership with
  `get_membership_state`. The result doesn't fail startup: a node without an
  `active` or `grace_period` membership logs a notice and starts anyway.

`Ready` means the backend started and the bridge answers. It does not mean
the RLN module's valid-root window is warm — that is a background refresh
the RLN module does not currently expose a probe for.

The library judges a send against the last `get_epoch_quota` it read: one at
start, and one each send starts in the background. Once a read with a
`rate_limit` has arrived it does not send past that rate, also with
`rate-limit-enabled` off. With it on, `rate-limit-messages-per-epoch`
(default 1) can only lower that limit.

### Presets for a test or local deployment

`LOGOS_DELIVERY_RLN_PRESETS` names a JSON file whose entries replace the
built-in ones of the same name, which is how a rig points a node at its own
registry without any public API for it:

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

An entry has `enabled` (default `false`) and, when enabled, needs
`registry-id` and a positive `epoch-size-sec`. The rest is optional:
`max-epoch-gap` (unset, the RLN module's default of 1 applies),
`enable-validation` (default `true`; `false` makes the node attach proofs
without validating incoming ones) and `manage-backend` (default `true`;
`false` leaves starting and stopping the RLN backend to the host).

`rln-identifier` is optional and defaults to this application's scope,
`sha256("rln/logos-delivery/v0.0.1")`. Name one only for a deployment that
needs a scope of its own — every node that must validate another's proofs has
to use the same value, and two that disagree reject each other's messages as
invalid rather than reporting a misconfiguration. The 32 bytes are arbitrary
to the protocol: the circuit path reduces them with `hash_to_field_le`
(Keccak-256) however they were chosen.

Keys are preset names spelled exactly as the delivery library spells them —
`""`, `twn`, `logos.dev`, `logos.test`, `status.prod`, `status.staging` — and matched exactly.
The empty name above is the preset-less config the library also accepts.

A variant spelling is an error, not a miss: `logostest` in this file, or in a
node's `preset`, fails rather than resolving to `logos.test`. The library
accepts some of those variants for its own network config, so a node could
otherwise come up on the right network with RLN silently off.

A file that cannot be read or parsed, an unknown preset name, or an enabled
entry missing a required field all fail `createNode` rather than quietly
producing a node without the rate limiting its deployment expects. So does a
key of the wrong JSON type (numbers are unsigned integers): the error names
the preset and the key, and the key is never read as its default. A key this
module does not know is ignored.

`rate-limit` (optional) is the rate asked for the node's relay membership
when the RLN module obtains one; omitted or `0`, the RLN module's default
(100) is asked. An allocator may grant less, a registry the node already
holds a membership on keeps that membership's rate, and the RLN module
refuses a rate outside the registry's bounds.

### How a membership is obtained

This module names the registries and the rates; `rln_module` (≥ 0.18.0)
obtains the memberships. For every registry it is started on it first asks a
membership allocator of the registry's network and otherwise registers from
the node's own payer (logos-rln-module's `docs/wire-binding.md`). No preset
key selects or configures this, and it shares nothing with this module's
discovery. A host that wants another allocator, or none, starts the RLN
module itself (`manage-backend: false`).

`rlnState` says that the backend started, not that the node holds a
membership: that is `rln_module.get_membership_state` with the registry and
identifier `rlnState` names.

### A deployment with native Mix

Native Mix proves under its own scope on a registry of its own, which this
module starts at 10 s epochs and a gap of 3, the values native Mix's proof
plugin runs at. That is a second registry with a second membership, so relay
and mix budgets are independent. The preset carries that registry:

```json
{
  "": {
    "enabled": true,
    "registry-id": "logos:local:<delivery registry>",
    "epoch-size-sec": 600,
    "rate-limit": 100,
    "mix-registry-id": "logos:local:<mix registry>",
    "mix-rate-limit": 20,
    "mix-metadata-topic": "/mix/1/metadata/proto"
  }
}
```

`mix-rln-identifier` defaults to native Mix's scope
(`mix-rln-spam-protection/v1`, zero-padded to 32 bytes, as the library
defaults it). The mix registry always runs at 10 s epochs and a gap of 3;
neither is configurable. `mix-registry-id` has to name another registry than
`registry-id`: the same id, also in another letter case, fails the file, as
do a missing `mix-metadata-topic` and an
identifier that is not 32 bytes of hex. `mix-rate-limit` (optional) is the
rate asked for the mix membership, on the same terms as `rate-limit`;
omitted or `0`, the RLN module's default (100) is asked, whatever
`rate-limit` says.

With a mix registry in the preset and Mix on, this module starts the backend
for the node's registries (one `start`) and the `createNode` config receives
`mix-rln-registry-id`, `mix-rln-identifier-hex` and `mix-rln-metadata-topic`
from the preset; an identifier or topic the config already carries is left as
written. The keys go where the config shape takes node keys:

| Shape | Keys go to |
|---|---|
| flat (bare node keys at the top level) | the top level |
| layered (`mode`, `preset`, `messagingOverrides`) | `messagingOverrides` |
| kernel entry (`entryLayer: "kernel"`) | `kernelConf` |

The kernel entry reads no anonymity level: there `mix` in `kernelConf`, else
the network preset, decides. A config that leaves Mix off gets none of the keys
and the node takes no mix membership. On a preset that runs RLN with a managed
backend, a `mix-rln-registry-id` set in the node config (in any layer, also
spelled `mixRlnRegistryId`) must equal the preset's mix registry; any other
value, or the key on a node that takes no mix membership, fails `createNode`.
`rlnState` reports `mixRegistryId`, `mixRlnIdentifier`, `mixEpochSizeSec` and
`mixMaxEpochGap` beside the relay fields, and the relay fields only when the
node takes the relay membership.

A host that shares the backend with other consumers sets
`manage-backend: false` and starts the RLN module itself, naming both
registries; the preset's mix keys then only feed the config, and a
`mix-rln-registry-id` the config sets is not checked.

### Rates by mode

A deployment can ask different rates of its Edge and Core nodes. A preset
entry says so under `modes`:

```json
{
  "": {
    "enabled": true,
    "registry-id": "logos:local:<delivery registry>",
    "epoch-size-sec": 600,
    "mix-registry-id": "logos:local:<mix registry>",
    "mix-metadata-topic": "/mix/1/metadata/proto",
    "modes": {
      "edge": {
        "mix-rate-limit": 20
      },
      "core": {
        "rate-limit": 600,
        "mix-rate-limit": 100
      }
    }
  }
}
```

The mode is the `createNode` config's top-level `mode`, read as the library
reads it: `Core` or `Edge` in any letter case, `Core` when absent, and always
`Core` with `entryLayer` `kernel`, where the library reads no mode.

`modes` has the keys `edge` and/or `core` (any letter case). A mode may set
only these:

| key | |
|---|---|
| `rate-limit`, `mix-rate-limit` | the rates the node's memberships are asked at |

A key a mode sets replaces the entry's value for nodes of that mode; a key it
leaves out is inherited from the entry, and a mode the entry does not name
runs the entry as written. Everything else — registries, epoch sizes,
identifiers — is the deployment's and cannot be set in a mode.

An unknown mode or a mode written twice (`edge` and `Edge`) fails the file as
`preset "" modes: …`; a key a mode may not set, or one of the wrong type, as
`preset "" mode edge: …`.

`rlnState` reports what the node asks as a node of its mode: `mode` (`Core`
or `Edge`), `rateLimit` and `mixRateLimit` (a rate left to the RLN module's
default of 100 is omitted; `mixRateLimit` appears only with a mix registry).
These are the rates asked, not the ones a membership was granted at: that is
`rln_module.get_membership_state`'s `rate_limit` once the membership is
active.

## Running the e2e

`tests/e2e/` is a pytest suite that boots `logosctl` daemons in Docker and
asserts the bring-up chain; `tests/e2e/README.md` says how to run it and
documents its env knobs.

## Time budgets

The library gives each request a budget before synthesizing a TRANSIENT
failure itself: 100 s for `get_membership_state` and `generate_proof`, 15 s
for `get_epoch_quota` and `validate_proof`. Each request op is one
`<name>AsyncResult` call on the generated client carrying its own deadline —
90 s for `get_membership_state`, `generate_proof` and `get_epoch_quota`,
10 s for `validate_proof` — and the reply arrives on the client's completion
callback. The bridge's `get_epoch_quota` deadline is longer than the
library's budget for it: a quota answer slower than 15 s arrives after the
library has given up. `start` and `stop` are
synchronous typed-client calls with no library clock behind them; they carry
20 s, which is what the protocol default already gave them.
