"""Two-daemon e2e: real message delivery between two delivery nodes peered
directly via staticnodes (node A's container-routable multiaddr).

Gate = the sender observes `messagePropagated` for its send — the signal proven
by logos-delivery-interop-tests (relay-only, no store ⇒ no messageSent). The
receiver-side `messageReceived` path is exercised too. See README → "Event payload shape".
"""

from __future__ import annotations

import logging
import time

import pytest

from libs.constants import CONTENT_TOPIC
from libs.helpers import (
    SUBSCRIBE_GRACE_S,
    DeliveryNode,
    event_content_topic,
    event_payload,
    event_request_id,
    event_source,
    is_connected,
    make_delivery_config,
    node_multiaddr,
    parse_event,
    wait_for_event,
)

logger = logging.getLogger(__name__)

CONNECTED_TIMEOUT_S = 60.0
PROPAGATED_TIMEOUT_S = 40.0
RECEIVED_TIMEOUT_S = 20.0

pytestmark = pytest.mark.two_node


def _peered_nodes(delivery_node_factory) -> tuple[DeliveryNode, DeliveryNode]:
    """Node A, and node B dialing it through `staticnodes`, once A reports the connection."""
    node_a = delivery_node_factory("A", make_delivery_config())
    with node_a.watch() as events:
        node_b = delivery_node_factory("B", make_delivery_config(static_peers=[node_multiaddr(node_a)]))
        wait_for_event(events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected)
    return node_a, node_b


def test_two_nodes_propagation(delivery_node_factory):
    """Gate: node A publishes; A observes messagePropagated for its requestId.

    A is the dialee (B dialed it via staticnodes), mirroring interop S06 where the
    sender is the node the peer connects to.
    """
    node_a, node_b = _peered_nodes(delivery_node_factory)
    node_b.subscribe(CONTENT_TOPIC)

    with node_a.watch("messagePropagated") as w:
        time.sleep(SUBSCRIBE_GRACE_S)
        request_id = node_a.send(CONTENT_TOPIC, "hello-propagation")
        assert request_id, "send returned an empty requestId"
        event = wait_for_event(w, "messagePropagated", timeout=PROPAGATED_TIMEOUT_S)

    logger.info("messagePropagated payload: %r", parse_event(event))
    rid = event_request_id(event)
    if rid is not None:
        assert rid == request_id, f"messagePropagated requestId {rid!r} != sent {request_id!r}"


def test_bidirectional_propagation(delivery_node_factory):
    """Both directions propagate: A→B and B→A."""
    node_a, node_b = _peered_nodes(delivery_node_factory)
    node_a.subscribe(CONTENT_TOPIC)
    node_b.subscribe(CONTENT_TOPIC)

    for sender, content in ((node_a, "a-to-b"), (node_b, "b-to-a")):
        with sender.watch("messagePropagated") as w:
            time.sleep(SUBSCRIBE_GRACE_S)
            request_id = sender.send(CONTENT_TOPIC, content)
            assert request_id, f"send from {sender.label} returned an empty requestId"
            event = wait_for_event(w, "messagePropagated", timeout=PROPAGATED_TIMEOUT_S)
        rid = event_request_id(event)
        if rid is not None:
            assert rid == request_id, f"{sender.label}: propagated requestId {rid!r} != sent {request_id!r}"


def test_two_nodes_message_received(delivery_node_factory):
    """Node A subscribes; B publishes; A observes messageReceived for the topic."""
    node_a, node_b = _peered_nodes(delivery_node_factory)
    node_a.subscribe(CONTENT_TOPIC)
    node_b.subscribe(CONTENT_TOPIC)

    with node_a.watch("messageReceived") as w:
        time.sleep(SUBSCRIBE_GRACE_S)
        request_id = node_b.send(CONTENT_TOPIC, "hello-receive")
        assert request_id, "send returned an empty requestId"
        event = wait_for_event(w, "messageReceived", timeout=RECEIVED_TIMEOUT_S)

    logger.info("messageReceived payload: %r", parse_event(event))
    topic = event_content_topic(event)
    if topic is not None:
        assert topic == CONTENT_TOPIC, f"messageReceived contentTopic {topic!r} != {CONTENT_TOPIC!r}"

    # A decoder that disagrees with the library's wire format yields empty bytes
    # rather than an error, so assert the payload survived the trip.
    payload = event_payload(event)
    assert payload, f"messageReceived carried an empty payload: {parse_event(event)!r}"

    source = event_source(event)
    if source is not None:
        assert source == "live", f"messageReceived source {source!r} != 'live'"
