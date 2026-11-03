# logos-delivery-module e2e tests

End-to-end checks that run the built `delivery_module` + `liblogosdelivery`
inside real `logoscore` daemon containers and drive them over RPC. Per PR: red on
regressions in `delivery_module` / `liblogosdelivery` / the codegen+RPC path.

Mock-based C++ unit tests (and the in-process C++ integration tests) live in
`../tests/`. This is a separate suite that runs the real stack across a container
boundary, which the C++ tests can't.

## Architecture

Two `delivery_module` daemons in a shared docker network, peered **directly** via
`staticnodes` — no bootstrap node. Node A starts first; node B is configured with
A's container-routable multiaddr and dials it; gossipsub/relay handles the rest.

```
docker network "logosdelivery-e2e-<uuid>"  (one /24 of 172.31.0.0/16 per xdist worker)
  ┌────────────────────────────┐        ┌────────────────────────────┐
  │ logoscore-a  (node A)       │◀──────▶│ logoscore-b  (node B)       │
  │ relay, cluster 198, shard 0 │ relay  │ staticnodes=[A multiaddr]   │
  └────────────────────────────┘        └────────────────────────────┘
```

This mirrors the proven two-node relay setup in
[`logos-delivery-interop-tests`](https://github.com/logos-messaging/logos-delivery-interop-tests)
(`tests/wrappers_tests/test_send_relay_propagation.py`, scenario S06): relay-only,
single shard, discv5 off. The single-node lifecycle tests use one daemon each.

## What's tested

| Test | Daemons | Asserts |
|---|---|---|
| `test_create_start_and_stop_node` | 1 | `load_module → createNode → start → stop` all succeed over RPC |
| `test_createNode_twice_rejected` | 1 | second `createNode` returns `success=false` |
| `test_query_and_subscribe_methods` | 1 | queries (`getAvailableConfigs`/`getAvailableNodeInfoIDs`/`getNodeInfo`) + `subscribe`/`unsubscribe` round-trip |
| `test_two_nodes_propagation` | 2 | **gate** — sender sees `messagePropagated` for its `requestId` |
| `test_bidirectional_propagation` | 2 | A→B and B→A both propagate |
| `test_two_nodes_message_received` | 2 | receiver sees `messageReceived` (see below) |
| `test_validating_node_drops_message_without_proof` | 3 | a message sent without an RLN proof is received by the non-validating node and rejected by the validating one (see "RLN") |
| `test_send_with_rln_off_propagates` | 2 | a sender with RLN off sees `messagePropagated` |
| `test_send_without_membership_reports_error` | 2 | a sender with RLN on and no membership sees `messageError` |
| `test_node_registers_membership` | 1 | a node's RLN module waits for funding, then registers an `active` membership (see "RLN") |
| `test_validating_node_receives_message_with_proof` | 2 | a message sent with an RLN proof reaches a validating node |
| `test_validating_service_relays_lightpush_with_proof` | 3 | a light push client's message with an RLN proof passes a validating light push server and reaches its relay peer |

We assert `messagePropagated`, not `messageSent`: with relay-only / no store peer,
`messageSent` never fires (interop S06 vs S07).

## Event payload shape (read before extending receive assertions)

`delivery_module`'s public methods are **synchronous** — `client.call(...)` returns
the `StdLogosResult` `{success, value, error}` directly; there are no per-call
result events. Only message delivery is async, via the typed events
`messageReceived` / `messageQueued` / `messagePropagated` / `messageSent` /
`messageError` / `connectionStateChanged`.

The logoscore CLI serializes a typed event as `{"event": <name>, "data": {"arg0":
<1st arg>, "arg1": <2nd arg>, ...}}` — each codegen event arg lands at `argN` by
position (so `messagePropagated.requestId` = `arg0`, `messageReceived.contentTopic`
= `arg1`). `parse_event` / `event_arg` in `libs/helpers.py` read that.

`messageReceived` fires and its `contentTopic` (`arg1`) is asserted. The
`vector<uint8_t>` `payload` arg's serialization is **not yet asserted** — if you
extend the receive checks to the payload, read `messageReceived payload: …` from
the logs and confirm the decoding in `libs/helpers.py` first.

`source` (`arg3`) is `"live"` or `"history"`. A node that has been down and
comes back can replay what it missed as `"history"`, so a test asserting on a
freshly published message should pin `arg3` to `"live"`.

## RLN

`test_rln.py` runs nodes with RLN on a local LEZ chain. `lez_registry` in `conftest.py`
starts the standalone LEZ sequencer on the worker's docker network and deploys an RLN
registry on it with logos-lez-rln's `run_setup`. Nodes read the sequencer's URL from
`LEZ_RLN_SEQUENCER` and the registry id from the presets file named by
`LOGOS_DELIVERY_RLN_PRESETS`. After `lez_registry.fund` funds a node's LEZ payer, the
node's RLN module registers a membership by itself. Funding waits for the chain's `CLOCK_50`,
because the registry refuses registrations before it.

## Prerequisites

- `docker` available on host.
- `LOGOSCORE_IMAGE` env: `logoscore:smoke-portable`, built once from
  `logos-co/logos-logoscore-py/tests/docker_smoke/build_smoke_image.sh`.
- `LOGOS_MODULES_DIR` env: a directory containing `delivery_module/manifest.json`
  (the output of `nix build .#install-portable` → `result/modules`).
- A host-side `logoscore` CLI on `PATH` (the client shells out to it). Build it from
  `logos-co/logos-logoscore-cli` pinned to the same commit as the smoke image.
- For `test_rln.py`:
  - `liblogos_rln_module` and `liblogos_lez_rln_module` in `LOGOS_MODULES_DIR` (CI
    stages them next to `delivery_module`).
  - `E2E_LEZ_SEQUENCER_IMAGE` env: the standalone LEZ sequencer image, pulled.
  - `E2E_LEZ_RLN_DIR` env: a logos-lez-rln checkout with its guest programs and tools
    built (see "Running locally — Linux").

If any of these is missing, the e2e tests **fail** and name it. `-m "not rln"` leaves
the RLN tests out.

## Running locally — Linux

```bash
# 1. Build the module in install-portable layout.
nix build .#install-portable
export LOGOS_MODULES_DIR=$PWD/result/modules

# 2. Build the logoscore smoke image (one-time) + CLI, pinned together, at the
#    commits of .github/actions/setup-e2e.
git clone https://github.com/logos-co/logos-logoscore-py.git /tmp/logoscore-py
cd /tmp/logoscore-py && git checkout <smoke-image-commit>
bash tests/docker_smoke/build_smoke_image.sh
export LOGOSCORE_IMAGE=logoscore:smoke-portable
cd -
nix build --out-link /tmp/logoscore-cli \
  "github:logos-co/logos-logoscore-cli/<logoscore-cli-commit>#cli"
export PATH="/tmp/logoscore-cli/bin:$PATH"

# 3. For test_rln.py: the LEZ sequencer image and logos-lez-rln, at the pins of
#    .github/actions/setup-lez-rln.
export E2E_LEZ_SEQUENCER_IMAGE=<sequencer-image>
docker pull "$E2E_LEZ_SEQUENCER_IMAGE"
curl -L https://risczero.com/install | bash
~/.risc0/bin/rzup install cargo-risczero <cargo-risczero-version> && ~/.risc0/bin/rzup install rust <risc0-rust-version>
git clone https://github.com/logos-co/logos-lez-rln.git /tmp/logos-lez-rln
cd /tmp/logos-lez-rln && git checkout <lez-rln-commit>
cargo risczero build --manifest-path lez-rln/methods/guest/Cargo.toml
cargo build --release --manifest-path lez-rln/Cargo.toml --bin mint_payer --bin run_setup --bin fund_account
export E2E_LEZ_RLN_DIR=/tmp/logos-lez-rln
cd -

# 4. Install deps + run.
cd tests/e2e
pip install -r requirements.txt
pytest -v -n 4 --dist=loadgroup
```

The sequencer image is amd64 only, so Docker on Apple silicon runs it emulated.

## Running locally — macOS

Docker-mode on macOS (arm64) needs a Linux remote builder for
`nix build .#install-portable` (the Linux `delivery_module_plugin.so` + bundled
dylibs), which isn't shipped here. Use CI to validate, or a Linux host.

## Configuration schema

Node configs are built by `libs/helpers.make_delivery_config()` and follow the
`createNode` `WakuNodeConf` JSON schema (see `../README.md#node-configuration-createnode`):

| Field | Value here | Why |
|---|---|---|
| `relay` | `true` | gossipsub relay path under test |
| `clusterId` | `198` | isolated test cluster (interop's `DEFAULT_CLUSTER_ID`) |
| `numShardsInNetwork` | `1` | single shard → every content topic maps to shard 0 |
| `discv5Discovery` | `false` | peering is explicit via `staticnodes`, not discovery |
| `nat` | `none` | a container has no router to map ports on, and the mapping can take QUIC's port |
| `staticnodes` | `[A multiaddr]` (node B only) | direct dial of node A |
| `store` / `filter` | `false` | not exercised here |
| `lightpush` / `lightpushnode` | `false` / unset | set by the light push test only, `lightpush` on its server and `lightpushnode` on its `relay: false` client |

> The cluster + shard must match across both nodes (they do — same config builder).
> A peer is reached via `getNodeInfo("Multiaddrs")` with the container IP
> substituted in (`helpers.node_multiaddr`), since the advertised address is `0.0.0.0`.

## Troubleshooting

Teardown saves every container's log to `$E2E_LOG_DIR` (default `/tmp`). CI uploads
them on failure as the `e2e-test-logs` or `e2e-tests-rln-logs` artifact. Set
`LOGOSCORE_PY_FORWARD_OUTPUT=1` to also mirror CLI + daemon output into pytest logs.

Each xdist worker's docker network takes its own /24 of `172.31.0.0/16`. If another
network on the host already uses that range, `docker network create` fails with "Pool
overlaps". Set `E2E_NETWORK_SUBNET` to a free range with a /24 per worker, such as a /22
for `-n 4`.

## CI

`.github/workflows/ci.yml` runs the tests on four xdist workers in two jobs, both set up
by `.github/actions/setup-e2e`. `e2e-tests` runs the tests not marked `rln`, with
`--dist=loadgroup` to keep the tests that share `node_a` and `node_b` on one worker.
`e2e-tests-rln` runs the `rln` tests and is not a required check.
