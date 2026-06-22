# Run a delivery node

Runs a delivery node (`logoscore` daemon + `delivery_module`). There is no GUI
or HTTP API — interaction is via the `logoscore` CLI. You can run it two ways:

- [With Docker](#with-docker) — quickest; everything runs in a container.
- [Without Docker](#without-docker) — build and run natively with Nix.

Both connect the node to the `logos.test` fleet by default.

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

## Without Docker

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
> [runtime doc-test](../doctests/outputs/delivery-module-runtime.md).

## Configuration

The node config is [`conf/logos-test.json`](../conf/logos-test.json); it uses
the `logos.test` network preset. With Docker it is mounted into the container
at `/conf` (`@/conf/logos-test.json`); without Docker, pass the path directly
(`@conf/logos-test.json`). Edit it and re-run the boot steps to change
settings.

To target the dev network instead, use
[`conf/logos-dev.json`](../conf/logos-dev.json) (preset `logos.dev`). Available
keys are documented in the
[README](../README.md#node-configuration-createnode).

The node is now connected to the `logos.test` network. See
[`query-node.md`](./query-node.md) to read its peer ID, ENR, and metrics.
