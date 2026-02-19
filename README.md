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

The delivery module provides the following API methods:

- `createNode(cfg: QString)` - Initialize the delivery node with JSON configuration
- `start()` - Start the delivery node
- `stop()` - Stop the delivery node
- `send(contentTopic: QString, payload: QString)` - Send a message
- `subscribe(contentTopic: QString)` - Subscribe to receive messages on a topic
- `unsubscribe(contentTopic: QString)` - Unsubscribe from a topic

### Events

The module emits the following events:

- `deliveryInitialized` - When the node is initialized
- `deliveryStarted` - When the node is started
- `messageSent` - When a message is successfully sent
- `messageReceived` - When a message is received
- `messageError` - When an error occurs during message sending

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
