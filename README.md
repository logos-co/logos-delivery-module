# logos-delivery-module

Wrap LogosMessaging API (liblogosdelivery) and make it available as a Logos Core module.

This module provides high-level message delivery capabilities through the liblogosdelivery interface from [logos-delivery](https://github.com/logos-messaging/logos-delivery), packaged as a Logos module plugin compatible with logos-core.

Full API documentation is in [`src/delivery_module_plugin.h`](src/delivery_module_plugin.h) (`DeliveryModulePlugin`).

## How to Build

### Using Nix (Recommended)

#### Build Complete Module (Library + Headers)

```bash
# Build everything (default)
nix build
```

The result will include:
- `/lib/delivery_module_plugin.dylib` (or `.so` on Linux) - The Delivery module plugin
- `/lib/liblogosdelivery.dylib` (or `.so` on linux) - The logos-delivery library
- `/lib/librln.dylib` (or `.so` in linux) - Zerokit's RLN library
- `/lib/libpq.dylib` (or `.so` on Linux) - PostgreSQL runtime library
- `/lib/libpq.5.dylib` (or `.so.5` on Linux)

#### Build Individual Components

```bash
# Build only the library (plugin + liblogosdelivery reference)
nix build '.#lib'

# Build only the generated headers
nix build '.#include'

# build module in local and in protable logos_core format
nix build .#lgx / .#lgx-portable
```

#### Development Shell

```bash
# Enter development shell with all dependencies
nix develop
```

**Note:** In zsh, you need to quote the target (e.g., `'.#default'`) to prevent glob expansion.

If you don't have flakes enabled globally, add experimental flags:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

The compiled artifacts can be found at `result/`

## Output Structure

When built with Nix, the module produces:

```
result/
└── lib/
    ├── delivery_module_plugin.dylib  # or .so on Linux — Logos module plugin
    ├── liblogosdelivery.dylib
    ├── librln.dylib
    ├── libpq.dylib                   # or .so on Linux — PostgreSQL runtime
    └── libpq.5.dylib                 # or .so.5 on Linux
```

### Requirements

#### Build Tools
- CMake (3.14 or later)
- Ninja build system
- pkg-config

#### Dependencies
- Qt6 (qtbase)
- Qt6 Remote Objects (qtremoteobjects)
- logos-liblogos (provided via Nix)
- logos-cpp-sdk (provided via Nix)
- logos-delivery / liblogosdelivery — target (provided via Nix)
- PostgreSQL (libpq) — runtime dependency bundled by the Nix build

All dependencies are automatically handled by the Nix flake configuration.

## Module Interface

The delivery module provides the following API methods (all synchronous, all return LogosResult):

- `createNode(cfg: QString)` - Initialize the delivery node with a JSON configuration (call once)
- `start()` - Start the delivery node
- `stop()` - Stop the delivery node
- `send(contentTopic: QString, payload: QString)` - Send a message (returns a request id)
- `subscribe(contentTopic: QString)` - Subscribe to receive messages on a topic
- `unsubscribe(contentTopic: QString)` - Unsubscribe from a topic
- `getAvailableNodeInfoIDs()` - List queryable node info identifiers
- `getNodeInfo(nodeInfoId: QString)` - Retrieve node info by identifier
- `getAvailableConfigs()` - Retrieve available configuration parameter descriptions
- `collectOpenMetricsText()` - Node metrics as OpenMetrics/Prometheus text for the `openmetrics` module (see [Metrics](#metrics))

### Node Configuration (`createNode`)

`createNode` accepts a **flat** JSON object whose keys correspond to `WakuNodeConf`
field names (camelCase) from
[logos-delivery](https://github.com/logos-messaging/logos-delivery).
Unknown keys are silently ignored. Every field has a built-in default, so only
values that differ from defaults need to be supplied.

#### Commonly used keys

| Key                  | Type             | Default    | Description                              |
|----------------------|------------------|------------|------------------------------------------|
| `mode`               | string           | `"noMode"` | `"Core"`, `"Edge"`, or `"noMode"`        |
| `preset`             | string           | `""`       | Network preset (`"logos.test"`, `"logos.dev"`, `"twn"`) |
| `clusterId`          | number (uint16)  | `0`        | Cluster identifier                       |
| `entryNodes`         | array of string  | `[]`       | Bootstrap peers (enrtree / multiaddress) |
| `relay`              | boolean          | `false`    | Enable relay protocol                    |
| `rlnRelay`           | boolean          | `false`    | Enable RLN rate-limit nullifier          |
| `tcpPort`            | number (uint16)  | `60000`    | P2P TCP listen port                      |
| `numShardsInNetwork` | number (uint16)  | `1`        | Auto-sharding shard count                |
| `logLevel`           | string           | `"INFO"`   | `"TRACE"`, `"DEBUG"`, `"INFO"`, `"WARN"` |
| `logFormat`          | string           | `"TEXT"`   | `"TEXT"` or `"JSON"`                     |
| `maxMessageSize`     | string           | `"150KiB"` | Maximum message payload size             |

#### Presets

Using a `preset` populates cluster ID, entry nodes, sharding, RLN, and other
network-specific defaults automatically. Individual keys supplied alongside a
preset override the preset values.

- `"logos.test"` – Logos Test fleet (the default for running a node; mix
  enabled, p2pReliability on, auto-shards, built-in bootstrap nodes).
- `"logos.dev"` – Logos Dev Network (cluster 2, mix enabled, p2pReliability on,
  8 auto-shards, built-in bootstrap nodes).
- `"twn"` – The RLN-protected Waku Network (cluster 1).

Minimal example using the default `logos.test` preset:

```json
{
  "logLevel": "INFO",
  "mode": "Core",
  "preset": "logos.test"
}
```

### Content Topics

Content topics identify message channels for publishing and subscribing. Use a
properly structured content topic for your application following the format
specified in
[LIP-23: Topics](https://lip.logos.co/messaging/informational/23/topics.html#content-topics).

Example: `"/myapp/1/chat/proto"`

### Sending Messages (`send`)

`send(contentTopic, payload)` accepts a content topic and a raw payload string.
The plugin converts the payload to UTF-8 bytes, base64-encodes it, and wraps it
in a JSON envelope before crossing the FFI boundary:

```json
{ "contentTopic": "<topic>", "payload": "<base64>", "ephemeral": false }
```

The call is synchronous and returns a **request id** on success. The actual
network delivery is asynchronous — track results via the emitted events:

- **`messageError`** – the module could not send the message.
- **`messagePropagated`** – the message reached the network but is not yet
  validated.
- **`messageSent`** – the message has been confirmed by the network.

### Events

Asynchronous events are emitted off-thread as Logos Plugin events. Each event
carries a `QVariantList data` with positional values:

- **`messageSent`** – message confirmed by the network
  - `data[0]` (`QString`): request id
  - `data[1]` (`QString`): message hash
  - `data[2]` (`QString`): local timestamp (ISO-8601)
- **`messageError`** – send failure
  - `data[0]` (`QString`): request id
  - `data[1]` (`QString`): message hash
  - `data[2]` (`QString`): error message
  - `data[3]` (`QString`): local timestamp (ISO-8601)
- **`messagePropagated`** – message reached the network but not yet validated
  - `data[0]` (`QString`): request id
  - `data[1]` (`QString`): message hash
  - `data[2]` (`QString`): local timestamp (ISO-8601)
- **`messageReceived`** – a message arrived on a subscribed topic
  - `data[0]` (`QString`): message hash
  - `data[1]` (`QString`): content topic
  - `data[2]` (`QString`): payload (base64-encoded)
  - `data[3]` (`QString`): timestamp (nanoseconds since epoch)
- **`connectionStateChanged`** – node connectivity change
  - `data[0]` (`QString`): connection status
  - `data[1]` (`QString`): local timestamp (ISO-8601)

### Metrics

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

## Architecture

```
┌─────────────────────────────────────┐
│  Logos Core (Qt Application)        │
└──────────────┬──────────────────────┘
               │
               │ Plugin Interface
               ▼
┌─────────────────────────────────────┐
│  delivery_module_plugin             │
│  (Qt Plugin - this repository)      │
└──────────────┬──────────────────────┘
               │
               │ C FFI
               ▼
┌─────────────────────────────────────┐
│  liblogosdelivery                   │
│  (from logos-delivery)              │
│  High-level Message-delivery API    │
└──────────────┬──────────────────────┘
               │
               │ Nim API
               ▼
┌─────────────────────────────────────┐
│ logos-delivery                      │
│ Core message-delivery implementation│
└─────────────────────────────────────┘
```

## Development

### Local Development

```bash
# Enter development shell (exports LOGOS_MODULE_BUILDER_ROOT and all other deps)
nix develop

# Configure — env vars are exported automatically by the dev shell
cmake -B build -S . -GNinja

# Build
ninja -C build
```
