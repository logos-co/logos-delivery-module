# Run a delivery node

Runs a delivery node (`logoscore` daemon + `delivery_module`) in Docker.
There is no GUI or HTTP API — interaction is via the `logoscore` CLI.

## Prerequisites

- Docker with Compose

## Start

```bash
git clone https://github.com/logos-co/logos-delivery-module.git
cd logos-delivery-module
docker compose up -d --build
```

First build runs Nix and downloads release packages — allow ~30–45 min.
Later starts are fast.

## Boot the node

The daemon is running; load the module and start the node:

```bash
docker exec logos-node logoscore load-module delivery_module --json
docker exec logos-node logoscore call delivery_module createNode @/conf/logos-test.json --json
docker exec logos-node logoscore call delivery_module start --json
```

Verify:

```bash
docker exec logos-node logoscore status --json
```

The node is now connected to the `logos.test` network. See
[`query-node.md`](./query-node.md) to read its peer ID, ENR, and metrics.

## Stop

```bash
docker compose down
```

## Configuration

The node config is [`conf/logos-test.json`](../conf/logos-test.json), mounted
into the container at `/conf`. It uses the `logos.test` network preset. Edit it
and re-run the boot steps to change settings; available keys are documented in
the [README](../README.md#node-configuration-createnode).
