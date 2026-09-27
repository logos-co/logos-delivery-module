# Architecture

Where this module sits in the stack, and how much of that stack a given node
actually mounts.

## Layers

A message passes through four layers on its way out:

```text
Logos Core  ·  your module or UI
     │  calls delivery_module methods
     ▼
delivery_module            ← this repository, a Qt plugin
     │  C FFI
     ▼
liblogosdelivery
     │  Nim API
     ▼
logos-delivery             ← the node implementation
```

This repository is the middle box. It owns no protocol logic: it adapts the
universal Logos module API onto `liblogosdelivery`'s C FFI, and turns the
callbacks coming back the other way into typed events. Everything about how
messages actually travel is decided by
[logos-delivery](https://github.com/logos-messaging/logos-delivery).

That division is why the configuration you pass to `createNode` is handed
through verbatim — logos-delivery owns the grammar, and this module does not
interpret it.

## What a node mounts

`createNode`'s `entryLayer` decides how much of the stack comes up:

| `entryLayer` | What you get |
| ------------ | ------------ |
| `"kernel"` | Transport node only. |
| `"messaging"` | Kernel plus the messaging client. |
| `"channels"` | Kernel, messaging, and reliable channels. **The default.** |

The layer you pick determines which methods work. On a kernel-only node,
`send`, `subscribe` and the `channel*` methods fail with "node has no messaging
client" or "no reliable channel manager". `getNodeInfo`, `storeQuery` and
metrics keep working.

The concrete configuration shapes — an app developer's full stack, a node
operator's public service node, a self-hosted network — are documented with
`createNode` in the [API reference](api_reference.rst).

## Plugin-hosted discovery

With `pluginKadDiscovery` on, logos-delivery delegates kademlia service
discovery to this module, which hosts it on `libp2p_module`. After `createNode`
the module asks the node (`logosdelivery_get_discovery_requirements`) whether a
plugin is expected and which DHT bootstrap peers its configuration resolved,
presets included. It sets `libp2p_module` up from that answer — the peers as
`bootstrapNodes`, `mountKad` and `mountServiceDiscovery` on — laid over
libp2p's own `LIBP2P_MODULE_CONFIG`.

libp2p is first contacted on the first discovery call after `start`, not at
`createNode`, so a missing module or a bad bootstrap set surfaces there. Only
the first bootstrap peer is handed over: libp2p dials the set inside a fixed
10 s call budget, which two DNS-resolved peers exceed.

Set `LD_DISCO_TRACE` to a file path to log every call across the plugin
boundary; logos-core discards a module's stderr, so that file is the only view
into it.
