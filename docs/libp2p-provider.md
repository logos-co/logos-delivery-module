# Run the delivery node over the libp2p module's node

By default, `delivery_module` starts a libp2p node of its own inside `liblogosdelivery`. It is also possible to use `logos-libp2p-module`'s libp2p node by setting `libp2pProvider` in the `createNode` config.

```json
{
  "mode": "Edge",
  "preset": "logos.test",
  "kernelConf": { "nodekey": "<hex private key>" },
  "libp2pProvider": "libp2p_module"
}
```

The value is the logos-core module name of the provider, and this module both binds that name over the `libp2p` interface and registers the net backend under it. `liblogosdelivery` reads the same key and builds the node on the backend of that name, so the two sides agree by construction. See its `docs/api/net-backend.md`.

The provider is [`logos-libp2p-module`](https://github.com/logos-co/logos-libp2p-module), which logos-core routes to under the name `libp2p_module` from its `metadata.json`. `interfaces/libp2p.h` mirrors that module's `src/plugin.h`, and the signatures must match it exactly. The minimum provider is master at `ec7b8f5` or later, which is where `pingPeer` and the `peerId` field on `protocolAcceptStream` first appear.

## Preconditions, each one enforced by `createNode`

1. The named module runs in the same logos-core instance with its node started, so it answers `getNodeInfo("PeerId")`.
2. No other delivery node in the process already drives it.
3. Both modules hold the same private key (`kernelConf.nodekey` here, `privKey` there). The two peer IDs are compared, and a node that does not answer `getNodeInfo("MyPeerId")` leaves the identity unverified and logs that.
4. `liblogosdelivery` exports `logosdelivery_register_net_backend` and `logosdelivery_net_backend_respond`. Both resolve at runtime, so a library without them runs the default path and `createNode` reports "liblogosdelivery has no net backend ABI".

## Wire contract

`liblogosdelivery` calls `submit(requestId, opJson)` with `{"op": "<name>", "args": {...}}`, and this module answers `logosdelivery_net_backend_respond(requestId, ok, data)`, where `data` is the result JSON or the error text. `submit` returns at once and up to 16 worker threads run the calls, so a long poll does not stop the other traffic. Every request gets one answer, and past 4096 queued requests that answer is "libp2p net bridge queue is full".

This list is the allow-list, and each `args` object is the argument JSON that the provider method documents.

| op | provider call |
| --- | --- |
| `getNodeInfo` | `getNodeInfo(field)`, used by this module to read the provider's peer id |
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

- Edge protocols only: `createNode` fails when the config enables relay, because gossipsub needs a real `PubSub` mounted on a switch.
- discv5 stays off. It owns a UDP socket and the signature key, so use entry nodes and peer exchange. The library turns it off itself and logs that.
- Give a service peer explicitly. The delivery node keeps a peer store of its own that the provider never writes to, so peer selection sees only the peers it was told about.
- The delivery node still advertises its configured addresses, in its ENR and over peer exchange, and nothing listens on them. The provider's addresses are the reachable ones.
- No push events: an inbound stream waits in a queue on the provider side and the library polls it.
- Every stream read and write costs one call across the module boundary.
- A provider restart invalidates every stream id and subscription. Create the delivery node again.
- One delivery node per process may bridge. A second `createNode` with `libp2pProvider` fails instead of taking the provider over.
- The C ABI has no unregister, so the bridge lives for the process. A worker parked in a long poll can return after the delivery node is gone; it then finds the transport marked dead and answers with an error.
