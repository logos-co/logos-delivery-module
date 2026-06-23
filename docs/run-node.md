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

### Get the tools onto your `PATH`

The release tags below are the current pre-releases — bump them to the newest
from each repo's releases page if needed.

**Linux** (each tarball holds one AppImage; AppImages need FUSE — in a
container/headless host without it, `export APPIMAGE_EXTRACT_AND_RUN=1`):

```bash
arch=x86_64        # or: aarch64
curl -fsSL "https://github.com/logos-co/logos-logoscore-cli/releases/download/pre-release-8002477-4/logoscore-${arch}-linux.tar.gz" | tar xz
curl -fsSL "https://github.com/logos-co/logos-package-downloader/releases/download/pre-release-99d70db-7/lgpd-${arch}-linux.tar.gz" | tar xz
curl -fsSL "https://github.com/logos-co/logos-package-manager/releases/download/pre-release-05b2cf8-7/lgpm-${arch}-linux.tar.gz" | tar xz
mkdir -p bin
for t in logoscore lgpd lgpm; do chmod +x "${t}-${arch}.AppImage"; ln -sf "$PWD/${t}-${arch}.AppImage" "bin/$t"; done
export PATH="$PWD/bin:$PATH"
```

**macOS (Apple Silicon):**

```bash
curl -fsSL "https://github.com/logos-co/logos-logoscore-cli/releases/download/pre-release-8002477-4/logoscore-aarch64-macos.tar.gz" | tar xz
curl -fsSL "https://github.com/logos-co/logos-package-downloader/releases/download/pre-release-99d70db-7/lgpd-aarch64-macos.tar.gz" | tar xz
curl -fsSL "https://github.com/logos-co/logos-package-manager/releases/download/pre-release-05b2cf8-7/lgpm-aarch64-macos.tar.gz" | tar xz
export PATH="$PWD/logoscore-aarch64-macos/bin:$PWD/lgpd-aarch64-macos/bin:$PWD/lgpm-aarch64-macos/bin:$PATH"
```

### Download the module and boot the node

```bash
# Fetch delivery_module from the Logos catalog, then install it into ./modules
lgpd download delivery_module --output ./packages
mkdir -p modules
lgpm install --dir ./packages --modules-dir ./modules

# logos.test node config
cat > logos-test.json <<'JSON'
{ "preset": "logos.test", "logLevel": "DEBUG" }
JSON

# Run the daemon (it binds capability_module automatically, so ./modules only
# needs delivery_module), then boot the node
logoscore -D -m ./modules > logs.txt &
logoscore load-module delivery_module
logoscore call delivery_module createNode @logos-test.json
logoscore call delivery_module start
```

Verify with `logoscore status`; stop with `logoscore stop`.

> `lgpd download delivery_module` pulls the newest version from the Logos
> catalog — `lgpd`'s built-in repository is
> [`logos-modules-release`](https://github.com/logos-co/logos-modules-release).
> The `logos.test` preset needs `delivery_module` **≥ 0.1.3**; earlier builds
> only know `twn` and `logos.dev`. (You can also grab a specific `.lgx`
> straight from the
> [release page](https://github.com/logos-co/logos-modules-release/releases)
> and skip `lgpd` — `lgpm install --file <path>` then takes it.)

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
