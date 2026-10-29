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
| `test_node_registers_membership` | 1 | a node's RLN module waits for funding, then registers a membership that becomes `active` (see "RLN") |
| `test_validating_node_receives_message_with_proof` | 2 | a message sent with an RLN proof reaches a validating node |
| `test_validating_service_relays_lightpush_with_proof` | 3 | a light push client's message with an RLN proof passes a validating light push server and reaches its relay peer |
| `test_lightpushed_message_reaches_filter_client` | 2 | an Edge sender's message reaches an Edge receiver through the `logos.test` fleet (see "Fleet") |
| `test_lightpushed_message_is_stored` | 1 | an Edge sender's message is in a fleet store node |
| `test_relayed_message_reaches_filter_client` | 2 | a Core sender's message reaches an Edge receiver through the fleet |
| `test_lightpushed_message_reaches_relay_node` | 2 | an Edge sender's message with an RLN proof passes a validating Core receiver through the fleet |
| `test_relay_node_advertises_through_libp2p_module` | 2 | another `libp2p_module` node finds a relay node's advert with its peer id, address, cluster, capabilities and shard (see "Service discovery") |

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

`messageReceived` carries the `contentTopic` (`arg1`) and the `vector<uint8_t>`
`payload` (`arg2`), which arrives as `bytes`; the fleet tests assert both.

`source` (`arg3`) is `"live"` or `"history"`. A node that has been down and
comes back can replay what it missed as `"history"`, so a test asserting on a
freshly published message should pin `arg3` to `"live"`.

## RLN

`test_rln.py` runs nodes with RLN on a local LEZ chain. Each xdist worker starts the
standalone LEZ sequencer in a container on its docker network and deploys an RLN
registry on it with logos-lez-rln's `run_setup` (`lez_registry` in `conftest.py`). Nodes
reach the sequencer through `LEZ_RLN_SEQUENCER`. RLN is turned on through a presets file
named by `LOGOS_DELIVERY_RLN_PRESETS`, whose `""` entry names the local registry, instead
of the `logos.test` preset, which names the testnet registry and always turns proof
validation off (see `docs/pages/rln.md`).

A node that sends with a proof registers a membership of its own: the fixture funds the
node's LEZ payer with logos-lez-rln's `fund_account`, and the RLN module registers by
itself. Funding waits until the chain's `CLOCK_50` has ticked, since the registry
refuses a registration before that and the module does not retry it.

A node on the LEZ testnet starts from `lez_wallet` and sends with `rln_membership`. Its
LEZ wallet syncs the testnet from the block after its last synced one, and RLN waits for
it: from block 0 that takes several minutes. So `lez_wallet` is one new wallet per
session: a daemon whose sequencer (`LEZ_RLN_SEQUENCER`) is unreachable creates it without
syncing, and its `last_synced_block` is then set to the testnet sequencer's
`getLastBlockId`. The wallet has no private notes to find in the blocks it skips. Its
config, which names the sequencer, is left out, so each node writes its own.
`rln_membership` is the testnet membership in `E2E_RLN_MEMBERSHIP_DIR`, a directory
holding `liblogos_rln_module`'s keystore folder (`liblogos_rln_module/<instance>/`).

## Fleet

`test_fleet.py`, marked `fleet`, sends through the live `logos.test` fleet. Each test
runs nodes on the `logos.test` preset (`helpers.make_fleet_config`: `mode` `Core` or
`Edge`, the preset's network and RLN, no NAT port mapping) and checks that what one
sends reaches a second node, started in parallel with it, or a fleet store node. 
An Edge receiver's `subscribe` returns before a fleet node holds its filter subscription,
so the receiver waits 3 s before the send. Each node is in its own docker network,
so the message has to cross the fleet. The sender holds the membership.
Every node validates proofs: its presets file has a `logos.test` entry
with `enable-validation` on, which the module's built-in entry leaves off.

Two proofs with the same `message_id` in one epoch disclose the membership's secret to
anyone who sees both, and every copy of the membership starts from the same counters.
So the fleet tests run one at a time, each sender starting from the keystore the
previous one left (`fleet_membership`); under xdist that takes `--dist=loadgroup`, and
`fleet_membership` fails without it. Across runs, `E2E_FLEET_EPOCH_FILE` names a file
holding the last epoch a run sent in: a run sends only in a later epoch, waiting if it
has to and printing how long, and writes the epoch when it ends. Without the file it
waits for the next epoch. CI keeps the file in the Actions cache and runs the fleet tests
only on master, since a branch's cache entries are invisible to master's runs; locally,
point it at a file of your own. Local and CI runs don't share it, so don't run the fleet
tests while another run from the same membership sends.

```bash
E2E_RLN_MEMBERSHIP_DIR=<membership dir> E2E_FLEET_EPOCH_FILE=~/.logos-e2e-local/fleet-epoch \
  pytest -v -m fleet
```

`storeQuery` takes an explicit peer, so the store test queries the preset's entry nodes
(`FLEET_ENTRY_NODES`) in turn.

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
- For `test_rln.py`: `liblogos_rln_module` and `liblogos_lez_rln_module` in
  `LOGOS_MODULES_DIR` (CI installs them next to `delivery_module`);
  `E2E_LEZ_SEQUENCER_IMAGE`, the standalone LEZ sequencer image, pulled; and
  `E2E_LEZ_RLN_DIR`, a logos-lez-rln checkout with its guest programs and its
  `run_setup`, `mint_payer` and `fund_account` built (see "Running locally — Linux").
  Without them the RLN tests skip.
- For the `fleet` tests: the testnet membership in `E2E_RLN_MEMBERSHIP_DIR` (see "RLN";
  without it they skip), outbound HTTPS to `testnet.lez.logos.co`, and outbound TCP to
  the `logos.test` fleet (port 30303).
- For `test_service_discovery.py`: `libp2p_module` in `LOGOS_MODULES_DIR`. Without it
  those tests skip.

If any of these is missing, all e2e tests **skip** with a clear reason.

## Running locally — Linux

The smoke image and the Python client come from the logos-logoscore-py commit that CI
pins as `LOGOSCORE_PY_REV` in `.github/actions/setup-e2e`. Set the same variable
before these steps.

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

# 3. For test_rln.py: the LEZ sequencer image, and logos-lez-rln's guest programs
#    (cargo-risczero, which builds them in docker) and tools, at the pins of ci.yml and
#    .github/actions/build-lez-rln.
docker pull harbor.status.im/logos-blockchain/logos-execution-zone/sequencer_service-standalone@sha256:b90dd1890b9b2d429a2793a69a1c98e0dcaf61db8e01a5cfd592eea0fa3c4688
export E2E_LEZ_SEQUENCER_IMAGE=harbor.status.im/logos-blockchain/logos-execution-zone/sequencer_service-standalone@sha256:b90dd1890b9b2d429a2793a69a1c98e0dcaf61db8e01a5cfd592eea0fa3c4688
curl -L https://risczero.com/install | bash
~/.risc0/bin/rzup install cargo-risczero 3.0.5 && ~/.risc0/bin/rzup install rust 1.88.0
git clone https://github.com/logos-co/logos-lez-rln.git /tmp/logos-lez-rln
cd /tmp/logos-lez-rln && git checkout 235ddaeee84eec385a2d4b68d69f702a29c14cf3
cargo risczero build --manifest-path lez-rln/methods/guest/Cargo.toml
cargo build --release --manifest-path lez-rln/Cargo.toml --bin mint_payer --bin run_setup --bin fund_account
export E2E_LEZ_RLN_DIR=/tmp/logos-lez-rln
cd -

# 4. Install deps + run.
cd tests/e2e
pip install -r requirements.txt
pip install --no-deps --force-reinstall \
  "logoscore @ git+https://github.com/logos-co/logos-logoscore-py.git@$LOGOSCORE_PY_REV"
pytest -v -m "not fleet" -n 4
```

The sequencer image is amd64 only; on Apple silicon Docker runs it emulated.

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
| `nat` | `none` | a container has no router to map ports on; the mapping opens UDP sockets on kernel-assigned ports, which can take QUIC's port (the TCP port by default) |
| `staticnodes` | `[A multiaddr]` (node B only) | direct dial of node A |
| `store` / `filter` | `false` | not exercised here |
| `lightpush` / `lightpushnode` | `false` / unset | the light push test only: `true` on its server, and on its client (`relay: false`) the server's multiaddr |
| `pluginKadDiscovery` / `kadBootstrapNodes` | unset | the service discovery tests only: `true` and the libp2p_module bootstrap's multiaddr |

> The cluster + shard must match across both nodes (they do — same config builder).
> A peer is reached via `getNodeInfo("Multiaddrs")` with the container IP
> substituted in (`helpers.node_multiaddr`), since the advertised address is `0.0.0.0`.

## Troubleshooting

Daemon container logs are saved to `$E2E_LOG_DIR` (default `/tmp`) on teardown, and
uploaded in CI on failure as the `e2e-test-logs` artifact (`e2e-rln-logs` for the RLN
job, the sequencer's among them; `e2e-fleet-logs` for the fleet one). Set
`LOGOSCTL_PY_FORWARD_OUTPUT=1` to also mirror CLI + daemon output into pytest logs.

Each xdist worker's docker network takes its own /24 of `172.31.0.0/16` (worker
`gw<n>` the n-th, a run without xdist the first). If another network on the host
already uses that range, `docker network create` fails with "Pool overlaps"; set
`E2E_NETWORK_SUBNET` to a free range with a /24 for each worker, such as a /22 for
`-n 4`. The fleet tests' networks take a range docker picks.

## CI

`.github/workflows/ci.yml` has an `e2e-tests` job (ubuntu-latest) that builds the logosctl
smoke image and copies the CLI out of it, installs `.#lgx-portable` and its released
dependencies with `logosctl install`, runs the tests without RLN
(`pytest -v -m "not rln" -n 4`), and uploads logs on failure. The e2e
jobs set up through the composite action `.github/actions/setup-e2e`. The two in `ci.yml`
run the tests on four xdist workers.

`e2e-tests` runs the tests not marked `rln`. The `e2e-rln` job runs the `rln` tests,
except the `fleet` ones; it is not a required check. It builds logos-lez-rln through
`.github/actions/build-lez-rln` (from the Actions cache when its pin is unchanged) and
pulls the sequencer image, pinned by digest in `ci.yml`.

The workflow `e2e-fleet.yml` runs the `fleet` tests nightly and on demand, on master
only, never on PRs, and its runs queue (see "Fleet"). It restores the testnet membership
through `.github/actions/restore-rln-membership`, which unpacks the repository secret
`RLN_KEYSTORE` into `E2E_RLN_MEMBERSHIP_DIR`. The secret is replaced after every renewal
from the logosctl config dir that holds the membership:

```bash
COPYFILE_DISABLE=1 tar czf rln-keystore.tar.gz --no-xattrs --exclude rln_keystore.lock \
  -C <config-dir>/data liblogos_rln_module
tar tzvf rln-keystore.tar.gz   # liblogos_rln_module/<instance>/rln_*: four files, no lock
base64 < rln-keystore.tar.gz > rln-keystore.b64
gh secret set RLN_KEYSTORE --repo logos-co/logos-delivery-module < rln-keystore.b64
rm rln-keystore.tar.gz rln-keystore.b64
```
