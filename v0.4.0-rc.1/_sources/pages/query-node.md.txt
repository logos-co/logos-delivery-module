# Query a running node

A delivery node exposes no HTTP/REST API. All node info — identity, version,
and Prometheus metrics — is read through the `logoscore` CLI talking to the
daemon. Assumes a node started per [`run-node.md`](./run-node.md).

Run the commands inside the container (`docker exec logos-node ...`) or
directly if you have `logoscore` on the host.

## List available info

```bash
logoscore call delivery_module getAvailableNodeInfoIDs --json | jq
```

```json
{
  "method": "getAvailableNodeInfoIDs",
  "module": "delivery_module",
  "result": {
    "error": null,
    "success": true,
    "value": "[\"Version\",\"Metrics\",\"Multiaddrs\",\"ENR\",\"PeerId\"]"
  },
  "status": "ok"
}
```

## Read a specific value

```bash
logoscore call delivery_module getNodeInfo ENR --json | jq
```

```json
{
  "method": "getNodeInfo",
  "module": "delivery_module",
  "result": {
    "error": null,
    "success": true,
    "value": "enr:-LW4QItc5tHj3rWoFaaQIUWvaBYijDf2TJKW83SNdyylJVAYVoUlBl1h5..."
  },
  "status": "ok"
}
```

The value lives at `.result.value`. Extract it with `jq -r`:

```bash
logoscore call delivery_module getNodeInfo PeerId --json | jq -r '.result.value'
```

## Info IDs

| ID                 | Command                        | Returns                       |
| ------------------ | ------------------------------ | ----------------------------- |
| `PeerId`           | `getNodeInfo PeerId`           | libp2p peer ID                |
| `ENR`              | `getNodeInfo ENR`              | discv5 ENR URI                |
| `Multiaddrs`       | `getNodeInfo Multiaddrs`       | Listen multiaddresses         |
| `Metrics`          | `getNodeInfo Metrics`          | Prometheus text exposition    |
| `Version`          | `getNodeInfo Version`          | `liblogosdelivery` version    |
| `IsRunning`        | `getNodeInfo IsRunning`        | `true` once started, else `false` |

## Connection status

```bash
logoscore call delivery_module getConnectionStatus --json | jq -r '.result.value'
```

Returns `Disconnected`, `PartiallyConnected` or `Connected`, the same values
the `connectionStateChanged` event carries. The event fires on transitions
only; this reads the current status.

The `logoscore` binary version is in `logoscore status --json` →
`.daemon.version`.
