# Running This Delivery Module Against logosctl

`logos-delivery-module` is a Logos `core` module that wraps
[liblogosdelivery](https://github.com/logos-messaging/logos-delivery) to provide
high-level message-delivery capabilities. This doc-test exercises **this**
delivery-module commit end-to-end through the headless `logosctl` runtime:

1. Build the `logosctl` CLI from its published flake. `logosctl` is the
   headless frontend for `logos-liblogos` and installs packages itself, so
   building it brings in the whole module-runtime stack (`logos_host`,
   `liblogos_core`, the IPC layer).
2. Build **this** delivery module as an installable `.lgx` package straight from
   its own flake's `#lgx` output, **pinned to the commit under test** — so the
   module you run is built from exactly what is checked out here, not the latest
   published release.
3. Start the `logosctl` daemon and install the `.lgx` into its session,
   together with the RLN modules `delivery_module` requires and the
   optional `libp2p_module`.
4. Load `delivery_module`, introspect it with `module show`, call
   `createNode` with a Waku node config, then call `start` — verifying the
   module actually runs and boots a delivery node.

Because the module is built from the commit under test and then loaded and called
through a real `logosctl` daemon, a green run is real evidence that this change
keeps the delivery module loadable and callable.

On Windows, CI cross-builds the module and the RLN modules it requires, and
uses a staged native `logosctl` host to load them and create a node with RLN
(Rate Limiting Nullifier) off. `libp2p_module`, which has no Windows build,
is left out. That leg uses an offline config, verifies RLN stays disabled,
and stops before `start`, which requires network access.

**What you'll build:** This `delivery_module`, packaged as `.lgx` and installed into a `logosctl` session on Linux/macOS, then loaded by a native `logosctl` daemon on Windows.

**What you'll learn:**

- How to build the `logosctl` runtime from its flake
- How a module's flake exposes a ready-to-install `.lgx` via its `#lgx` output
- How to install an `.lgx` into a `logosctl` session
- How a module's declared dependencies have to be installed alongside it
- How to start the `logosctl` daemon, load a module, introspect it, and call its methods
- How to create and start a delivery node with `createNode` and `start`
- How to tell a created node from a running one, and read its connection status
- How to shut the daemon down and confirm it has exited
- How to load the module and its RLN dependencies and create a node on Windows

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- **A Linux or macOS machine for the package walkthrough.** The Windows section runs in CI against staged cross-built artifacts.

---

## Step 1: Build logosctl

Build the `logosctl` CLI from its published flake. The result is symlinked to
`./logos/`. `logosctl` is the headless frontend for `logos-liblogos`, so this
one build brings in the whole module-runtime stack the daemon needs.

### 1.1 Build the CLI

```bash
nix build 'github:logos-co/logos-logoscore-cli#ctl' --out-link ./logos
```

The build produces `logos/bin/logosctl` plus bundled runtime libraries
and a `logos/modules/` directory containing the built-in
`capability_module` (required for the auth handshake when loading
modules). The daemon scans that directory on its own.

---

## Step 2: Build this delivery module

Build **this** delivery module's `.lgx` straight from its flake's `#lgx`
output, along with the RLN modules it requires and the optional
`libp2p_module`. Each
build drops its package into `./packages/`, which the next section
installs into a `logosctl` session in one go. Every module built with
[`logos-module-builder`](https://github.com/logos-co/logos-module-builder)
exposes a ready-to-install `#lgx`.

> The `` in the URL is what pins the build to a specific commit: the
> doc-test runner expands it to a concrete ref. Locally that is this
> checkout's `HEAD` (see `run.sh`); in CI it is the commit being tested. With
> no pin it falls back to the latest `master`.

### 2.1 Build the module's .lgx

Build the `#lgx` output, link it as `./delivery-lgx`, and copy the
package into `./packages/`. (This compiles the module and its SDK
dependencies through Nix, so the first build is slow.)

```bash
# From inside the clone this is simply: nix build '.#lgx'
nix build 'github:logos-co/logos-delivery-module#lgx' -o delivery-lgx
mkdir -p packages && cp -f delivery-lgx/*.lgx packages/
```

The `.lgx` package is now in `./packages/`:

```bash
ls packages/
```

### 2.2 Build the libp2p dependency

`libp2p_module` is an optional dependency of `delivery_module`: it
hosts external service discovery. A node configured for that
(`plugin-kad-discovery`) fails to start without it; any other node
runs without it. Build it at the rev this module's `flake.lock` pins
for it. From a clone, this flake re-exports the
same package — `nix build '.#libp2p_module-lgx'` — which reads the
pin straight out of the lock.

The second command builds the module's `#lib` without linking it.
The `.lgx` is a development package: its libraries load their own
dependencies (on Linux, `libtinycbor`) from the Nix store rather
than carrying them. The package is compressed, so Nix cannot see
those references and never fetches them; building `#lib` brings
them into the store.

```bash
nix build 'git+https://github.com/logos-co/logos-libp2p-module?rev=63275942f125c3fd77d4842e137bea2a8cded665#lgx' -o libp2p-lgx
nix build 'git+https://github.com/logos-co/logos-libp2p-module?rev=63275942f125c3fd77d4842e137bea2a8cded665#lib' --no-link
cp -f libp2p-lgx/*.lgx packages/

```

One more `.lgx` package:

```bash
ls packages/
```

### 2.3 Build the RLN dependency chain

`rln_module` is a required dependency of `delivery_module`: the
module does not load unless the whole chain is installed alongside
it — `rln_module` → `rln_lez_module` (the registry provider) and
`membership_allocation_client_module` (the allocator client, which
brings its own libp2p node). That holds even for a node whose preset
has RLN off.

Each is built at the rev this module's `flake.lock` pins for it, so
the RLN modules you install are the ones the delivery module was
built against. From a clone, this flake re-exports the same three
packages — `nix build '.#rln_module-lgx'`, and likewise
`.#rln_lez_module-lgx` and
`.#membership_allocation_client_module-lgx` — which reads the pins
straight out of the lock.

```bash
nix build 'github:logos-co/logos-rln-module/992fab9747aa84a3c72a36176470dccf226130f6#lgx' -o rln-lgx
nix build 'github:logos-co/logos-rln-lez-module/5a3b87dd0fecf22e97ce81603eba348d2bff69cc#lgx' -o rln-lez-lgx
nix build 'github:igor-sirotin/logos-membership-allocation-client-module/82660ca1b8cb19118b2ce1cba72752fc6baa4897#lgx' -o allocation-client-lgx
cp -f rln-lgx/*.lgx rln-lez-lgx/*.lgx allocation-client-lgx/*.lgx packages/

```

Three more `.lgx` packages, one per module in the chain:

```bash
ls packages/
```

---

## Step 3: Run the daemon, install and call the module

Start a `logosctl` daemon and install the packages into its session, then
use the client subcommands to load `delivery_module`, introspect it, create
a node from a Waku config, and start it. Daemon output goes to the
session's `logs/daemon.log`.

### 3.1 Write the node config

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

### 3.2 Start the daemon

Start the logosctl daemon. It keeps its config, installed modules and
module data in one session directory (`~/.logosctl` by default; this
doc-test passes `--config-dir ./session`):

```bash
logosctl daemon start --detach
```

`--detach` returns once the daemon accepts commands. The client
subcommands below connect to it through the session directory.

### 3.3 Inspect the startup log

Review the daemon's startup output:

```bash
cat session/logs/daemon.log
```

### 3.4 Install the .lgx packages

Install every package in `./packages/` into the session. `--dir`
takes each `.lgx` in the directory from disk, with no catalog
involved. These are all `core` modules, so they land in the session's
`modules/` directory. The packages are unsigned (local dev builds),
which the default signature policy accepts with a warning; `-y`
confirms, since there is no terminal to ask on.

```bash
logosctl install --dir packages -y
```

### 3.5 Confirm the install

List the session's packages and confirm all five landed:

```bash
logosctl package ls
```

### 3.6 Check daemon status

Verify the daemon is running:

```bash
logosctl daemon status
```

### 3.7 List discovered modules

`delivery_module` should be visible in the session's modules directory:

```bash
logosctl module ls
```

### 3.8 Load the module

Load `delivery_module` into the running daemon. The host loads the
RLN chain it requires along with it:

```bash
logosctl module load delivery_module
```

### 3.9 Confirm the module is loaded

Re-run `daemon status`; the module that was `not_loaded` before now
reports `loaded`:

```bash
logosctl daemon status
```

### 3.10 Introspect the module with module show

`module show` lists the `Q_INVOKABLE` methods the module exposes — the
same methods you can `call`:

```bash
logosctl module show delivery_module
```

### 3.11 Create the delivery node

`createNode` takes a Waku node configuration JSON document and creates a
liblogosdelivery node context. The `@` prefix tells `logosctl` to load
the file contents as the argument:

```bash
logosctl call delivery_module createNode @waku-config.json
```

```bash
sleep 5
```

### 3.12 Check the node is not running yet

`createNode` only creates the node. The `IsRunning` node info item tells
a created node from a started one:

```bash
logosctl call delivery_module getNodeInfo IsRunning
```

### 3.13 Read the connection status before start

`getConnectionStatus` reads the status `connectionStateChanged` reports
on transitions. A node that is not started is `Disconnected`:

```bash
logosctl call delivery_module getConnectionStatus
```

### 3.14 Start the delivery node

`start` boots the node created by `createNode`. This is a synchronous call
that returns when the node has started:

```bash
logosctl call delivery_module start
```

### 3.15 Check the node is running

```bash
logosctl call delivery_module getNodeInfo IsRunning
```

### 3.16 Read the connection status after start

Once started, the status follows the node's peers: `Disconnected`,
`PartiallyConnected` or `Connected`, depending on how far it has
joined the network.

```bash
logosctl call delivery_module getConnectionStatus
```

### 3.17 Review daemon logs after node start

Check the daemon output for node startup activity:

```bash
cat session/logs/daemon.log
```

### 3.18 Stop the daemon

Shut the daemon down cleanly:

```bash
logosctl daemon stop
```

`daemon stop` returns once the daemon has exited.

### 3.19 Confirm the daemon has stopped

With no daemon running, the client reports `not_running` and exits
non-zero, so we add `|| true` to let the doc-test assert on the output:

```bash
logosctl daemon status
```

---

## Step 4: Create a Windows node with the RLN modules installed

CI stages the Windows modules, their DLLs, and a native `logosctl` host.
Copy `delivery_module` and the RLN chain it requires into a private
session's `modules/` directory, then start a daemon on that session and
use an offline node config. The shared Windows runner provides `run` to
launch each staged executable.

### 4.1 Stage the delivery module and the RLN chain

```bash
mkdir -p windows-smoke-config/modules
cp -R install-portable/modules/delivery_module windows-smoke-config/modules/
for m in rln_module rln_lez_module membership_allocation_client_module; do
  cp -R "$m-install-portable/modules/$m" windows-smoke-config/modules/
done

```

### 4.2 Write an offline node config

```json
{"logLevel":"INFO"}
```

### 4.3 Start the Windows daemon

```bash
run windows-logosctl/bin/logosctl.exe --config-dir ./windows-smoke-config daemon start --detach
```

### 4.4 Discover the modules

```bash
run windows-logosctl/bin/logosctl.exe --config-dir ./windows-smoke-config module ls
```

### 4.5 Load the module with its RLN dependencies

```bash
run windows-logosctl/bin/logosctl.exe --config-dir ./windows-smoke-config module load delivery_module
```

### 4.6 Confirm the RLN modules are loaded

```bash
run windows-logosctl/bin/logosctl.exe --config-dir ./windows-smoke-config daemon status
```

### 4.7 Inspect the module methods

```bash
run windows-logosctl/bin/logosctl.exe --config-dir ./windows-smoke-config module show delivery_module
```

### 4.8 Create an offline delivery node

```bash
run windows-logosctl/bin/logosctl.exe --config-dir ./windows-smoke-config call delivery_module createNode @windows-smoke-node.json
```

### 4.9 Confirm RLN remains disabled

```bash
run windows-logosctl/bin/logosctl.exe --config-dir ./windows-smoke-config call delivery_module rlnState
```

### 4.10 Confirm the node is created but not running

```bash
run windows-logosctl/bin/logosctl.exe --config-dir ./windows-smoke-config call delivery_module getNodeInfo IsRunning
```

### 4.11 Stop the Windows daemon

```bash
run windows-logosctl/bin/logosctl.exe --config-dir ./windows-smoke-config daemon stop
```
