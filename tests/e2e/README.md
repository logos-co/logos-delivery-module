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
| `test_restored_membership_is_active` | 1 | the membership in `E2E_RLN_MEMBERSHIP_DIR` is `active` on the registry (see "RLN") |
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

`test_rln.py` runs nodes with RLN on the LEZ testnet registry used by `logos.test`.
RLN is turned on through a presets file named by `LOGOS_DELIVERY_RLN_PRESETS` instead
of the `logos.test` preset, because that preset always turns proof validation off (see
`docs/pages/rln.md`).

The tests marked `rln_membership` send with a proof, so they need an active
membership: `E2E_RLN_MEMBERSHIP_DIR` names a directory holding `liblogos_rln_module`'s
keystore folder (`liblogos_rln_module/<instance>/`), and each sending node starts with
a copy of it.

## Prerequisites

- `docker` available on host.
- `LOGOSCORE_IMAGE` env: `logoscore:smoke-portable`, built once from
  `logos-co/logos-logoscore-py/tests/docker_smoke/build_smoke_image.sh`.
- `LOGOS_MODULES_DIR` env: a directory containing `delivery_module/manifest.json`
  (the output of `nix build .#install-portable` → `result/modules`).
- A host-side `logoscore` CLI on `PATH` (the client shells out to it). Build it from
  `logos-co/logos-logoscore-cli` pinned to the same commit as the smoke image.
- For `test_rln.py`: `liblogos_rln_module` and `liblogos_lez_rln_module` in
  `LOGOS_MODULES_DIR` (CI stages them next to `delivery_module`), and outbound HTTPS to
  `testnet.lez.logos.co`. Without the modules the RLN tests skip.
- For the `rln_membership` tests: `E2E_RLN_MEMBERSHIP_DIR` (see "RLN"). Without it
  they skip.

If any of these is missing, all e2e tests **skip** with a clear reason.

## Running locally — Linux

```bash
# 1. Build the module in install-portable layout.
nix build .#install-portable
export LOGOS_MODULES_DIR=$PWD/result/modules

# 2. Build the logoscore smoke image (one-time) + CLI, pinned together.
git clone https://github.com/logos-co/logos-logoscore-py.git /tmp/logoscore-py
cd /tmp/logoscore-py && git checkout aa45db52
bash tests/docker_smoke/build_smoke_image.sh
export LOGOSCORE_IMAGE=logoscore:smoke-portable
cd -
nix build --out-link /tmp/logoscore-cli \
  "github:logos-co/logos-logoscore-cli/5a1cf746e841e8e8a8fd48333cff55bd12c45c52#cli"
export PATH="/tmp/logoscore-cli/bin:$PATH"

# 3. Install deps + run.
cd tests/e2e
pip install -r requirements.txt
pytest -v -n 4 --dist=loadgroup
```

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
| `staticnodes` | `[A multiaddr]` (node B only) | direct dial of node A |
| `store` / `filter` | `false` | not exercised here |
| `lightpush` / `lightpushnode` | `false` / unset | the light push test only: `true` on its server, and on its client (`relay: false`) the server's multiaddr |

> The cluster + shard must match across both nodes (they do — same config builder).
> A peer is reached via `getNodeInfo("Multiaddrs")` with the container IP
> substituted in (`helpers.node_multiaddr`), since the advertised address is `0.0.0.0`.

## Troubleshooting

Daemon container logs are saved to `$E2E_LOG_DIR` (default `/tmp`) on teardown, and
uploaded as the `e2e-test-logs` artifact in CI on failure. Set
`LOGOSCORE_PY_FORWARD_OUTPUT=1` to also mirror CLI + daemon output into pytest logs.

Each xdist worker's docker network takes its own /24 of `172.31.0.0/16` (worker
`gw<n>` the n-th, a run without xdist the first). If another network on the host
already uses that range, `docker network create` fails with "Pool overlaps"; set
`E2E_NETWORK_SUBNET` to a free range.

## CI

`.github/workflows/ci.yml` has an `e2e-tests` job (`needs: build-and-test`,
ubuntu-latest) that builds `.#install-portable`, builds the smoke image + CLI, runs
`pytest -v -n 4 --dist=loadgroup`, and uploads logs on failure. `build-and-test`
pre-warms the `install-portable` cachix cache so the e2e job hits it warm. Both e2e jobs
set up through the composite action `.github/actions/setup-e2e`, and run the tests on
four xdist workers; `--dist=loadgroup` keeps the tests that share `node_a` and `node_b`
on one.

The `e2e-rln-membership` job in `ci.yml` runs the `rln_membership` tests on PRs from
branches of this repo and pushes to master. It is not a required check. It restores the
membership from the repository secret
`RLN_KEYSTORE`, which is replaced after every renewal from the logosctl config dir
that holds the membership:

```bash
COPYFILE_DISABLE=1 tar czf rln-keystore.tar.gz --no-xattrs --exclude rln_keystore.lock \
  -C <config-dir>/data liblogos_rln_module
tar tzvf rln-keystore.tar.gz   # liblogos_rln_module/<instance>/rln_*: four files, no lock
base64 < rln-keystore.tar.gz > rln-keystore.b64
gh secret set RLN_KEYSTORE --repo logos-co/logos-delivery-module < rln-keystore.b64
rm rln-keystore.tar.gz rln-keystore.b64
```
