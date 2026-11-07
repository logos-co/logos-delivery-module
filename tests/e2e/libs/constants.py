"""Networking + waku-cluster constants for the two-daemon e2e fixtures."""

from __future__ import annotations

import os

NETWORK_SUBNET = os.environ.get("E2E_NETWORK_SUBNET", "172.31.0.0/16")

CLUSTER_ID = "198"
NUM_SHARDS_IN_NETWORK = 1

NODE_TCP_PORT = 60000

CONTENT_TOPIC = "/test/1/logos-delivery-e2e/proto"

# The `logos.test` preset's registry on the LEZ testnet (docs/pages/rln.md).
RLN_REGISTRY_ID = "logos:testnet:fd322d32a46ddf57eece9db8d68e9b722262d1f770760fca63b8cb9e7775b971"
RLN_EPOCH_SIZE_SEC = 600
# A second registry at native Mix's profile (10 s epoch, gap 3) beside the
# relay one; the testnet has none, so the mix tests run only where one is
# named.
RLN_MIX_REGISTRY_ID = os.environ.get("RLN_MIX_REGISTRY_ID", "")
RLN_MIX_METADATA_TOPIC = "/mix/1/metadata/proto"
# A membership allocator the deployment runs (peer id + multiaddr); the
# allocator tests run only where one is named.
RLN_ALLOCATOR_PEER_ID = os.environ.get("RLN_ALLOCATOR_PEER_ID", "")
RLN_ALLOCATOR_MULTIADDR = os.environ.get("RLN_ALLOCATOR_MULTIADDR", "")
