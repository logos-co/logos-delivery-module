# RLN bridge

The delivery library (liblogosdelivery) does not implement RLN itself — it
asks an external RLN module for every RLN operation. Its plugin is
implementation-agnostic: it never names a registry or a membership, carries
no configuration and never starts the backend. All of that lives here. This
module answers those requests in-process: `src/rln_bridge.cpp` adds the
registry id and rln identifier from the node's preset, calls the co-loaded
`rln_module` through its generated typed client and feeds each reply
back. Every request is also emitted as an `rln*Request` event for
observability; `rlnRespond` exists to answer a request from outside, but on a
bridge-enabled node the bridge answers first and a second response per reqId
is rejected.

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

Of the shipped presets only `logos.test` has RLN **on**; `logos.dev` and the
preset-less `""` have it off (see [`networks.md`](./networks.md)).

| `logos.test` | |
|---|---|
| Registry | `logos:testnet:fd322d32a46ddf57eece9db8d68e9b722262d1f770760fca63b8cb9e7775b971` — the LEZ testnet registration program's config account (base58 `J3NXPWUspX2zqJU3tUbR6MHmm83aJc5AzvidEaBBKnat`) |
| LEZ zone | LEZ v0.3.1, sequencer `https://testnet.lez.logos.co/`, channel `0101010101010101010101010101010101010101010101010101010101010101` |
| Epoch size | 600 s |
| Max epoch gap | the RLN module's default (the preset sets none) |
| RLN identifier | this application's, `sha256("rln/logos-delivery/v0.0.1")` |
| Validation | off — nodes attach proofs but do not validate incoming ones |

The RLN stack is three modules: `rln_module` (0.16.2), its registry
provider `rln_lez_module` (6.1.1) and its allocator client
`membership_allocation_client_module` (0.1.0), which dials through
`libp2p_module` (≥ 1.1.0). `rln_lez_module` maps `logos:testnet` to that
sequencer through its built-in network table, so a `logos.test` node needs no
LEZ environment.

The `logos.test` registry is on the LEZ v0.3 testnet zone; `rln_lez_module`
6.1.1 links LEZ v0.4.0-rc3.

Sending needs an **active RLN membership** in that registry. Receiving
doesn't. A node without a membership starts, subscribes, relays and can
validate proofs. Only its sends fail. How the node gets one is the preset's
business too: an entry that names an allocator key (below) makes the RLN
module ask the deployment's membership allocator first and fall back to a
registration the node pays for itself; an entry with no allocator key only
registers from its own funded payer. The builtin `logos.test` entry has
allocator discovery on and names the fleet's bootstrap nodes.

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
- Bringing the backend up reaches the chain, so no caller waits for it: one
  worker in the module brings the RLN backend up and down, in order.
  `createNode` returns without waiting, and `start()` and `stop()` return
  once dispatched. Follow it with `rlnState` or the `rlnStateChanged` event.
  The states are `Disabled`, `Initializing`, `Ready`, `Failed` and `Stopped`:
  `Disabled` → `Initializing` → `Ready` | `Failed` on the way up; `Stopped`
  after `stop()`, also with `manage-backend: false`. The next `start` brings
  the backend up again (`Initializing` → `Ready` | `Failed`), so `Ready`
  holds only while the backend runs.
- A node start that fails is cleaned up by the same worker: it stops the node,
  then the backend, and the state ends in `Stopped`. Until that has run,
  `start()` is refused with an error; call it again after `Stopped`.
- With Mix on, wait for `rlnState` `Ready` before calling `start()`. The Mix
  proof plugin reads the registry parameters once, at node start, and fails
  the start if the backend is not up. A `start()` after `stop()` begins the
  backend's bring-up and the node start together, so on a Mix node that start
  fails unless the backend comes up first: call `rlnBridgeEnable()` after
  `stop()` and wait for `Ready`.
- This module starts `rln_module` itself; the library no longer
  does. A bridge that cannot come up is not fatal: the `rln*Request` events
  plus `rlnRespond` remain, but nothing starts the backend on that path.
- `rln_module` is an `optional_dependency`, so the host loads it with this
  module when it is installed and skips it when it is not: a node whose
  preset has RLN off runs without the RLN stack installed at all. A node on
  an RLN-enabled preset needs it — and its own deps, `rln_lez_module` and
  `membership_allocation_client_module` (which needs `libp2p_module`) —
  installed next to this module, or bring-up ends in `Failed`.
- Bring-up fires `start` from this module. If it fails, `rlnState` goes to
  `Failed` with the RLN module's error. The one call names every registry
  the node takes a membership on — the relay one, and the mix one when the
  preset has it — so the RLN module provisions a membership per registry,
  each at its own rate. With an allocator in the preset each entry also carries
  the `allocator` block the RLN module asks first (its `provisioning.step`
  reads `delegating`, then `done`; a refusal or an unreachable allocator
  shows as `provisioning.fallback_reason` while the funded steps run). The
  allocator is dialled by `membership_allocation_client_module` over this node's
  `libp2p_module`, so bring-up starts that node first (shared with plugin
  discovery, started once), always mounted for service discovery
  (`mountKad`, `mountServiceDiscovery`) and bootstrapped to the deployment's
  DHT peers (see [A deployment with a membership allocator](#a-deployment-with-a-membership-allocator)).
  Both are in the RLN module's dependency chain (`rln_module` →
  `membership_allocation_client_module` → `libp2p_module`), so loading the
  RLN stack brings them along; a `libp2p_module` that fails to come up is a
  notice in the log and provisioning falls back to funding. A preset
  `rate-limit` or `mix-rate-limit` outside the bounds the
  registry's chain config admits (`get_registry_parameters`
  `min_rate_limit..=max_rate_limit`) also ends bring-up in `Failed`, with the
  bounds in the message, rather than as a membership that never appears.
- Pre-hashed proof methods (`generate_proof_prehashed`,
  `validate_proof_prehashed`) are not used: the library's plugin hands this
  module the signal, not its hash.
- At startup the library also checks the node's membership with
  `get_membership_state`. The result doesn't block startup: a node without an
  `active` or `grace_period` membership logs a notice and starts anyway.

`Ready` means the backend started and the bridge answers. It does not mean
the RLN module's valid-root window is warm — that is a background refresh
the RLN module does not currently expose a probe for.

With rate limiting on (`rate-limit-enabled`), every send queries
`get_epoch_quota` and is queued while `remaining` is 0.

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
the preset and the key, and the key is never read as its default.

`rate-limit` (optional) is the rate the RLN module registers the node's relay
membership at; omitted or `0`, the module's default (100) applies. The
registry's chain config bounds it (see `get_registry_bounds` on the lez
module).

### A deployment with a membership allocator

A preset entry that names an allocator key asks a membership allocator (the
RLN Membership Allocation Protocol; the serving side is
`membership_allocation_module`) first and registers from the node's own
payer only when none grants. An entry with no allocator key has no allocator
and registers funded. With an allocator, the RLN module by default
discovers the allocators advertising the registry and auth vector through
libp2p service discovery. That needs `libp2p_module` and DHT bootstrap peers
that can reach the allocator, not `pluginKadDiscovery`: the node's one libp2p
node is created with `mountServiceDiscovery: true`, `mountKad: true` and
DHT bootstrap peers from three sources, merged and deduplicated by peer id:
the node's kad peers when `pluginKadDiscovery` is on (the library reports
them only then), the RLN preset's `allocator-bootstrap-nodes` (the builtin
`logos.test` preset lists the fleet's entry nodes), and the `bootstrapNodes`
of `LIBP2P_MODULE_CONFIG` (see [run-node.md](run-node.md)). A node with no
bootstrap peer reaches no registrar, finds none and goes straight on. With
discovery on and no allocator found, the RLN module looks for one once more
before a funded payer registers, which delays that registration by about a
minute per registry. A deployment can also name one allocator in the preset,
asked after the discovered ones (or alone, with `allocator-discover: false`):

```json
{
  "": {
    "enabled": true,
    "registry-id": "logos:local:<delivery registry>",
    "epoch-size-sec": 600,
    "rate-limit": 100,
    "mix-registry-id": "logos:local:<mix registry>",
    "mix-rate-limit": 20,
    "mix-metadata-topic": "/mix/1/metadata/proto",
    "allocator-peer-id": "16Uiu2HAm…",
    "allocator-multiaddr": "/ip4/203.0.113.7/tcp/9000"
  }
}
```

| key | |
|---|---|
| `allocator-discover` | ask the allocators discovery finds first. Defaults to `true` only when the entry names another allocator key (`allocator-peer-id` / `allocator-multiaddr`, `allocator-auth-*`, `allocator-bootstrap-nodes`); an entry with none has no allocator. `false` without a peer id turns the allocator path off for the preset. The builtin `logos.test` entry has it on |
| `allocator-peer-id`, `allocator-multiaddr` | an allocator's libp2p identity and the address to dial, asked after the discovered ones; both or neither |
| `allocator-auth-type` | the auth vector (`<scheme>-<authoriser>`); default `peer-any`: any libp2p peer, one membership per peer per registry, no key material on the node |
| `allocator-auth-payload` / `allocator-auth-provider` | the vector's payload (hex) or the producer module that makes it (`rln_sign_module`); at most one; required for any vector outside the `peer` scheme. A `peer-` vector (`peer-any`, `peer-allowlist`) signs nothing, its payload is empty: nothing needs to be set for it |
| `allocator-auth-args` | forwarded to the producer verbatim |
| `allocator-bootstrap-nodes` | DHT peers the allocator path bootstraps from, beside the node's own, as `/…/p2p/<peerId>` multiaddrs; the builtin `logos.test` preset lists the fleet entry nodes |

One allocator serves both registries: the RLN module names the registry in
each request, so relay and mix memberships come from the same allocator at
their own rates. `membership_allocation_client_module` is a declared
dependency of `rln_module`, and `libp2p_module` of the client, so a node
that loads the RLN stack has them (their bundles must be
installed, or the RLN module does not load); this module brings the libp2p
node up. The allocator client (`membership_allocation_client_module`) waits
up to 20 s for a connected peer before it looks an allocator up, and the RLN
module, after a discovery miss, probes again every 60 s while the payer is
unfunded (see logos-rln-module's `docs/wire-binding.md`). An allocator that cannot
be reached leaves provisioning to fall back to funding, saying so in
`provisioning.fallback_reason`. `rlnState` reports `allocatorDiscover`,
`allocatorAuthType` and, when one is named, `allocatorPeerId` and
`allocatorMultiaddr`.

### A deployment with native Mix

Native Mix proves under its own scope on a registry whose epoch size and gap
match its proof plugin (10 s, gap 3; the plugin refuses to start against
anything else). Usually that is a second registry with a second membership,
so relay and mix budgets are independent. The preset carries that registry:

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
defaults it); `mix-epoch-size-sec` and `mix-max-epoch-gap` default to 10 and
3, and with a mix registry any other value is refused (the Mix proof plugin
requires them); without one the two keys are only type-checked. A
mix registry on a disabled entry, a missing `mix-metadata-topic` or an
identifier that is not 32 bytes of hex fail the file. `mix-rate-limit`
(optional) is the rate the mix membership is registered at; omitted or `0`,
the RLN module's default applies.

`mix-registry-id` may equal `registry-id` when the relay registry runs
`epoch-size-sec` 10 and `max-epoch-gap` 3 (written out: unset, the gap is the
RLN module's default of 1) and the two rates (`rate-limit`,
`mix-rate-limit`) agree, or one is unset: a registry runs one epoch size, one
gap and one rate. The node then holds one membership serving both scopes, at
the rate that is set; message budgets are per scope. Any other entry with the
two equal is refused, as is one whose two ids differ only in letter case: one
registry is spelled identically in both keys.

With a mix registry in the preset, a `createNode` config that turns Mix on
has this module start the backend for both registries (one `start`) and
receives `mix-rln-registry-id`, `mix-rln-identifier-hex` and
`mix-rln-metadata-topic` from the preset — an identifier or topic the config
already carries is left as written. What turns Mix on depends on the config
shape, by the library's own rule for mounting Mix, and the keys go where that
shape takes node keys:

| Shape | Mix is on with | Keys go to |
|---|---|---|
| flat (bare node keys at the top level) | `"mix": true`, or an anonymity level above `None` | the top level |
| layered (`mode`, `preset`, `messagingOverrides`) | an anonymity level above `None` in `messagingOverrides` (`mix` is not a `messagingOverrides` key) | `messagingOverrides` |
| kernel entry (`entryLayer: "kernel"`) | `"mix": true` in `kernelConf` (the kernel entry ignores the messaging layer, so a level does nothing) | `kernelConf` |

An anonymity level above `None` is `anonymityLevel` (also spelled
`anonymity-level`) `Preferred` or `Required`. A config that does not turn Mix
on gets none of the keys, and the node takes no membership on the mix
registry: the backend is started for the relay registry alone, and `rlnState`
reports no mix field. A mode with `relay-membership: false` then leaves the
node no membership at all, so such a config fails `createNode`.

A node that runs Mix runs Mix RLN exactly when its deployment runs RLN;
without Mix either is fine:

| RLN (preset) | Mix | Mix RLN | |
|---|---|---|---|
| on or off | off | | accepted |
| off | on | off | accepted |
| off | on | on (`mix-rln-registry-id` in the config) | `createNode` fails: no backend to prove on |
| on | on | off (no mix registry in the preset, no `mix-rln-registry-id` in the config) | `createNode` fails |
| on | on | on | accepted |

A `mix-rln-registry-id` set in the node
config (top level, `kernelConf` or `messagingOverrides`, also spelled
`mixRlnRegistryId`, whether or not `mix` is on) must equal the preset's mix
registry or, when the preset has none, the relay registry at 10 / 3;
anything else fails `createNode`. With `manage-backend: false` the key is not
checked: the host named the registries in its own `start`. For a node that
runs Mix, `rlnState` reports `mixRegistryId`, `mixRlnIdentifier`,
`mixEpochSizeSec` and `mixMaxEpochGap` beside the relay fields.

A host that shares the backend with other consumers sets
`manage-backend: false` and starts the RLN module itself, naming both
registries; the preset's mix keys then only feed the config and the bounds
check.

### Rates and allocators by mode

A deployment can ask different things of its Edge and Core nodes: another
rate, another allocator. Membership tiers are separate allocator nodes, told
apart by auth type: Edge nodes ask the `peer-any` one, Core nodes the
`peer-allowlist` one. Both are `peer` vectors, so neither needs a payload or
a producer. A preset entry says so under `modes`:

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
        "mix-rate-limit": 20,
        "allocator-auth-type": "peer-any"
      },
      "core": {
        "rate-limit": 600,
        "mix-rate-limit": 100,
        "allocator-auth-type": "peer-allowlist"
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
| `allocator-discover`, `allocator-peer-id`, `allocator-multiaddr`, `allocator-auth-type`, `allocator-auth-payload`, `allocator-auth-provider`, `allocator-auth-args` | the allocator a node of that mode asks |
| `relay-membership` | boolean, default `true`. `false`: the node takes no membership on the relay registry (below). Only a mode can set it |

A key a mode sets replaces the entry's value for nodes of that mode; a key it
leaves out is inherited from the entry, and a mode the entry does not name
runs the entry as written. Everything else — registries, epoch sizes,
identifiers, `allocator-bootstrap-nodes` — is the deployment's and cannot be
set in a mode.

Each mode's keys are laid over the entry and the result is read as an entry
of its own, so every rule on this page holds per mode. In the example the
entry names no allocator key, and each mode's `allocator-auth-type` turns
allocator discovery on for that mode. An unknown mode, a mode written twice
(`edge` and `Edge`), a key a mode may not set, a key of the wrong type or a
rule only one mode breaks fails the file, and the error names the mode:
`preset "" mode edge: …`. The entry as written, without `modes`, has to be
valid too.

`relay-membership: false` starts the backend for the mix registry alone: the
`start` names no relay registry and no relay rate (its top-level
`epoch_size_sec` and `max_epoch_gap` stay at the relay values, the RLN
module's defaults for a registry no call has named), and the node holds one
membership, on the mix registry. It is accepted only when the mode has a mix
registry that is not the relay registry. logos-delivery v0.40.0 requires a
Relay membership on every sending node, so such a node cannot send there:
`relay-membership: false` is usable only with a library that implements
logos-messaging/logos-delivery#4779.

`rlnState` reports what the node asks as a node of its mode: `mode` (`Core`
or `Edge`), `rateLimit` and `mixRateLimit` (a rate left to the RLN module's
default is omitted; `mixRateLimit` appears only with a mix registry) and
`relayMembership: false` when the node takes no Relay membership, in which
case there is no `rateLimit`. These are the rates asked, not the ones a
membership was granted at. `allocatorAuthType` beside them is the mode's.

What a membership was granted at is read back from the RLN module
(`get_membership_state`), because an allocator caps silently: it grants the
lower of the rate asked and its own cap, and its reply carries no rate. After
`Ready` the module reads each registry it provisions (the relay registry
unless the node takes no Relay membership, and a mix registry of its own) in
rounds: a round runs after 5 s of quiet following the previous one, backing
off to 60 s while the RLN module does not answer (5, 10, 20, 40, 60) and back
to 5 s once a read succeeds. A registry is read until its membership is
active, and nothing is read later than 10 minutes after the `Ready`. A
request that is waiting (`start`, `stop`, teardown), or a relay read the
module did not answer, postpones the mix read to the next round.
`rlnState` then also reports:

| Field | Value |
|---|---|
| `membership`, `mixMembership` | `{"state": "<state>"}`, the RLN module's state for that registry's membership (`unknown`, `pending`, `active`, ...), plus `"step": "<step>"` while the module reports a provisioning step (`delegating`, `waiting_for_wallet`, `awaiting_funding`, `registering`, `done`, `refused`) |
| `grantedRateLimit`, `grantedMixRateLimit` | the on-chain rate of that membership, once its state is `active` |

The fields are absent until the registry's first read, and again once a new
bring-up begins (a `start` after a `stop`); after the 10 minutes they keep the
last value read. A registry relay and mix share has one membership, reported
under both names. On a host-managed backend (`manage-backend: false`) the read
goes to the host's backend the same way.

When a membership becomes active at another rate than the one asked, `Ready`
is published again with a `message` that names it, and the state stays `Ready`:

```
mix membership granted at rate 20; the preset asks 100
relay membership granted at rate 20; the preset asks 100
relay membership granted at rate 20, mix membership at rate 5; the preset asks 100 and 50
relay and mix membership granted at rate 20; the preset asks 100
```

The last form is a shared registry. A rate the preset leaves to the RLN
module's default is not compared.

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
