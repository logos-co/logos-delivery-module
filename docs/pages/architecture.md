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
messages coming back the other way into typed events. Everything about how
messages actually travel is decided by
[logos-delivery](https://github.com/logos-messaging/logos-delivery).

## Threads

`liblogosdelivery` is built on nim-ffi's **poll model**: every export answers
with a message on one queue per node, and a file descriptor is readable while
the queue holds something. This module never receives a callback. nim-ffi's own host-side queue reader
(`nim_ffi::Host`, `lib/nim_ffi_host.hpp`, vendored from nim-ffi's `host/`)
reads that queue on the module's own thread: a
`QSocketNotifier` drains it from the Qt event loop between calls, and a method
call pumps inline until its reply arrives — serving events and the library's
RLN questions on the way, so nothing can deadlock waiting for something this
thread must itself deliver.


The process holds three threads of its own: the Qt main thread (module
methods, the pump), logos-protocol's transport thread (`client_thread_entry`),
and liblogosdelivery's one nim-ffi thread running the node
(`typedthreads::threadProcWrapper`) — plus, transiently, the RLN bring-up
thread `createNode` spawns, reaped by the next method call once it is done.
`ps -M` may also show a `_pthread_wqthread`: a libdispatch worker the system
keeps parked, not one of ours. Measured under `logoscore` on
macOS (`ps -M` on the module's host process, node created, started, one send):

| delivery_module host | threads |
| --- | --- |
| released 0.2.1, callback model, no RLN | 4 (+ nim-ffi's event thread) |
| callback model with the RLN bridge (two worker lanes) | 6 |
| this build, poll model, RLN mounted and answering | **3** (+ a libdispatch worker the system parks) |

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
