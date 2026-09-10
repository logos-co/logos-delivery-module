# Running This Delivery Module Against logoscore

`logos-delivery-module` is a Logos `core` module that wraps
[liblogosdelivery](https://github.com/logos-messaging/logos-delivery) to provide
high-level message-delivery capabilities. This doc-test exercises **this**
delivery-module commit end-to-end through the headless `logoscore` runtime:

1. Build the `logoscore` CLI and the `lgpm` local package manager from their
   published flakes. `logoscore` is the headless frontend for `logos-liblogos`,
   so building it brings in the whole module-runtime stack (`logos_host`,
   `liblogos_core`, the IPC layer).
2. Build **this** delivery module as an installable `.lgx` package straight from
   its own flake's `#lgx` output, **pinned to the commit under test** — so the
   module you run is built from exactly what is checked out here, not the latest
   published release.
3. Install the `.lgx` into a `./modules` directory with `lgpm`, together with
   the RLN modules `delivery_module` depends on.
4. Start `logoscore` in daemon mode (`-D`), load `delivery_module`, introspect
   it with `module-info`, call `createNode` with a Waku node config, then call
   `start` — verifying the module actually runs and boots a delivery node.

Because the module is built from the commit under test and then loaded and called
through a real `logoscore` daemon, a green run is real evidence that this change
keeps the delivery module loadable and callable.

**What you'll build:** This `delivery_module`, packaged as `.lgx`, installed with `lgpm`, and called through a `logoscore` daemon.

**What you'll learn:**

- How to build the `logoscore` runtime and the `lgpm` package manager from their flakes
- How a module's flake exposes a ready-to-install `.lgx` via its `#lgx` output
- How to install an `.lgx` into a modules directory with `lgpm`
- How a module's declared dependencies have to be installed alongside it
- How to start the `logoscore` daemon, load a module, introspect it, and call its methods
- How to create and start a delivery node with `createNode` and `start`
- How to shut the daemon down and confirm it has exited

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- **A Linux or macOS machine.**

---

## Step 1: Build logoscore

Build the `logoscore` CLI from its published flake. The result is symlinked to
`./logos/`. `logoscore` is the headless frontend for `logos-liblogos`, so this
one build brings in the whole module-runtime stack the daemon needs.

### 1.1 Build the CLI

```bash
nix build 'github:logos-co/logos-logoscore-cli' --out-link ./logos
```

The build produces `logos/bin/logoscore` plus bundled runtime libraries
and a `logos/modules/` directory containing the built-in
`capability_module` (required for the auth handshake when loading
modules).

---

## Step 2: Build the lgpm package manager

`lgpm` installs `.lgx` packages into a modules directory and scans what is
installed. Build it from the `logos-package-manager` flake and link it as
`./lgpm`.

### 2.1 Build lgpm

```bash
nix build 'github:logos-co/logos-package-manager#cli' -o lgpm
```

The executable is at `./lgpm/bin/lgpm`.

---

## Step 3: Build and install this delivery module

Build **this** delivery module's `.lgx` straight from its flake's `#lgx`
output and install it into a local `./modules` directory with `lgpm`, along
with the RLN modules it depends on. Every module built with
[`logos-module-builder`](https://github.com/logos-co/logos-module-builder)
exposes a ready-to-install `#lgx`.

> The `` in the URL is what pins the build to a specific commit: the
> doc-test runner expands it to a concrete ref. Locally that is this
> checkout's `HEAD` (see `run.sh`); in CI it is the commit being tested. With
> no pin it falls back to the latest `master`.

### 3.1 Build the module's .lgx

Build the `#lgx` output and link it as `./delivery-lgx`. (This compiles
the module and its SDK dependencies through Nix, so the first build is
slow.)

```bash
# From inside the clone this is simply: nix build '.#lgx'
nix build 'github:logos-co/logos-delivery-module#lgx' -o delivery-lgx
```

The `.lgx` package is now under `./delivery-lgx/`:

```bash
ls delivery-lgx/*.lgx
```

### 3.2 Build the RLN dependency chain

`delivery_module` declares `liblogos_rln_module` in
`metadata.json#dependencies`, and the host refuses to load a module
whose dependency chain is absent from the modules directory — so that
chain has to be installed alongside it: `liblogos_rln_module` →
`liblogos_lez_rln_module` → `lez_core`.

Each is built at the rev this module's `flake.lock` pins for it, so
the RLN modules you install are the ones the delivery module was
built against. From a clone, this flake re-exports the same three
packages — `nix build '.#liblogos_rln_module-lgx'`, and likewise for
`liblogos_lez_rln_module` and `lez_core` — which reads the pins
straight out of the lock.

```bash
nix build 'git+https://github.com/logos-co/logos-rln-modules?ref=feat/lip-alignment&rev=c89c7691d06af32c002426f3c6f6dece79dbffaa&dir=logos-rln-module#lgx' -o rln-lgx
nix build 'git+https://github.com/logos-co/logos-rln-modules?ref=fix/module-dep-chain-resolution&rev=c56869db0d477212d8c3937c8fd85fcb6f27909b&dir=logos-lez-rln-module#lgx' -o lez-rln-lgx
nix build 'github:logos-blockchain/logos-execution-zone-module/0ea57f8a1c57539d6ee0961a9cd27b064685b9e8#lgx' -o lez-core-lgx

```

Three more `.lgx` packages, one per module in the chain:

```bash
ls rln-lgx/*.lgx lez-rln-lgx/*.lgx lez-core-lgx/*.lgx
```

### 3.3 Seed the modules directory with the bundled capability module

`delivery_module` is loaded through the host's capability layer, so the
modules directory also needs the `capability_module` that ships with
`logoscore`. Copy it across first.

```bash
mkdir -p modules
cp -RL ./logos/modules/. ./modules/

```

### 3.4 Install the .lgx packages with lgpm

Install the freshly-built packages into `./modules`. These are all
`core` modules, so they go to `--modules-dir`. The packages are
unsigned (local dev builds), so we pass `--allow-unsigned`.

```bash
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file lez-core-lgx/*.lgx
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file lez-rln-lgx/*.lgx
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file rln-lgx/*.lgx
./lgpm/bin/lgpm --modules-dir ./modules --allow-unsigned install --file delivery-lgx/*.lgx

```

### 3.5 Confirm the install

Scan the directory and confirm all four modules landed:

```bash
./lgpm/bin/lgpm --modules-dir ./modules list
```

---

## Step 4: Run the daemon and call the module

Start `logoscore` in daemon mode pointed at `./modules`, then use the client
subcommands to load `delivery_module`, introspect it, create a node from a
Waku config, and start it. Daemon output is captured in `logs.txt`.

### 4.1 Write the node config

Create `waku-config.json` — a `logos.dev` network configuration for the
delivery node: cluster 2 with 8 auto-shards, relay/filter/lightpush
enabled, mix routing, and discv5 discovery.

> Extended Kademlia discovery (`enableKadDiscovery` +
> `kadBootstrapNodes`) is intentionally omitted here. The node's
> `start` performs a **blocking** Kademlia DHT bootstrap that only
> returns once the DHT is joined; in a headless/CI run that bootstrap
> does not complete, so `start` never returns and the call times out.
> discv5 (plus the relay/rendezvous peers) is enough to join the
> network for this doc-test. See `tests/test_delivery_start_hang.cpp`.

```json
{
  "tcpPort": 30303,
  "discv5UdpPort": 9000,
  "nodekey": "b9800176f31e41304dff5d385944c349300204361fca56beec956b7181fbc5ae",
  "clusterId": 2,
  "numShardsInNetwork": 8,
  "maxConnections": 300,
  "relay": true,
  "store": false,
  "filter": true,
  "lightpush": true,
  "logLevel": "DEBUG",
  "websocketSupport": true,
  "websocketPort": 8000,
  "websocketSecureSupport": false,
  "discv5Discovery": true,
  "mix": true,
  "mixkey": "fc2d71d52cdd37cb49cca3f6a8e6877f40bc999ed67ab5808bb3b9f685cf0f94",
  "nat": "extip:138.68.122.137",
  "extMultiaddrs": ["/dns4/delivery-01.do-ams3.logos.dev.status.im/tcp/30303"]
}
```

### 4.2 Start the daemon

Start logoscore in daemon mode in the background, capturing output to
`logs.txt`:

```bash
logoscore -D -m ./modules > logs.txt &
```

The `-D` flag starts the daemon. The client subcommands below connect to
this running process via the config written under `~/.logoscore/`.

```bash
sleep 3
```

### 4.3 Inspect the startup log

Review the daemon's startup output:

```bash
cat logs.txt
```

### 4.4 Check daemon status

Verify the daemon is running:

```bash
logoscore status
```

### 4.5 List discovered modules

`delivery_module` should be visible in the scan directory:

```bash
logoscore list-modules
```

### 4.6 Load the module

Load `delivery_module` into the running daemon. Only this module is
named — the host resolves `metadata.json#dependencies` and loads the
RLN chain with it:

```bash
logoscore load-module delivery_module
```

### 4.7 Confirm the module is loaded

Re-run `status`; the module that was `not_loaded` before now reports
`loaded`:

```bash
logoscore status
```

### 4.8 Introspect the module with module-info

`module-info` lists the `Q_INVOKABLE` methods the module exposes — the
same methods you can `call`:

```bash
logoscore module-info delivery_module
```

### 4.9 Create the delivery node

`createNode` takes a Waku node configuration JSON document and creates a
liblogosdelivery node context. The `@` prefix tells `logoscore` to load
the file contents as the argument:

```bash
logoscore call delivery_module createNode @waku-config.json
```

```bash
sleep 5
```

### 4.10 Start the delivery node

`start` boots the node created by `createNode`. This is a synchronous call
that returns when the node has started:

```bash
logoscore call delivery_module start
```

### 4.11 Review daemon logs after node start

Check the daemon output for node startup activity:

```bash
cat logs.txt
```

### 4.12 Stop the daemon

Shut the daemon down cleanly:

```bash
logoscore stop
```

The daemon removes its state file and exits.

```bash
sleep 5
```

### 4.13 Confirm the daemon has stopped

With no daemon running, the client reports `not_running` and exits
non-zero, so we add `|| true` to let the doc-test assert on the output:

```bash
logoscore status
```
