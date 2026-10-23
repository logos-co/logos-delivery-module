# logos-delivery-module e2e tests

End-to-end checks that run the built `delivery_module` + `liblogosdelivery`
inside real `logosctl` daemon containers and drive them over RPC. Per PR: red on
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
  │ logosctl-a  (node A)        │◀──────▶│ logosctl-b  (node B)        │
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
| `test_lightpushed_message_reaches_filter_client` | 2 | an Edge sender's message reaches an Edge receiver through the `logos.test` fleet (see "Fleet") |
| `test_lightpushed_message_is_stored` | 2 | an Edge sender's message is in a fleet store node |
| `test_relayed_message_reaches_filter_client` | 2 | a Core sender's message reaches an Edge receiver through the fleet |
| `test_lightpushed_message_reaches_relay_node` | 2 | an Edge sender's message with an RLN proof passes a validating Core receiver through the fleet |

We assert `messagePropagated`, not `messageSent`: with relay-only / no store peer,
`messageSent` never fires (interop S06 vs S07).

## Event payload shape (read before extending receive assertions)

`delivery_module`'s public methods are **synchronous** — `client.call(...)` returns
the `StdLogosResult` `{success, value, error}` directly; there are no per-call
result events. Only message delivery is async, via the typed events
`messageReceived` / `messageQueued` / `messagePropagated` / `messageSent` /
`messageError` / `connectionStateChanged`.

`logosctl watch` serializes a typed event as `{"event": <name>, "data": {"arg0":
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
of the `logos.test` preset, whose built-in entry turns proof validation off (see
`docs/pages/rln.md`).

The tests marked `rln_membership` send with a proof, so they need an active
membership: `E2E_RLN_MEMBERSHIP_DIR` names a directory holding `liblogos_rln_module`'s
keystore folder (`liblogos_rln_module/<instance>/`), and each sending node starts with
a copy of it.

## Fleet

`test_fleet.py`, marked `fleet`, sends through the live `logos.test` fleet. Each test
runs two nodes on the `logos.test` preset (`helpers.make_fleet_config`: `mode` `Core`
or `Edge`, the preset's network and RLN, no NAT port mapping), started in parallel, and
checks that one gets what the other sent.
Each node is in its own docker network, so the message has to cross the fleet. The
sender holds the membership. The Core receiver of
`test_lightpushed_message_reaches_relay_node` validates proofs: its presets file has a
`logos.test` entry with `enable-validation` on.

Two proofs with the same `message_id` in one epoch disclose the membership's secret to
anyone who sees both, and every copy of the membership starts from the same counters.
So the fleet tests run one at a time, each sender starting from the keystore the
previous one left (`fleet_membership`). Across runs, `E2E_FLEET_EPOCH_FILE` names a file
holding the last epoch a run sent in: a run sends only in a later epoch, waiting if it
has to and printing how long, and writes the epoch when it ends. Without the file it
waits for the next epoch. CI keeps the file in the Actions cache; locally, point it at a
file of your own. Local and CI runs don't share it, so don't run the fleet tests while
another run from the same membership sends.

A node's LEZ wallet syncs the testnet from the block after its last synced one, and RLN
waits for it: from block 0 that takes over a minute. So every fleet node starts from the
wallet in `E2E_FLEET_WALLET_DIR` (`fleet_wallet`), and each node whose wallet is ready
copies its `storage.json` and `payer.json` back there when its test ends. The wallet's
config, which names the sequencer, is written anew by each node. Without the variable,
the wallet lasts one run. `storage.json` holds the wallet's keys in plain text: CI keeps
the folder in the Actions cache encrypted with the secret `E2E_FLEET_WALLET_KEY`
(`.github/actions/fleet-wallet-encryption`), and
the wallet's payer account must never be funded, since every fleet node holds its keys.

```bash
E2E_RLN_MEMBERSHIP_DIR=<membership dir> E2E_FLEET_EPOCH_FILE=~/.logos-e2e-local/fleet-epoch \
  E2E_FLEET_WALLET_DIR=~/.logos-e2e-local/fleet-wallet pytest -v -m fleet
```

`storeQuery` takes an explicit peer, so the store test queries the preset's entry nodes
(`FLEET_ENTRY_NODES`) in turn.

## Prerequisites

- `docker` available on host.
- `LOGOSCTL_IMAGE` env: `logosctl:smoke-portable`, built once from
  `logos-co/logos-logoscore-py/tests/logosctl/docker_smoke/build_smoke_image.sh`.
- `LOGOS_MODULES_DIR` env: the `modules/` of a logosctl session where `logosctl install`
  put this repo's `nix build .#lgx-portable` and its released dependencies.
- A host-side `logosctl` CLI on `PATH` (the client shells out to it): the image's own,
  copied out of `/opt/logosctl`, or a `logos-co/logos-logoscore-cli` release.
- For `test_rln.py`: `liblogos_rln_module` and `liblogos_lez_rln_module` in
  `LOGOS_MODULES_DIR` (CI installs them next to `delivery_module`), and outbound HTTPS to
  `testnet.lez.logos.co`. Without the modules the RLN tests skip.
- For the `rln_membership` tests: `E2E_RLN_MEMBERSHIP_DIR` (see "RLN"). Without it
  they skip.
- For the `fleet` tests: the membership, and outbound TCP to the `logos.test` fleet
  (port 30303).

If any of these is missing, all e2e tests **skip** with a clear reason.

## Running locally — Linux

The smoke image and the Python client come from the logos-logoscore-py commit that CI
pins as `LOGOSCORE_PY_REV` in `.github/workflows/ci.yml`. Set the same variable before
these steps.

```bash
# 1. Build the logosctl smoke image (one-time) and copy its CLI out of it.
git clone https://github.com/logos-co/logos-logoscore-py.git /tmp/logoscore-py
cd /tmp/logoscore-py && git checkout "$LOGOSCORE_PY_REV"
bash tests/logosctl/docker_smoke/build_smoke_image.sh
export LOGOSCTL_IMAGE=logosctl:smoke-portable
cd -
docker rm -f logosctl-cli 2>/dev/null
docker create --name logosctl-cli logosctl:smoke-portable
docker cp logosctl-cli:/opt/logosctl - | tar -x -C /tmp
docker rm logosctl-cli
export PATH="/tmp/logosctl/bin:$PATH"

# 2. Build the module and install it into its own logosctl session, next to the
#    released RLN modules. CI runs the same commands. Bump these versions by hand
#    when flake.lock moves the RLN modules. No test uses kad service discovery, so
#    libp2p_module is not installed.
nix build .#lgx-portable
export LOGOSCTL_CONFIG_DIR=/tmp/logosctl-modules
logosctl daemon start --detach
logosctl catalog refresh
logosctl install liblogos_lez_rln_module --version 5.1.0 -y
logosctl install liblogos_rln_module --version 0.12.0 --no-deps -y
logosctl install result/*.lgx -y
logosctl daemon stop
export LOGOS_MODULES_DIR=$LOGOSCTL_CONFIG_DIR/modules
unset LOGOSCTL_CONFIG_DIR

# 3. Install deps + run.
cd tests/e2e
pip install -r requirements.txt
pip install --no-deps --force-reinstall \
  "logoscore @ git+https://github.com/logos-co/logos-logoscore-py.git@$LOGOSCORE_PY_REV"
pytest -v -n 4 --dist=loadgroup
```

## Running locally — macOS

Docker-mode on macOS (arm64) needs a Linux remote builder for
`nix build .#lgx-portable` (the Linux `delivery_module_plugin.so` + bundled
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
uploaded in CI on failure as the `e2e-test-logs` artifact (`e2e-rln-membership-logs`
for the membership job, `e2e-fleet-logs` for the fleet one). Set
`LOGOSCTL_PY_FORWARD_OUTPUT=1` to also mirror CLI + daemon output into pytest logs.

Each xdist worker's docker network takes its own /24 of `172.31.0.0/16` (worker
`gw<n>` the n-th, a run without xdist the first). If another network on the host
already uses that range, `docker network create` fails with "Pool overlaps"; set
`E2E_NETWORK_SUBNET` to a free range with a /24 for each worker, such as a /22 for
`-n 4`. The fleet tests' networks take a range docker picks.

## CI

`.github/workflows/ci.yml` has an `e2e-tests` job (ubuntu-latest) that builds the logosctl
smoke image and copies the CLI out of it, installs `.#lgx-portable` and its released
dependencies with `logosctl install`, runs `pytest -v -n 4 --dist=loadgroup`, and uploads
logs on failure. Both e2e jobs
set up through the composite action `.github/actions/setup-e2e`, and run the tests on
four xdist workers; `--dist=loadgroup` keeps the tests that share `node_a` and `node_b`
on one.

The `e2e-rln-membership` job in `ci.yml` runs the `rln_membership` tests, except the
`fleet` ones, on PRs from branches of this repo and pushes to master. It is not a
required check. The workflow `e2e-fleet.yml` runs the `fleet` tests nightly and on
demand, never on PRs, and its runs queue (see "Fleet"). Both restore the membership
through `.github/actions/restore-rln-membership` from the repository secret
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

`e2e-fleet.yml` also needs the secret `E2E_FLEET_WALLET_KEY`, the passphrase of the
cached fleet wallet. A new one costs one run a full wallet sync:

```bash
openssl rand -base64 32 | gh secret set E2E_FLEET_WALLET_KEY --repo logos-co/logos-delivery-module
```
