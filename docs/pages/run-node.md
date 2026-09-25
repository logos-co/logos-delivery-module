# Run a delivery node

Runs a delivery node (a Logos Core daemon + `delivery_module` 0.3.0, for Logos
Testnet v0.3). There is no GUI or HTTP API — interaction is via the daemon's
CLI (`logosctl`, or `logoscore` in the Docker and Nix setups). You can run it
three ways:

- [With Docker](#with-docker) — quickest; everything runs in a container.
- [Prebuilt binaries](#without-docker-prebuilt-binaries) — download release
  binaries, nothing to build (Linux and macOS).
- [Build with Nix](#without-docker-build-with-nix) — build from source on any
  platform.

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

The image is built from [logos-docker](https://github.com/logos-co/logos-docker)
with `delivery_module` 0.3.0 from the Logos catalog. The first build runs Nix —
allow ~30–45 min. Later starts are fast.

### Boot the node

The daemon is running; load the module and start the node:

```bash
docker exec logos-node logoscore --config-dir /var/lib/logos/config load-module delivery_module --json
docker exec logos-node logoscore --config-dir /var/lib/logos/config call delivery_module createNode @/conf/logos-test.json --json
docker exec logos-node logoscore --config-dir /var/lib/logos/config call delivery_module start --json
```

Verify:

```bash
docker exec logos-node logoscore --config-dir /var/lib/logos/config status --json
```

### Stop

```bash
docker compose down
```

Node data lives in the `logos-persistence` volume, so it survives `down` and
rebuilds. `docker compose down -v` removes it.

## Without Docker: prebuilt binaries

Run a node from released binaries — nothing to build, no repository clone. You
need one CLI, [`logosctl`](https://github.com/logos-co/logos-logoscore-cli)
0.3.0: the node daemon, its client, and the package manager in one binary. It
is published for Linux (`x86_64` / `aarch64`) and macOS (Apple Silicon /
`aarch64`).

### Install logosctl

This downloads `logosctl` for your OS/arch into `./bin` (the script pins a
release — bump `LOGOSCTL_TAG` in it to move to a newer one):

```bash
curl -fsSL https://raw.githubusercontent.com/logos-co/logos-delivery-module/master/scripts/install-node-tools.sh | sh
export PATH="$PWD/bin:$PATH"
```

### Install the module and boot the node

```bash
# Start the daemon; --detach returns once it accepts commands
logosctl daemon start --detach

# Fetch delivery_module from the Logos catalog and install it
logosctl catalog refresh
logosctl package install delivery_module --version 0.3.0 --yes

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

## Without Docker: build with Nix

Build the runtime and this module with Nix, then run the `logoscore` daemon
directly on the host.

### Prerequisites

- [Nix](https://nixos.org/download.html) with flakes enabled
- Linux or macOS

### Build the runtime and module

Build the `logoscore` CLI (the headless runtime) and the `lgpm` package
manager from their flakes, then build and install this module's `.lgx`.
Both of its dependencies are optional. `libp2p_module` hosts external service
discovery: a node configured for it (`plugin-kad-discovery`) fails to start
without it, any other node runs without it. An RLN-enabled preset also needs
`liblogos_rln_module` and `liblogos_lez_rln_module`. This flake
re-exports their `.lgx`s from its own locked inputs, so they match the revs
the module was built against:

```bash
git clone https://github.com/logos-co/logos-delivery-module.git
cd logos-delivery-module
git checkout v0.3.0

# Runtime + package manager
nix build 'github:logos-co/logos-logoscore-cli' --out-link ./logos
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm

# This module, built from the checkout
nix build '.#lgx' -o delivery-lgx

# libp2p, for external service discovery
nix build '.#libp2p_module-lgx' -o libp2p-lgx

# Its RLN dependency chain
nix build '.#liblogos_rln_module-lgx' -o rln-lgx
nix build '.#liblogos_lez_rln_module-lgx' -o lez-rln-lgx

# Seed the modules dir with the bundled capability module, then install
mkdir -p modules
cp -RL ./logos/modules/. ./modules/
for pkg in libp2p-lgx lez-rln-lgx rln-lgx delivery-lgx; do
  ./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file "$pkg"/*.lgx
done
```

The first build compiles the whole runtime stack through Nix — allow
~30–45 min. Later builds are fast.

`.#lgx` is a development package that only the Nix-built runtime above can
load. To install a local build into a released `logosctl` instead, build
`.#lgx-portable` and pass it to `logosctl package install`.

### Start the daemon

Put `logoscore` on your `PATH` and start it in daemon mode pointed at
`./modules`:

```bash
export PATH="$PWD/logos/bin:$PATH"
logoscore -D -m ./modules > logs.txt &
```

### Boot the node

```bash
logoscore load-module delivery_module
logoscore call delivery_module createNode @conf/logos-test.json
logoscore call delivery_module start
```

Verify:

```bash
logoscore status
```

### Stop

```bash
logoscore stop
```

> For a fully pinned, build-from-this-commit walkthrough — plus notes on the
> blocking Kademlia bootstrap in headless runs — see the
> [runtime doc-test](https://github.com/logos-co/logos-delivery-module/blob/master/doctests/outputs/delivery-module-runtime.md).

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
logosctl call delivery_module getNodeInfo MyMultiaddresses --json | jq -r .result.value
# /ip4/<public-ip>/tcp/30303/p2p/16Uiu2…
```

With Docker, run the same calls through
`docker exec logos-node logoscore --config-dir /var/lib/logos/config call …`;
with the Nix build, through `logoscore call …`. See
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
(preset `logos.dev`, with discovery hosted on `libp2p_module`) — see
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
TCP p2p port (`tcp-port`, 30303) and the UDP discovery port (`discv5-udp-port`,
9000) on the host firewall.

### QUIC

QUIC runs next to TCP on its own UDP port. It is off by default; turn it on
with:

```json
"messagingOverrides": {
  "quic-support": true,
  "quic-port": 60000
}
```

Pin `quic-port`: unset, it is OS-assigned. Open that UDP port, and with
Docker, add `"60000:60000/udp"` to the `ports` in
[`docker-compose.yml`](../../docker-compose.yml). The node then listens on, and
advertises, `/udp/60000/quic-v1` as well as TCP.

Enable it only where the UDP port is reachable: peers dial QUIC before TCP,
and a blocked QUIC port costs each of them a 10 s timeout before falling back.

On Linux, raise the socket receive buffer limit, or the node logs
`QUIC UDP receive buffer capped below requested size` at every start:

```bash
sudo sysctl -w net.core.rmem_max=8388608
```

### RLN

RLN comes from the preset — there is nothing to configure. Both
`logos.test` and `logos.dev` currently run with it off, so a node needs no
RLN modules today; `module load` reports `liblogos_rln_module` as skipped and
carries on. [`networks.md`](./networks.md) tracks each network's RLN state.

On a preset with RLN on, install the RLN modules next to this one before
`createNode`, or bring-up ends in `Failed`:

- Prebuilt binaries — they are in a separate catalog:

  ```bash
  logosctl catalog add https://github.com/logos-co/logos-rln-modules/releases/download/index/logos-repo.json
  logosctl catalog refresh
  logosctl package install liblogos_rln_module --yes
  ```

- Nix — the build above already installs them.
- Docker — set the `RLN_VERSION`, `LEZ_RLN_VERSION` and `LEZ_CORE_VERSION`
  build args of the [logos-docker](https://github.com/logos-co/logos-docker)
  image in [`docker-compose.yml`](../../docker-compose.yml).

Follow bring-up with `rlnState`: `Disabled` → `Initializing` → `Ready` |
`Failed`. See [`rln.md`](./rln.md) for the details.

### Plugin-hosted discovery

Enable it with `"pluginKadDiscovery": true` in `messagingOverrides` (or in
`kernelConf` for a kernel-only node); leave it out to keep the node's internal
discovery. [`conf/logos-dev.json`](../../conf/logos-dev.json) needs nothing
but the switch. Extra DHT bootstrap peers go through the node's own
`kad-bootstrap-node` key.

It needs `libp2p_module` installed next to this module (see the Nix build
above). Without it `start` fails and the node is stopped. On Windows, where
`libp2p_module` has no build, use internal discovery.

`LIBP2P_MODULE_CONFIG` (inline JSON or a file path) is optional. Set it only to
override libp2p's own defaults, for example its listen addresses when other
nodes must reach it: unset, it listens on an ephemeral loopback port.

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
