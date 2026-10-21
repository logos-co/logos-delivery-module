"""Networking + waku-cluster constants for the two-daemon e2e fixtures."""

from __future__ import annotations

import os

NETWORK_SUBNET = os.environ.get("E2E_NETWORK_SUBNET", "172.31.0.0/16")

CLUSTER_ID = "198"
NUM_SHARDS_IN_NETWORK = 1

NODE_TCP_PORT = 60000

CONTENT_TOPIC = "/test/1/logos-delivery-e2e/proto"
# The network's one shard, which every content topic maps to.
PUBSUB_TOPIC = f"/waku/2/rs/{CLUSTER_ID}/0"

# The `logos.test` preset's registry on the LEZ testnet (docs/pages/rln.md).
RLN_REGISTRY_ID = "logos:testnet:fd322d32a46ddf57eece9db8d68e9b722262d1f770760fca63b8cb9e7775b971"
RLN_EPOCH_SIZE_SEC = 600
# delivery_module's RLN identifier, sha256("rln/logos-delivery/v0.0.1") (docs/pages/rln.md).
RLN_IDENTIFIER = "5e269b6a19fce081f5808b13442dcbc3522197638dd38df5a28bc4e55236b977"
