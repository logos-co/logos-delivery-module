# Networks: `logos.dev` vs `logos.test`

Two hosted networks are available to a delivery node. Pick one with the
`preset` key in the `createNode` config:

```json
{ "mode": "Core", "preset": "logos.test" }
```

The repo ships a ready config for each — [`conf/logos-test.json`](../../conf/logos-test.json)
and [`conf/logos-dev.json`](../../conf/logos-dev.json). See
[`run-node.md`](./run-node.md) for how to boot with one.

## Which to use

**`logos.test` unless you have a reason not to.** It is the stable
testnet-candidate fleet and the target for application development. It is the
default throughout this repo's docs and the shipped Docker setup.

**`logos.dev` is the bleeding-edge integration fleet.** It is redeployed
freely and may break at any time. Use it when you need to exercise unreleased
delivery changes, not to build against.

The two are separate clusters, so nodes on one do not see nodes on the other.

## The presets

| | `logos.dev` | `logos.test` |
|---|---|---|
| Cluster ID | **3** | **2** |
| Entry nodes | `delivery-0{1,2}.<dc>.logos.dev.status.im` | `node-0{1,2}.<dc>.logos.test.status.im` |
| Sharding | auto, 8 shards | auto, 8 shards |
| Max message size | 150 KiB | 150 KiB |
| RLN | off | off |
| Mix routing | on | on |
| P2P reliability | on | on |
| discv5 | on | on |
| Kademlia discovery | on | on |

Cluster ID and entry nodes are the only differences; every other preset
parameter is identical.

Six entry nodes each, two per data centre, across `do-ams3`,
`gc-us-central1-a` and `ac-cn-hongkong-c`.

Both preset names also accept a dotless spelling (`logosdev`, `logostest`).
The other two presets — `twn` (The Waku Network, cluster 1, RLN on) and
`status.prod` (cluster 16, 1 shard) — are separate networks and out of scope
here.

> `logos.dev` moved from cluster 2 to cluster 3 in logos-delivery
> [#4113](https://github.com/logos-messaging/logos-delivery/pull/4113). A build
> pinned to a delivery revision older than that still resolves the preset to
> cluster 2 and will not reach the fleet.

## The fleets

Both fleets live in [`status-im/infra-logos`](https://github.com/status-im/infra-logos)
as Terraform workspaces (`dev` and `test`) and run the same delivery module
version, bumped together.

**`logos.dev`** hosts run the delivery module *only*. They sit on smaller
instances (2 vCPU / 4 GB) alongside the rest of the dev-stage estate —
blockchain, storage and chat hosts that are provisioned separately.

**`logos.test`** hosts run a fuller node: the delivery module *and* the storage
(Codex) module, with four of them acting as mix-proxy relays. They sit on
larger instances (4 vCPU / 8 GB). A blockchain module is configured but
currently disabled upstream.

Beyond that the node configuration matches: relay, store, filter, lightpush and
mix protocols enabled, discv5 plus Kademlia discovery, and a Postgres-backed
store with a `time:259200;size:50GB` retention policy.
