from __future__ import annotations

import time
import uuid

import pytest

from libs.constants import (
    FLEET_CONTENT_TOPIC_PREFIX,
    FLEET_PUBSUB_TOPIC,
    RLN_VALID_MESSAGES,
)
from libs.helpers import (
    event_content_topic,
    event_payload,
    event_request_id,
    in_parallel,
    make_fleet_config,
    wait_for_connected,
    wait_for_event,
    wait_for_lez_wallet,
    wait_for_valid_roots,
)

CONNECTED_TIMEOUT_S = 180.0
WALLET_TIMEOUT_S = 180.0
VALID_ROOTS_TIMEOUT_S = 180.0
SENT_TIMEOUT_S = 180.0
RECEIVED_TIMEOUT_S = 60.0
# An Edge node subscribes at the fleet 1 s after its `subscribe` returns.
FILTER_SUBSCRIBE_GRACE_S = 3.0

PAYLOAD = "Fleet round trip"

pytestmark = [
    pytest.mark.rln,
    pytest.mark.fleet,
    pytest.mark.xdist_group("fleet"),
    pytest.mark.timeout(1200),
]


def _content_topic() -> str:
    return f"{FLEET_CONTENT_TOPIC_PREFIX}{uuid.uuid4().hex}/proto"


def _sender(fleet_node_factory, fleet_membership, label: str, *, mode: str):
    node = fleet_node_factory(label, make_fleet_config(mode=mode), membership=fleet_membership)
    wait_for_connected(node, timeout=CONNECTED_TIMEOUT_S)
    wait_for_lez_wallet(node, timeout=WALLET_TIMEOUT_S)
    return node


def _filter_client(fleet_node_factory, content_topic: str):
    node = fleet_node_factory("filter-client", make_fleet_config(mode="Edge"))
    wait_for_connected(node, timeout=CONNECTED_TIMEOUT_S)
    node.subscribe(content_topic)
    time.sleep(FILTER_SUBSCRIBE_GRACE_S)
    return node


def _relay_node(fleet_node_factory, content_topic: str):
    node = fleet_node_factory("relay-node", make_fleet_config(mode="Core"))
    wait_for_connected(node, timeout=CONNECTED_TIMEOUT_S)
    node.subscribe(content_topic)
    return node


def test_lightpushed_message_reaches_filter_client(fleet_node_factory, fleet_membership):
    content_topic = _content_topic()
    receiver, sender = in_parallel(
        lambda: _filter_client(fleet_node_factory, content_topic),
        lambda: _sender(fleet_node_factory, fleet_membership, "lightpush-client", mode="Edge"),
    )

    with receiver.watch() as events:
        sender.send(content_topic, PAYLOAD)
        event = wait_for_event(events, "messageReceived", timeout=RECEIVED_TIMEOUT_S)

    assert (event_content_topic(event), event_payload(event)) == (content_topic, PAYLOAD.encode())


def test_lightpushed_message_is_stored(fleet_node_factory, fleet_membership):
    content_topic = _content_topic()
    sender = _sender(fleet_node_factory, fleet_membership, "lightpush-client", mode="Edge")

    with sender.watch() as events:
        request_id = sender.send(content_topic, PAYLOAD)
        # The preset turns reliability on, so `messageSent` waits for a store node to confirm the message.
        wait_for_event(
            events, "messageSent", timeout=SENT_TIMEOUT_S,
            predicate=lambda e: event_request_id(e) == request_id,
        )


def test_relayed_message_reaches_filter_client(fleet_node_factory, fleet_membership):
    content_topic = _content_topic()
    receiver, sender = in_parallel(
        lambda: _filter_client(fleet_node_factory, content_topic),
        lambda: _sender(fleet_node_factory, fleet_membership, "relay-node", mode="Core"),
    )
    sender.subscribe(content_topic)

    with receiver.watch() as events:
        sender.send(content_topic, PAYLOAD)
        event = wait_for_event(events, "messageReceived", timeout=RECEIVED_TIMEOUT_S)

    assert (event_content_topic(event), event_payload(event)) == (content_topic, PAYLOAD.encode())


def test_lightpushed_message_reaches_relay_node(fleet_node_factory, fleet_membership):
    content_topic = _content_topic()
    receiver, sender = in_parallel(
        lambda: _relay_node(fleet_node_factory, content_topic),
        lambda: _sender(fleet_node_factory, fleet_membership, "lightpush-client", mode="Edge"),
    )
    assert wait_for_valid_roots(receiver, sender, timeout=VALID_ROOTS_TIMEOUT_S) == "valid"

    with receiver.watch() as events:
        sender.send(content_topic, PAYLOAD)
        event = wait_for_event(events, "messageReceived", timeout=RECEIVED_TIMEOUT_S)

    assert (event_content_topic(event), event_payload(event)) == (content_topic, PAYLOAD.encode())
    assert receiver.metric(RLN_VALID_MESSAGES, shard=FLEET_PUBSUB_TOPIC) >= 1
