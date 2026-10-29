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

# The local LEZ sequencer's JSON-RPC port, and LEZ's CLOCK_50 account with the clock
# program's shard in it (logos-execution-zone lez/programs/clock/core): bytes 8-16 hold
# the timestamp in ms, 0 until block 50.
LEZ_SEQUENCER_PORT = 3040
LEZ_CLOCK_50_ACCOUNT = "4BdcjoXkq786TMWcBGGHqcxeLYMZmn17rL4eM9ZyRWkX"
LEZ_CLOCK_SHARD = "2hwaRW8sMnyKLLRbDRw8k68tiv9bnfq98oxcdtcp157Z"
# The LEZ testnet's sequencer.
LEZ_SEQUENCER_URL = "https://testnet.lez.logos.co/"

RLN_EPOCH_SIZE_SEC = 600
# delivery_module's RLN identifier, sha256("rln/logos-delivery/v0.0.1") (docs/pages/rln.md).
RLN_IDENTIFIER = "5e269b6a19fce081f5808b13442dcbc3522197638dd38df5a28bc4e55236b977"
