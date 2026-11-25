# Run a delivery node

Runs a delivery node (a Logos Core daemon + `delivery_module`, for Logos
Testnet v0.3). There is no GUI or HTTP API — interaction is via the daemon's
CLI, `logosctl`. You can run it three ways:

- [With Docker](#with-docker) — quickest; everything runs in a container.
- [Prebuilt binaries](#without-docker-prebuilt-binaries) — download release
  binaries, nothing to build (Linux and macOS).
- [Build with Nix](#without-docker-build-with-nix) — build this checkout from
  source (Linux and macOS).

The Docker and prebuilt paths run the released `delivery_module` 0.3.3 with
the catalog's RLN packages, `liblogos_rln_module` 0.12.0 and
`liblogos_lez_rln_module` 5.1.0. The Nix path builds this checkout, which
loads `rln_module`, `rln_lez_module` and `membership_allocation_client_module`
instead.

All three connect the node to the `logos.test` fleet by default. A node that
other peers should be able to dial also needs its
[public address](#public-address) set.

## With Docker

### Prerequisites

- Docker with Compose

### Start

```bash
git clone https://github.com/logos-co/logos-delivery-module.git
cd logos-delivery-module
docker compose up -d --build
```

The image is built from [logos-docker](https://github.com/logos-co/logos-docker):
the `logosctl` daemon with the released `delivery_module` and its RLN packages
from the Logos catalog.

### Boot the node

The daemon is running; load the module and create the node:

```bash
docker exec logos-node logosctl module load delivery_module
docker exec logos-node logosctl call delivery_module createNode @/conf/logos-test.json
```

`logos.test` runs RLN: the node starts without a membership, but cannot send
until it holds one — see [Fund the node's RLN membership](#fund-the-nodes-rln-membership).
Start it:

```bash
docker exec logos-node logosctl call delivery_module start
```

Verify:

```bash
docker exec logos-node logosctl daemon status
```

The daemon logs to the container's output: `docker logs logos-node`.

### Stop

```bash
docker compose down
```

Node data lives in the `logos-persistence` volume, so it survives `down` and
rebuilds. `docker compose down -v` removes it.

## Without Docker: prebuilt binaries

Run a node from released binaries — nothing to build, no repository clone. You
need one CLI, [`logosctl`](https://github.com/logos-co/logos-logoscore-cli)
0.3.0: the node daemon, its client, and the package manager in one binary.

### Install logosctl

Download the archive for your system from the
[logosctl 0.3.0 release](https://github.com/logos-co/logos-logoscore-cli/releases/tag/0.3.0)
and follow its install steps to put `logosctl` on your `PATH`.

### Install the module and boot the node

```bash
# Start the daemon; --detach returns once it accepts commands
logosctl daemon start --detach

# Fetch delivery_module and the RLN modules logos.test needs from the Logos
# catalog and install them. The RLN module names its registry provider without
# a version, so that one goes in first.
logosctl catalog refresh
logosctl package install delivery_module --version 0.3.3 --yes
logosctl install liblogos_lez_rln_module --version 5.1.0 -y
logosctl install liblogos_rln_module --version 0.12.0 --no-deps -y

# logos.test node config (layered createNode shape — see Configuration below)
cat > logos-test.json <<'JSON'
{
  "preset": "logos.test",
  "messagingOverrides": {
    "logLevel": "DEBUG",
    "tcp-port": 30303,
    "discv5-udp-port": 9000,
    "nat": "extip:<public-ip>"
  }
}
JSON

logosctl module load delivery_module
logosctl call delivery_module createNode @logos-test.json
logosctl call delivery_module start
```

Replace `<public-ip>` with the host's public IPv4 address, or drop the `nat`
line for a node nobody needs to dial — see [Public address](#public-address).

`logosctl` keeps everything — installed modules, logs, node data — in its
session directory, `~/.logosctl` (`--config-dir` picks another). The daemon
log is `~/.logosctl/logs/daemon.log`.

Verify with `logosctl daemon status`; stop with `logosctl daemon stop`.

## Fund the node's RLN membership

`logos.test` rate-limits with RLN on the LEZ testnet (see [`rln.md`](./rln.md)).
`createNode` brings the RLN modules up on the preset's registry; they create
the node's own wallet and register a membership as soon as its payer account
holds enough native LEZ — the node pays for itself, nothing is registered by
hand. The node starts without a membership; it cannot send until the
membership is active. A build of this checkout first asks a membership
allocator of the registry's network and pays only when none grants (see
[`rln.md`](./rln.md#how-a-membership-is-obtained)).

With Docker, run the commands below inside the container
(`docker exec -it logos-node sh`).

```bash
# The RLN modules' names: the released 0.3.3 packages (Docker, prebuilt binaries)
RLN=liblogos_rln_module LEZ=liblogos_lez_rln_module
# ... or a build of this checkout
RLN=rln_module LEZ=rln_lez_module

# The account to fund (state "ready", "network":"testnet")
logosctl call $LEZ wallet_status
# → {... "payer":"<ACCOUNT>" ...}
```

Send it **at least 200000000** native on the LEZ testnet: the registration
price is 1000000 (rate limit 100), and the transaction's fee reserve, sized
from the chain's live fees, is ~1.8e8 at base fee 8 (most of it is refunded).
Bridge funds from the Logos blockchain testnet with a channel deposit, or
transfer from an account that already holds LEZ testnet native.

Then wait for the membership (typically 1–3 minutes after the funds land):

```bash
printf '%s' '5e269b6a19fce081f5808b13442dcbc3522197638dd38df5a28bc4e55236b977' > /tmp/rln-id.arg
logosctl call $RLN get_membership_state \
  logos:testnet:fd322d32a46ddf57eece9db8d68e9b722262d1f770760fca63b8cb9e7775b971 @/tmp/rln-id.arg
# "state":"unknown" with provisioning.step "awaiting_funding" → not funded yet
# "state":"active"                                            → the node can send
```

The wallet (and the key to its funds) lives in the node's data — the
`logos-persistence` volume with Docker, the session directory otherwise.

## Without Docker: build with Nix

Build `logosctl` and this module with Nix, then run the daemon on the host.

### Prerequisites

- [Nix](https://nixos.org/download.html) with flakes enabled
- Linux or macOS

### Build the runtime and module

Build `logosctl` from its flake, then this module's `.lgx` and the modules it
loads. `libp2p_module` hosts [plugin discovery](#plugin-hosted-discovery) and
nothing else: a node that does not use it runs without it. An RLN-enabled
preset needs `rln_module` and what it depends on, `rln_lez_module` and
`membership_allocation_client_module`. This flake re-exports their `.lgx`s
from its own inputs, so they match the revs the module was built against:

```bash
git clone https://github.com/logos-co/logos-delivery-module.git
cd logos-delivery-module

# The CLI: daemon, client and package manager in one binary
nix build 'github:logos-co/logos-logoscore-cli#ctl' --out-link ./logos

# This module, built from the checkout
nix build '.#lgx' -o delivery-lgx

# libp2p, for plugin discovery
nix build '.#libp2p_module-lgx' -o libp2p-lgx

# The RLN chain: registry provider, allocator client, then the RLN module
nix build '.#rln_lez_module-lgx' -o rln-lez-lgx
nix build '.#membership_allocation_client_module-lgx' -o allocation-client-lgx
nix build '.#rln_module-lgx' -o rln-lgx

mkdir -p packages
cp -f delivery-lgx/*.lgx libp2p-lgx/*.lgx rln-lez-lgx/*.lgx \
  allocation-client-lgx/*.lgx rln-lgx/*.lgx packages/
```

The first build compiles the whole runtime stack through Nix — allow
~30–45 min. Later builds are fast.

`.#lgx` is a development package that only the Nix-built `logosctl` above can
load: its libraries find their own dependencies in the Nix store. On Linux
the libp2p package needs one more build to bring those into the store; the
[runtime doc-test](https://github.com/logos-co/logos-delivery-module/blob/master/doctests/outputs/delivery-module-runtime.md)
shows it. To install a local build into a released `logosctl` instead, build
`.#lgx-portable` and pass it to `logosctl package install`; such a build loads
`rln_module`, which the catalog does not carry yet, so there it runs presets
without RLN only.

### Start the daemon and install

```bash
export PATH="$PWD/logos/bin:$PATH"
logosctl daemon start --detach
logosctl install --dir packages -y
```

`--dir` installs every `.lgx` in the directory from disk, with no catalog
involved. As with the prebuilt binaries, everything lives in the session
directory, `~/.logosctl` unless `--config-dir` names another.

### Boot the node

```bash
logosctl module load delivery_module
logosctl call delivery_module createNode @conf/logos-test.json
logosctl call delivery_module start
```

The node can send once it holds an RLN membership — see
[Fund the node's RLN membership](#fund-the-nodes-rln-membership).

Verify with `logosctl daemon status`; stop with `logosctl daemon stop`.

> The [runtime doc-test](https://github.com/logos-co/logos-delivery-module/blob/master/doctests/outputs/delivery-module-runtime.md)
> walks through the same steps as an executable spec that CI runs, with the
> module dependencies pinned by commit, plus notes on the blocking Kademlia
> bootstrap in headless runs.

## Check the node

Once `start` returns, the node joins the network in the background. Within a
few minutes it should report connected peers:

```bash
logosctl call delivery_module getNodeInfo Metrics --json \
  | jq -r .result.value | grep '^libp2p_peers '
# libp2p_peers 8.0
```

and advertise its public address in its ENR and listen addresses:

```bash
logosctl call delivery_module getNodeInfo Multiaddrs --json | jq -r .result.value
# /ip4/<public-ip>/tcp/30303/p2p/16Uiu2…,/ip4/<public-ip>/udp/30303/quic-v1/p2p/16Uiu2…
```

With Docker, prefix them with `docker exec logos-node`. See
[`query-node.md`](./query-node.md) for everything else the node reports.

## Configuration

The config uses the layered `createNode` shape: `preset` picks the network,
`mode` defaults to `"Core"` (`"Edge"` for a light node), per-layer settings
go in `messagingOverrides`. The repo ships it as
[`conf/logos-test.json`](../../conf/logos-test.json): Docker mounts it into the
container at `/conf` (`@/conf/logos-test.json`); with the Nix build, pass the
path directly (`@conf/logos-test.json`). The prebuilt-binaries path above
writes the same config inline, plus `nat`. Edit it and re-run the boot steps
to change settings.

Keep extra keys inside `messagingOverrides` / `channelsOverrides` /
`kernelConf` — a bare top-level key (even `logLevel`) switches parsing to the
legacy flat shape. Unpinned listening ports are OS-assigned; the config pins
the p2p ports to match the Docker port mappings.

For the dev network, use [`conf/logos-dev.json`](../../conf/logos-dev.json)
(preset `logos.dev`) — see
[`networks.md`](./networks.md) for how the two differ. The full config
grammar, including kernel-only nodes (`"entryLayer": "kernel"`), is
documented in the [API reference](api_reference.rst).

The node's local state goes to the runtime's per-instance persistence
directory unless `localStoragePath` names another.

### Public address

The node advertises only addresses peers can actually dial. It no longer
publishes `0.0.0.0` or a placeholder port: until it learns its public address,
its ENR carries no IP at all and other nodes cannot connect to it.

For a node others should reach, set the address explicitly:

```json
"messagingOverrides": {
  "nat": "extip:<public-ip>"
}
```

`nat` also takes `"any"` (the default: discover a UPnP or NAT-PMP gateway),
`"upnp"`, `"pmp"` or `"none"`. Inside Docker, use `extip`. Open the
p2p port (`tcp-port`, 30303) for both TCP and UDP (QUIC, below) and the UDP
discovery port (`discv5-udp-port`, 9000) on the host firewall.

### QUIC

QUIC is on by default and runs next to TCP. Unless `quic-port` names another
port, it listens on UDP at the TCP port number — `/udp/30303/quic-v1` for the
config above. [`docker-compose.yml`](../../docker-compose.yml) already
publishes it.

On Linux, raise the socket receive buffer limit, or the node logs
`QUIC UDP receive buffer capped below requested size` at every start:

```bash
sudo sysctl -w net.core.rmem_max=8388608
```

### RLN

RLN comes from the preset — there is nothing to configure. `logos.test` runs
with it on (the LEZ testnet registry), so a `logos.test` node needs the RLN
modules and an active RLN membership; see [`rln.md`](./rln.md#turning-rln-on).
`logos.dev` runs with it off, so a `logos.dev` node needs no RLN modules;
`module load` reports the RLN module as skipped and carries on.
[`networks.md`](./networks.md) tracks each network's RLN state.

On a preset with RLN on, the RLN modules have to be installed next to this
one before `createNode`, or bring-up ends in `Failed`. Each path above
installs them; the Docker image takes their version from the `RLN_VERSION`
build arg in [`docker-compose.yml`](../../docker-compose.yml).

Follow bring-up with `rlnState`: `Disabled` → `Initializing` → `Ready` |
`Failed`. RLN comes up once, with the node, and `start` and `stop` do not
touch it. See [`rln.md`](./rln.md#turning-rln-on) for the details, and for
when to call `start` on a node with Mix on.

### Plugin-hosted discovery

Enable it with `"pluginKadDiscovery": true` in `messagingOverrides` (or in
`kernelConf` for a kernel-only node); leave it out to keep the node's internal
discovery. On top of [`conf/logos-dev.json`](../../conf/logos-dev.json) the
switch is all it takes:

```json
{
  "preset": "logos.dev",
  "messagingOverrides": { "pluginKadDiscovery": true }
}
```

Extra DHT bootstrap peers go through the node's own `kad-bootstrap-node` key.

It needs `libp2p_module` installed next to this module (see the Nix build
above). Without it `start` fails and the node is stopped. On Windows, where
`libp2p_module` has no build, use internal discovery.

`LIBP2P_MODULE_CONFIG` (inline JSON or a file path) is optional. Set it only to
override libp2p's own defaults, for example its listen addresses when other
nodes must reach it: unset, it listens on an ephemeral loopback port. With
plugin discovery the node is created with `mountKad` and
`mountServiceDiscovery` on and the node's DHT bootstrap peers in place of the
config's `bootstrapNodes`. Without it this module creates no libp2p node. The
RLN stack does not use `libp2p_module` or this variable: its allocation client
runs a node of its own.

How it works is described in [Architecture](./architecture.md#plugin-hosted-discovery).

## Metrics

The node already aggregates Prometheus metrics internally (the same set exposed
on `metricsServerPort`, rendered behind the `"Metrics"` node-info attribute).
`collectOpenMetricsText()` hands that exposition text back **verbatim** so the
[`openmetrics`](https://github.com/logos-co/openmetrics-module) module can scrape
this module without standing up a separate HTTP server — no in-module parsing or
reshaping. The openmetrics scraper parses the text, injects a
`module="delivery_module"` label on every series, and merges it with other
modules.

Point the `openmetrics` module at this one by name, selecting the text-source
convention with `"format": "text"`:

```bash
logoscore --config-dir /tmp/om call openmetrics start \
  '{"port":9090,"modules":[{"name":"delivery_module","format":"text"}]}'
curl http://localhost:9090/metrics   # every series carries module="delivery_module"
```

Before a node is created (or if the read fails) `collectOpenMetricsText()`
returns an empty document so a scrape never errors out on this module.

For a one-off read without the `openmetrics` module, the raw exposition text is
also available via `getNodeInfo Metrics` — see [`query-node.md`](./query-node.md).
