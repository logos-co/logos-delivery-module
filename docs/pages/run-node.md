# Run a delivery node

Runs a delivery node (`logoscore` daemon + `delivery_module`). There is no GUI
or HTTP API — interaction is via the `logoscore` CLI. You can run it three ways:

- [With Docker](#with-docker) — quickest; everything runs in a container.
- [Prebuilt binaries](#without-docker-prebuilt-binaries) — download release
  binaries, nothing to build (Linux and macOS).
- [Build with Nix](#without-docker-build-with-nix) — build from source on any
  platform.

All three connect the node to the `logos.test` fleet by default.

## With Docker

### Prerequisites

- Docker with Compose

### Start

```bash
git clone https://github.com/logos-co/logos-delivery-module.git
cd logos-delivery-module
docker compose up -d --build
```

First build runs Nix and downloads release packages — allow ~30–45 min.
Later starts are fast.

### Boot the node

The daemon is running; load the module and start the node:

```bash
docker exec logos-node logoscore load-module delivery_module --json
docker exec logos-node logoscore call delivery_module createNode @/conf/logos-test.json --json
docker exec logos-node logoscore call delivery_module start --json
```

Verify:

```bash
docker exec logos-node logoscore status --json
```

### Stop

```bash
docker compose down
```

## Without Docker: prebuilt binaries

Run a node from released binaries — nothing to build, no repository clone. You
need three CLIs from the Logos releases:

- **`logoscore`** — the node daemon ([logos-logoscore-cli](https://github.com/logos-co/logos-logoscore-cli))
- **`lgpd`** — package downloader, fetches modules from the Logos catalog
  ([logos-package-downloader](https://github.com/logos-co/logos-package-downloader))
- **`lgpm`** — package manager, installs them locally
  ([logos-package-manager](https://github.com/logos-co/logos-package-manager))

All three are published for Linux (`x86_64` / `aarch64`) and macOS (Apple
Silicon / `aarch64`).

### Install the tools

This downloads `logoscore`, `lgpd`, and `lgpm` for your OS/arch into `./bin`
(the script pins a known-good release of each — bump the `*_TAG` values in it to
move to newer builds):

```bash
curl -fsSL https://raw.githubusercontent.com/logos-co/logos-delivery-module/master/scripts/install-node-tools.sh | sh
export PATH="$PWD/bin:$PATH"
```

### Download the module and boot the node

```bash
# Fetch delivery_module from the Logos catalog, then install it into ./modules
mkdir -p packages modules
lgpd download delivery_module --output ./packages
lgpm install --dir ./packages --modules-dir ./modules

# logos.test node config (layered createNode shape — see Configuration below)
cat > logos-test.json <<'JSON'
{
  "preset": "logos.test",
  "messagingOverrides": {
    "logLevel": "DEBUG",
    "tcp-port": 30303,
    "discv5-udp-port": 9000
  }
}
JSON

# Run the daemon (it binds capability_module automatically, so ./modules only
# needs delivery_module), then boot the node
logoscore -D -m ./modules > logs.txt &
logoscore load-module delivery_module
logoscore call delivery_module createNode @logos-test.json
logoscore call delivery_module start
```

Verify with `logoscore status`; stop with `logoscore stop`.

## Without Docker: build with Nix

Build the runtime and this module with Nix, then run the `logoscore` daemon
directly on the host.

### Prerequisites

- [Nix](https://nixos.org/download.html) with flakes enabled
- Linux or macOS

### Build the runtime and module

Build the `logoscore` CLI (the headless runtime) and the `lgpm` package
manager from their flakes, then build and install this module's `.lgx`:

```bash
git clone https://github.com/logos-co/logos-delivery-module.git
cd logos-delivery-module

# Runtime + package manager
nix build 'github:logos-co/logos-logoscore-cli' --out-link ./logos
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm

# This module, built from the current checkout
nix build '.#lgx' -o delivery-lgx

# Seed the modules dir with the bundled capability module, then install
mkdir -p modules
cp -RL ./logos/modules/. ./modules/
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file delivery-lgx/*.lgx
```

The first build compiles the whole runtime stack through Nix — allow
~30–45 min. Later builds are fast.

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

## Configuration

The config uses the layered `createNode` shape: `preset` picks the network,
`mode` defaults to `"Core"` (`"Edge"` for a light node), per-layer settings
go in `messagingOverrides`. The repo ships it as
[`conf/logos-test.json`](../../conf/logos-test.json): Docker mounts it into the
container at `/conf` (`@/conf/logos-test.json`); with the Nix build, pass the
path directly (`@conf/logos-test.json`). The prebuilt-binaries path above
writes the same config inline. Edit it and re-run the boot steps to change
settings.

Keep extra keys inside `messagingOverrides` / `channelsOverrides` /
`kernelConf` — a bare top-level key (even `logLevel`) switches parsing to the
legacy flat shape. Unpinned listening ports are OS-assigned; the config pins
the p2p ports to match the Docker port mappings.

For the dev network, use [`conf/logos-dev.json`](../../conf/logos-dev.json)
(preset `logos.dev`, with discovery hosted on `libp2p_module`) — see
[`networks.md`](./networks.md) for how the two differ. The full config
grammar, including kernel-only nodes (`"entryLayer": "kernel"`), is
documented in the [API reference](api_reference.rst).

The node is now connected to the `logos.test` network. See
[`query-node.md`](./query-node.md) to read its peer ID, ENR, and metrics.

### Plugin-hosted discovery

With `"pluginKadDiscovery": true` (in `messagingOverrides`, or in `kernelConf`
for a kernel-only node) logos-delivery delegates kademlia service discovery to
this module, which hosts it on `libp2p_module`. The config is logos-delivery's
alone and is forwarded as is. After `createNode` this module asks the node
(`logosdelivery_get_discovery_requirements`) whether a plugin is expected and
which DHT bootstrap peers its configuration resolved, presets included, and
sets `libp2p_module` up from that answer: the node's peers as `bootstrapNodes`,
`mountKad` and `mountServiceDiscovery` on. Explicit peers go through the
node's own key (`kad-bootstrap-node`, `/p2p/` multiaddrs);
[`conf/logos-dev.json`](../../conf/logos-dev.json) needs nothing but the switch.

`libp2p_module`'s remaining options (listen addresses, transport, key) come
from its own channel, the `LIBP2P_MODULE_CONFIG` environment variable (inline
JSON or a file path); the plugin overlays the node's answer on it rather than
replacing it. Unset, libp2p listens on an ephemeral loopback port, which is
fine for local runs and not reachable from outside.

Only the first bootstrap peer is handed over: `libp2p_module` dials the set
inside a fixed 10 s call budget, and two DNS-resolved peers exceed it. The
plugin brings libp2p up on the first discovery call after `start`, so a bad
bootstrap set surfaces there, not at `createNode`. Set `LD_DISCO_TRACE` to a
file path to log every call across the plugin boundary; logos-core discards a
module's stderr, so this file is the only view into it.

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
