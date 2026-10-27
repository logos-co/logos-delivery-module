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
| `test_node_registers_membership` | 1 | a node's RLN module waits for funding, then registers an `active` membership (see "RLN") |
| `test_validating_node_receives_message_with_proof` | 2 | a message sent with an RLN proof reaches a validating node |
| `test_validating_service_relays_lightpush_with_proof` | 3 | a light push client's message with an RLN proof passes a validating light push server and reaches its relay peer |
| `test_lightpushed_message_reaches_filter_client` | 2 | an Edge sender's message reaches an Edge receiver through the `logos.test` fleet (see "Fleet") |
| `test_lightpushed_message_is_stored` | 1 | an Edge sender sees `messageSent`, which follows a store node's confirmation |
| `test_relayed_message_reaches_filter_client` | 2 | a Core sender's message reaches an Edge receiver through the fleet |
| `test_lightpushed_message_reaches_relay_node` | 2 | an Edge sender's message with an RLN proof passes a validating Core receiver through the fleet |
| `test_relay_node_advertises_through_libp2p_module` | 2 | another `libp2p_module` node finds a relay node's advert with its peer id, address, cluster, capabilities and shard (see "Service discovery") |

Outside the fleet tests, we assert `messagePropagated`, not `messageSent`: with relay-only /
no store peer, `messageSent` never fires (interop S06 vs S07).

## Event payload shape

`delivery_module`'s public methods are **synchronous** — `client.call(...)` returns
the `StdLogosResult` `{success, value, error}` directly; there are no per-call
result events. Only message delivery is async, via the typed events
`messageReceived` / `messageQueued` / `messagePropagated` / `messageSent` /
`messageError` / `connectionStateChanged`.

`logosctl watch` serializes a typed event as `{"event": <name>, "data": {"arg0":
<1st arg>, "arg1": <2nd arg>, ...}}` — each codegen event arg lands at `argN` by
position (so `messagePropagated.requestId` = `arg0`, `messageReceived.contentTopic`
= `arg1`). `parse_event` / `event_arg` in `libs/helpers.py` read that.

`messageReceived` carries the `contentTopic` (`arg1`) and the `vector<uint8_t>`
`payload` (`arg2`). The payload arrives as `bytes`.

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

A node on the LEZ testnet starts from `lez_wallet` and sends with `rln_membership`. A LEZ
wallet syncs the chain from the block after its last synced one, and RLN waits for that
sync. From block 0 the sync takes several minutes, so `lez_wallet` is one new wallet per
session whose `last_synced_block` is the testnet's last block. A daemon with an
unreachable sequencer (`LEZ_RLN_SEQUENCER`) creates the wallet, so that it doesn't sync
either. The wallet loses nothing in the blocks it skips, because it has no private notes
there.

`rln_membership` is the testnet membership in `E2E_RLN_MEMBERSHIP_DIR`. That directory
holds `liblogos_rln_module`'s keystore folder (`liblogos_rln_module/<instance>/`).

## Fleet

`test_fleet.py` (marker `fleet`) sends through the live `logos.test` fleet. Each test
checks that a message from one node reaches a second node, or that a store node confirms
it. Each node runs in its own docker network, so the message has to cross the fleet.
Every node validates RLN proofs, because its presets file turns validation on, which the
module's built-in `logos.test` entry leaves off. An Edge node subscribes at the fleet 1 s
after its `subscribe` returns, so an Edge receiver waits 3 s before the sender sends.

Two proofs with the same `message_id` in one epoch reveal the membership's secret to
anyone who sees both. Every copy of the membership starts from the same `message_id`s,
so the fleet tests run one at a time, and each sender goes on from the keystore the
previous sender left (`fleet_membership`). Under xdist, this needs `--dist=loadgroup`.
Each run also writes the last epoch it sent in to the file `E2E_FLEET_EPOCH_FILE` names,
and the next run sends only in a later epoch. Without the file, a run waits for the next
epoch. CI keeps the file in the Actions cache and runs the fleet tests only on master.
Locally, point `E2E_FLEET_EPOCH_FILE` at a file of your own, and don't run the fleet
tests while another run sends with the same membership.

```bash
E2E_RLN_MEMBERSHIP_DIR=<membership dir> E2E_FLEET_EPOCH_FILE=~/.logos-e2e-local/fleet-epoch \
  pytest -v -m fleet
```

`logos.test` turns reliability on, so `messageSent` follows a store node's confirmation
that it holds the message, and the store test waits for it.

## Service discovery

`test_service_discovery.py` runs nodes whose kademlia service discovery is hosted on
`libp2p_module` (`pluginKadDiscovery`). Each test starts a daemon running only
`libp2p_module` as the DHT bootstrap and names it in its nodes' `kadBootstrapNodes`;
loading `delivery_module` loads the installed `libp2p_module` with it. `delivery_module`
reports nothing about discovery, so the tests read adverts with `libp2p_module`'s
`discoLookup`.

## Prerequisites

- `docker` available on host.
- `LOGOSCTL_IMAGE` env: `logosctl:smoke-portable`, built once from
  `logos-co/logos-logoscore-py/tests/logosctl/docker_smoke/build_smoke_image.sh`.
- `LOGOS_MODULES_DIR` env: the `modules/` of a logosctl session where `logosctl install`
  put this repo's `nix build .#lgx-portable` and its released dependencies.
- A host-side `logosctl` CLI on `PATH` (the client shells out to it): the image's own,
  copied out of `/opt/logosctl`, or a `logos-co/logos-logoscore-cli` release.
- For `test_rln.py`:
  - `liblogos_rln_module` and `liblogos_lez_rln_module` in `LOGOS_MODULES_DIR` (CI
    installs them next to `delivery_module`).
  - `E2E_LEZ_SEQUENCER_IMAGE` env: the standalone LEZ sequencer image, pulled.
  - `E2E_LEZ_RLN_DIR` env: a logos-lez-rln checkout with its guest programs and tools
    built (see "Running locally — Linux").
- For the `fleet` tests: the testnet membership in `E2E_RLN_MEMBERSHIP_DIR` (see "RLN"),
  outbound HTTPS to `testnet.lez.logos.co`, and outbound TCP to the `logos.test` fleet
  (port 30303).
- For `test_service_discovery.py`: `libp2p_module` in `LOGOS_MODULES_DIR`.

If any of these is missing, the e2e tests **fail** and name it. `-m "not rln"` leaves
the RLN tests out.

## Running locally — Linux

```bash
# 1. Build the logosctl image (one-time) at the logoscore-py-commit of
#    .github/actions/setup-e2e, and copy its CLI out.
git clone https://github.com/logos-co/logos-logoscore-py.git /tmp/logoscore-py
cd /tmp/logoscore-py && git checkout <logoscore-py-commit>
bash tests/logosctl/docker_smoke/build_smoke_image.sh
export LOGOSCTL_IMAGE=logosctl:smoke-portable
cd -
docker rm -f logosctl-cli 2>/dev/null
docker create --name logosctl-cli logosctl:smoke-portable
docker cp logosctl-cli:/opt/logosctl - | tar -x -C /tmp
docker rm logosctl-cli
export PATH="/tmp/logosctl/bin:$PATH"

# 2. Build the module and install it into its own logosctl session, next to the
#    released RLN modules and libp2p_module. CI runs the same commands. Bump these
#    versions by hand when flake.lock moves the modules.
nix build .#lgx-portable
export LOGOSCTL_CONFIG_DIR=/tmp/logosctl-modules
logosctl daemon start --detach
logosctl catalog refresh
logosctl install liblogos_lez_rln_module --version 5.1.0 -y
logosctl install liblogos_rln_module --version 0.12.0 --no-deps -y
logosctl install libp2p_module --version 1.1.0 -y
logosctl install result/*.lgx -y
logosctl daemon stop
export LOGOS_MODULES_DIR=$LOGOSCTL_CONFIG_DIR/modules
unset LOGOSCTL_CONFIG_DIR

# 3. For test_rln.py: the LEZ sequencer image and logos-lez-rln, at the pins of
#    .github/actions/setup-lez-rln.
export E2E_LEZ_SEQUENCER_IMAGE=<sequencer-image>
docker pull "$E2E_LEZ_SEQUENCER_IMAGE"
cargo install rzup --version <rzup-version> --locked
rzup install cargo-risczero <cargo-risczero-version> && rzup install rust <risc0-rust-version>
git clone https://github.com/logos-co/logos-lez-rln.git /tmp/logos-lez-rln
cd /tmp/logos-lez-rln && git checkout <lez-rln-commit>
cargo risczero build --manifest-path lez-rln/methods/guest/Cargo.toml
cargo build --release --manifest-path lez-rln/Cargo.toml --bin mint_payer --bin run_setup --bin fund_account
export E2E_LEZ_RLN_DIR=/tmp/logos-lez-rln
cd -

# 4. Install deps + run.
cd tests/e2e
pip install -r requirements.txt
pip install --no-deps --force-reinstall \
  "logoscore @ git+https://github.com/logos-co/logos-logoscore-py.git@<logoscore-py-commit>"
pytest -v -m "not fleet" -n 4
```

The sequencer image is amd64 only, so Docker on Apple silicon runs it emulated.

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
| `nat` | `none` | a container has no router to map ports on, and the mapping can take QUIC's port |
| `staticnodes` | `[A multiaddr]` (node B only) | direct dial of node A |
| `store` / `filter` | `false` | not exercised here |
| `lightpush` / `lightpushnode` | `false` / unset | set by the light push test only, `lightpush` on its server and `lightpushnode` on its `relay: false` client |
| `pluginKadDiscovery` / `kadBootstrapNodes` | unset | the service discovery tests only: `true` and the bootstrap's multiaddr |

> The cluster + shard must match across both nodes (they do — same config builder).
> A peer is reached via `getNodeInfo("Multiaddrs")` with the container IP
> substituted in (`helpers.node_multiaddr`), since the advertised address is `0.0.0.0`.

## Troubleshooting

Teardown saves every container's log to `$E2E_LOG_DIR` (default `/tmp`). CI uploads
them on failure as the `e2e-test-logs`, `e2e-tests-rln-logs` or `e2e-fleet-logs` artifact. Set
`LOGOSCTL_PY_FORWARD_OUTPUT=1` to also mirror CLI + daemon output into pytest logs.

Each xdist worker's docker network takes its own /24 of `172.31.0.0/16`. If another
network on the host already uses that range, `docker network create` fails with "Pool
overlaps". Set `E2E_NETWORK_SUBNET` to a free range with a /24 per worker, such as a /22
for `-n 4`. Docker picks the range of each fleet test's network.

## CI

`.github/workflows/ci.yml` runs the tests on four xdist workers in two jobs, both set up
by `.github/actions/setup-e2e`. `e2e-tests` runs the tests not marked `rln`.
`e2e-tests-rln` runs the `rln` tests except the `fleet` ones, and is not a required check.

`e2e-fleet.yml` runs the `fleet` tests nightly and on demand, only on master, and its
runs queue (see "Fleet"). Its membership comes from the repository secret
`RLN_KEYSTORE`, which `.github/actions/restore-rln-membership` unpacks into
`E2E_RLN_MEMBERSHIP_DIR`. After every renewal of the membership, replace the secret from
the logosctl config dir that holds the membership:

```bash
COPYFILE_DISABLE=1 tar czf rln-keystore.tar.gz --no-xattrs --exclude rln_keystore.lock \
  -C <config-dir>/data liblogos_rln_module
tar tzvf rln-keystore.tar.gz   # liblogos_rln_module/<instance>/rln_*: four files, no lock
base64 < rln-keystore.tar.gz > rln-keystore.b64
gh secret set RLN_KEYSTORE --repo logos-co/logos-delivery-module < rln-keystore.b64
rm rln-keystore.tar.gz rln-keystore.b64
```
