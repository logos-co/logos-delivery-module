# Run a delivery node

Runs a delivery node (`logoscore` daemon + `delivery_module`). There is no GUI
or HTTP API — interaction is via the `logoscore` CLI. You can run it three ways:

- [With Docker](#with-docker) — quickest; everything runs in a container.
- [Prebuilt binaries](#without-docker-prebuilt-binaries) — download release
  binaries, nothing to build (Linux only).
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

Download the `logoscore` daemon, the `lgpm` package manager, and this module's
prebuilt `.lgx` from their GitHub releases — nothing to build, no repository
clone required. The module package ([`logos-modules`](https://github.com/logos-co/logos-modules)
releases) bundles every Logos module, including `delivery_module`.

> Published only for Linux (`x86_64` / `aarch64`). On macOS, use Docker or the
> Nix build below — the `logoscore` daemon is released only for Linux.

```bash
arch=x86_64        # or: aarch64

# logoscore daemon (AppImage) — latest release
curl -L -o logoscore \
  "https://github.com/logos-co/logos-logoscore-cli/releases/latest/download/logoscore-${arch}-linux.AppImage"
chmod +x logoscore

# lgpm package manager (an AppImage inside the tarball)
curl -L "https://github.com/logos-co/logos-package-manager/releases/download/pre-release-05b2cf8-7/lgpm-${arch}-linux.tar.gz" | tar xz
chmod +x "lgpm-${arch}.AppImage"

# This module, prebuilt — latest logos-modules release
curl -L -o delivery_module.lgx \
  "https://github.com/logos-co/logos-modules/releases/latest/download/delivery_module.lgx"

# Install the module into ./modules
mkdir -p modules
"./lgpm-${arch}.AppImage" --modules-dir ./modules --allow-unsigned install --file delivery_module.lgx
```

Write the node config and boot the node (the daemon binds `capability_module`
automatically, so the `./modules` dir only needs `delivery_module`):

```bash
cat > logos-test.json <<'JSON'
{ "preset": "logos.test", "logLevel": "DEBUG" }
JSON

./logoscore -D -m ./modules > logs.txt &
./logoscore load-module delivery_module
./logoscore call delivery_module createNode @logos-test.json
./logoscore call delivery_module start
```

Verify with `./logoscore status`; stop with `./logoscore stop`.

> `lgpm` has no stable release yet, so its URL is pinned to a specific
> pre-release tag — bump it to the newest from the
> [package-manager releases](https://github.com/logos-co/logos-package-manager/releases)
> if needed.

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
> [runtime doc-test](../doctests/outputs/delivery-module-runtime.md).

## Configuration

The node config is just the `logos.test` network preset. The repo ships it as
[`conf/logos-test.json`](../conf/logos-test.json): with Docker it is mounted
into the container at `/conf` (`@/conf/logos-test.json`); with the Nix build,
pass the path directly (`@conf/logos-test.json`). The prebuilt-binaries path
above writes the same config inline as `logos-test.json` so no clone is needed.
Edit it and re-run the boot steps to change settings.

To target the dev network instead, use
[`conf/logos-dev.json`](../conf/logos-dev.json) (preset `logos.dev`). Available
keys are documented in the
[README](../README.md#node-configuration-createnode).

The node is now connected to the `logos.test` network. See
[`query-node.md`](./query-node.md) to read its peer ID, ENR, and metrics.
