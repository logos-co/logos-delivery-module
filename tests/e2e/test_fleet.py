from __future__ import annotations

import base64
import uuid

import pytest

from libs.constants import FLEET_ENTRY_NODES, FLEET_PUBSUB_TOPIC
from libs.helpers import (
    event_content_topic,
    event_payload,
    event_request_id,
    in_parallel,
    make_fleet_config,
    stored_messages,
    wait_for_connected,
    wait_for_event,
    wait_for_lez_wallet,
    wait_for_valid_roots,
    wait_until,
)

CONNECTED_TIMEOUT_S = 180.0
WALLET_TIMEOUT_S = 180.0
VALID_ROOTS_TIMEOUT_S = 180.0
PROPAGATED_TIMEOUT_S = 120.0
RECEIVED_TIMEOUT_S = 60.0
STORED_TIMEOUT_S = 60.0

RLN_VALID_MESSAGES = "logos_delivery_rln_valid_messages_total_total"

PAYLOAD = "Fleet round trip"

# Each sender starts from the keystore the previous one left, so one xdist worker runs them all.
pytestmark = [
    pytest.mark.rln,
    pytest.mark.rln_membership,
    pytest.mark.fleet,
    pytest.mark.xdist_group("fleet"),
]


def _content_topic() -> str:
    return f"/fleet-test/1/{uuid.uuid4().hex}/proto"


def _sender(fleet_node_factory, fleet_membership, label: str, *, mode: str):
    node = fleet_node_factory(label, make_fleet_config(mode=mode), membership=fleet_membership)
    wait_for_connected(node, timeout=CONNECTED_TIMEOUT_S)
    wait_for_lez_wallet(node, timeout=WALLET_TIMEOUT_S)
    return node


def _filter_client(fleet_node_factory, content_topic: str):
    node = fleet_node_factory("filter-client", make_fleet_config(mode="Edge"))
    wait_for_connected(node, timeout=CONNECTED_TIMEOUT_S)
    node.subscribe(content_topic)
    return node


def _validating_relay_node(fleet_node_factory, content_topic: str):
    node = fleet_node_factory("relay-node", make_fleet_config(mode="Core"), enable_validation=True)
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
    querier, sender = in_parallel(
        lambda: fleet_node_factory("store-client", make_fleet_config(mode="Edge")),
        lambda: _sender(fleet_node_factory, fleet_membership, "lightpush-client", mode="Edge"),
    )

    with sender.watch() as events:
        request_id = sender.send(content_topic, PAYLOAD)
        wait_for_event(
            events, "messagePropagated", timeout=PROPAGATED_TIMEOUT_S,
            predicate=lambda e: event_request_id(e) == request_id,
        )

    messages = wait_until(
        lambda: stored_messages(querier, content_topic, FLEET_ENTRY_NODES),
        timeout=STORED_TIMEOUT_S,
        interval=1,
        message="no fleet store node holds the message",
    )
    assert [base64.b64decode(m["message"]["payload"]) for m in messages] == [PAYLOAD.encode()]


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
        lambda: _validating_relay_node(fleet_node_factory, content_topic),
        lambda: _sender(fleet_node_factory, fleet_membership, "lightpush-client", mode="Edge"),
    )
    assert wait_for_valid_roots(receiver, sender, timeout=VALID_ROOTS_TIMEOUT_S) == "valid"

    with receiver.watch() as events:
        sender.send(content_topic, PAYLOAD)
        event = wait_for_event(events, "messageReceived", timeout=RECEIVED_TIMEOUT_S)

    assert (event_content_topic(event), event_payload(event)) == (content_topic, PAYLOAD.encode())
    assert receiver.metric(RLN_VALID_MESSAGES, shard=FLEET_PUBSUB_TOPIC) >= 1
