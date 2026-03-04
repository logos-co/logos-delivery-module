# logos-delivery-module

Wrap LogosMessaging API (liblogosdelivery) and make it available as a Logos Core module.

This module provides high-level message delivery capabilities through the liblogosdelivery interface from [logos-delivery](https://github.com/logos-messaging/logos-delivery), packaged as a Logos module plugin compatible with logos-core.

## How to Build

### Using Nix (Recommended)

#### Build Complete Module (Library + Headers)

```bash
# Build everything (default)
nix build

# Or explicitly
nix build '.#default'
```

**Current Status**: The build will compile the plugin successfully but fail at the install phase when looking for liblogosdelivery.dylib, as it's not yet available from logos-delivery.

The result will include (once liblogosdelivery is available):
- `/lib/logos/modules/delivery_module_plugin.dylib` (or `.so` on Linux) - The Delivery module plugin
- Symlink to `liblogosdelivery.dylib` (or `.so` on Linux) from logos-delivery
- `/share/logos-delivery-module/metadata.json` - Module metadata
- `/share/logos-delivery-module/generated/` - Generated module files

#### Build Individual Components

```bash
# Build only the library (plugin + liblogosdelivery reference)
nix build '.#lib'

# Build only the generated headers
nix build '.#include'
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
├── lib/
│   ├── liblogosdelivery.dylib        # Logos Messaging library (symlinked)
│   └── delivery_module_plugin.dylib  # Logos module plugin
└── include/
    ├── delivery_module_plugin.h      # Generated API header
    └── delivery_module_plugin.cpp    # Generated API implementation
    └── liblogosdelivery.h            # Header for liblogosdelivery
```

Both libraries must remain in the same directory, as `delivery_module_plugin.dylib` is configured with `@loader_path` to find `liblogosdelivery.dylib` relative to itself.

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
- logos-delivery with liblogosdelivery target (provided via Nix)

All dependencies are automatically handled by the Nix flake configuration.

## Module Interface

The delivery module provides the following API methods (all synchronous):

- `createNode(cfg: QString)` - Initialize the delivery node with a JSON configuration (call once)
- `start()` - Start the delivery node
- `stop()` - Stop the delivery node
- `send(contentTopic: QString, payload: QString)` - Send a message (returns a request id)
- `subscribe(contentTopic: QString)` - Subscribe to receive messages on a topic
- `unsubscribe(contentTopic: QString)` - Unsubscribe from a topic
- `getAvailableNodeInfoIDs()` - List queryable node info identifiers
- `getNodeInfo(nodeInfoId: QString)` - Retrieve node info by identifier
- `getAvailableConfigs()` - Retrieve available configuration parameter descriptions

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
| `preset`             | string           | `""`       | Network preset (`"twn"`, `"logos.dev"`)   |
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

- `"twn"` – The RLN-protected Waku Network (cluster 1).
- `"logos.dev"` – Logos Dev Network (cluster 2, mix enabled, p2pReliability on,
  8 auto-shards, built-in bootstrap nodes).

Minimal example using the `logos.dev` preset:

```json
{
  "logLevel": "INFO",
  "mode": "Core",
  "preset": "logos.dev"
}
```

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
- **`connectionStateChanged`** – node connectivity change
  - `data[0]` (`QString`): connection status
  - `data[1]` (`QString`): local timestamp (ISO-8601)

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
# Enter development shell
nix develop

# Now you have access to all build tools and dependencies
cmake -B build -S . -GNinja \
  -DLOGOS_CPP_SDK_ROOT=$LOGOS_CPP_SDK_ROOT \
  -DLOGOS_LIBLOGOS_ROOT=$LOGOS_LIBLOGOS_ROOT \
  -DLOGOS_DELIVERY_ROOT=$LOGOS_DELIVERY_ROOT

# Build
ninja -C build
```
