"""Networking + waku-cluster constants for the two-daemon e2e fixtures."""

from __future__ import annotations

import os

NETWORK_SUBNET = os.environ.get("E2E_NETWORK_SUBNET", "172.31.0.0/16")

CLUSTER_ID = "198"
NUM_SHARDS_IN_NETWORK = 1

NODE_TCP_PORT = 60000
LIBP2P_TCP_PORT = 60001

# The service every delivery service node advertises on kademlia service discovery.
LOGOS_DELIVERY_SERVICE_ID = "/logos/delivery"

CONTENT_TOPIC = "/test/1/logos-delivery-e2e/proto"
# The network's only shard, so every content topic maps to it.
PUBSUB_TOPIC = f"/waku/2/rs/{CLUSTER_ID}/0"

# The local LEZ sequencer's JSON-RPC port.
LEZ_SEQUENCER_PORT = 3040
# LEZ's CLOCK_50 account and its clock shard (lez/programs/clock/core), with the
# timestamp in ms at bytes 8-16, 0 until block 50.
LEZ_CLOCK_50_ACCOUNT = "4BdcjoXkq786TMWcBGGHqcxeLYMZmn17rL4eM9ZyRWkX"
LEZ_CLOCK_SHARD = "2hwaRW8sMnyKLLRbDRw8k68tiv9bnfq98oxcdtcp157Z"
# The LEZ testnet's sequencer.
LEZ_SEQUENCER_URL = "https://testnet.lez.logos.co/"

RLN_EPOCH_SIZE_SEC = 600
# delivery_module's RLN identifier, sha256("rln/logos-delivery/v0.0.1").
RLN_IDENTIFIER = "5e269b6a19fce081f5808b13442dcbc3522197638dd38df5a28bc4e55236b977"
RLN_VALID_MESSAGES = "logos_delivery_rln_valid_messages_total_total"

FLEET_PRESET = "logos.test"
# The `logos.test` preset's registry on the LEZ testnet (docs/pages/rln.md).
FLEET_REGISTRY_ID = "logos:testnet:fd322d32a46ddf57eece9db8d68e9b722262d1f770760fca63b8cb9e7775b971"
FLEET_CONTENT_TOPIC_PREFIX = "/fleet-test/1/"
# Autosharding maps FLEET_CONTENT_TOPIC_PREFIX to shard 6.
FLEET_PUBSUB_TOPIC = "/waku/2/rs/2/6"
