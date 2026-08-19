# Run the delivery node over the libp2p module's node

By default, `delivery_module` starts a libp2p node of its own inside `liblogosdelivery`. It is also possible to use `logos-libp2p-module`'s libp2p node by setting `useLibp2pModule` in the `createNode` config.

```json
{
  "mode": "Edge",
  "preset": "logos.test",
  "kernelConf": { "nodekey": "<hex private key>" },
  "useLibp2pModule": true
}
```

The provider is always [`logos-libp2p-module`](https://github.com/logos-co/logos-libp2p-module), which logos-core routes to under the name `libp2p_module` from its `metadata.json`. That name is a constant here (`kLibp2pModuleName` in `src/net_bridge.h`), because it is the only module that provides the `libp2p` interface and `bind_libp2p` needs a routing name rather than an implementation choice. `interfaces/libp2p.h` mirrors that module's `src/plugin.h`, and the signatures must match it exactly. The minimum provider is master at `ec7b8f5` or later, which is where `pingPeer` and the `peerId` field on `protocolAcceptStream` first appear.

## Preconditions, each one enforced by `createNode`

1. `libp2p_module` runs in the same logos-core instance with its node started, so it answers `getNodeInfo("PeerId")`.
2. No other delivery node in the process already drives it.
3. Both modules hold the same private key (`kernelConf.nodekey` here, `privKey` there). The two peer IDs are compared, and a node that does not answer `getNodeInfo("MyPeerId")` leaves the identity unverified and logs that.
4. `liblogosdelivery` exports `logosdelivery_register_net_backend` and `logosdelivery_net_backend_respond`. Both resolve at runtime, so a library without them runs the default path and `createNode` reports "liblogosdelivery has no net backend ABI".

## Wire contract

`liblogosdelivery` calls `submit(requestId, opJson)` with `{"op": "<name>", "args": {...}}`, and this module answers `logosdelivery_net_backend_respond(requestId, ok, data)`, where `data` is the result JSON or the error text. `submit` returns at once and up to 16 worker threads run the calls, so a long poll does not stop the other traffic. Every request gets one answer, and past 4096 queued requests that answer is "libp2p net bridge queue is full".

This list is the allow-list, and each `args` object is the argument JSON that the provider method documents.

| op | provider call |
| --- | --- |
| `getNodeInfo` | `getNodeInfo(field)` |
| `connectPeer` | `connectPeer(peerId, multiaddrs, timeoutMs)` |
| `connectedPeers` | `connectedPeers(direction)` |
| `dial` | `dial(peerId, proto)` |
| `pingPeer` | `pingPeer(peerId, timeoutMs)` |
| `mountProtocol` | `mountProtocol(proto)` |
| `protocolRequest` | `protocolRequest(argsJson)` |
| `protocolAcceptStream` | `protocolAcceptStream(argsJson)` |
| `streamReadLp` | `streamReadLpJson(argsJson)` |
| `streamWriteLp` | `streamWriteLpJson(argsJson)` |
| `streamClose` | `streamCloseJson(argsJson)` |
| `streamRelease` | `streamReleaseJson(argsJson)` |

## Limits

- Edge protocols only: relay needs a gossipsub facade that the delivery api layer does not have.
- discv5 stays off. It owns a UDP socket and the signature key, so use entry nodes and peer exchange.
- No push events: an inbound stream waits in a queue on the provider side and the library polls it.
- Every stream read and write costs one call across the module boundary.
- A provider restart invalidates every stream id and subscription. Create the delivery node again.
- One delivery node per process may bridge. A second `createNode` with `useLibp2pModule` fails instead of taking the provider over.
- The C ABI has no unregister, so the bridge lives for the process. A worker parked in a long poll can return after the delivery node is gone; it then finds the transport marked dead and answers with an error.
