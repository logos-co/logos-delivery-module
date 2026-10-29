from __future__ import annotations

import pytest

from libs.constants import CONTENT_TOPIC
from libs.helpers import (
    event_request_id,
    is_connected,
    make_delivery_config,
    node_multiaddr,
    wait_for_event,
    wait_until,
)

CONNECTED_TIMEOUT_S = 60.0
RECEIVED_TIMEOUT_S = 20.0
REJECTED_TIMEOUT_S = 20.0
PROPAGATED_TIMEOUT_S = 40.0
SEND_ERROR_TIMEOUT_S = 120.0

VALIDATION_FAILURES = "libp2p_pubsub_validation_failure_total"


@pytest.mark.rln
def test_validating_node_drops_message_without_proof(rln_node_factory, delivery_node_factory):
    """The non-validating node receives the message; the validating one rejects it."""
    validating = rln_node_factory("validating", make_delivery_config(), enable_validation=True)
    non_validating = rln_node_factory("non-validating", make_delivery_config(), enable_validation=False)
    validating.subscribe(CONTENT_TOPIC)
    non_validating.subscribe(CONTENT_TOPIC)

    with non_validating.watch() as events, validating.watch() as validating_events:
        peers = [node_multiaddr(validating), node_multiaddr(non_validating)]
        sender = delivery_node_factory("plain-sender", make_delivery_config(static_peers=peers))
        wait_for_event(events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected)
        wait_for_event(
            validating_events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected
        )
        sender.send(CONTENT_TOPIC, "no-proof")
        wait_for_event(events, "messageReceived", timeout=RECEIVED_TIMEOUT_S)

    wait_until(
        lambda: validating.metric(VALIDATION_FAILURES) >= 1,
        timeout=REJECTED_TIMEOUT_S,
        message="validating node rejected no message",
    )


def test_send_with_rln_off_propagates(delivery_node_factory):
    sender = delivery_node_factory("plain-sender", make_delivery_config())
    with sender.watch() as events:
        delivery_node_factory("receiver", make_delivery_config(static_peers=[node_multiaddr(sender)]))
        wait_for_event(events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected)
        request_id = sender.send(CONTENT_TOPIC, "rln-off")
        event = wait_for_event(events, "messagePropagated", timeout=PROPAGATED_TIMEOUT_S)
    assert event_request_id(event) == request_id


@pytest.mark.rln
def test_send_without_membership_reports_error(rln_node_factory, delivery_node_factory):
    sender = rln_node_factory("rln-sender", make_delivery_config(), enable_validation=False)
    with sender.watch() as events:
        delivery_node_factory("receiver", make_delivery_config(static_peers=[node_multiaddr(sender)]))
        wait_for_event(events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected)
        request_id = sender.send(CONTENT_TOPIC, "no-membership")
        event = wait_for_event(events, "messageError", timeout=SEND_ERROR_TIMEOUT_S)
    assert event_request_id(event) == request_id
