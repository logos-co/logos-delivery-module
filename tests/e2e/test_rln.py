from __future__ import annotations

import pytest

from libs.constants import CONTENT_TOPIC, PUBSUB_TOPIC, RLN_VALID_MESSAGES
from libs.helpers import (
    event_request_id,
    is_connected,
    make_delivery_config,
    node_multiaddr,
    register_membership,
    rln_membership_state,
    wait_for_event,
    wait_for_valid_roots,
    wait_until,
)

CONNECTED_TIMEOUT_S = 10.0
RECEIVED_TIMEOUT_S = 10.0
VALID_ROOTS_TIMEOUT_S = 30.0
WALLET_TIMEOUT_S = 45.0
REJECTED_TIMEOUT_S = 5.0
PROPAGATED_TIMEOUT_S = 10.0
# TODO: logos-messaging/logos-delivery#4403
SEND_ERROR_TIMEOUT_S = 75.0
RLN_MEMBERSHIP_TIMEOUT_S = 45.0

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


@pytest.mark.rln
def test_node_registers_membership(rln_node_factory, lez_registry):
    """The RLN module waits for its payer to be funded, then registers a membership by itself."""
    node = rln_node_factory("membership", make_delivery_config(), enable_validation=False)

    def settled():
        state = rln_membership_state(node)
        return state if state.get("provisioning", {}).get("step") in ("awaiting_funding", "refused") else None

    state = wait_until(settled, timeout=WALLET_TIMEOUT_S, message="membership never awaited funding")
    if state["provisioning"]["step"] != "awaiting_funding":
        raise AssertionError(f"membership: {state!r}")
    register_membership(node, lez_registry.fund, timeout=RLN_MEMBERSHIP_TIMEOUT_S)


@pytest.mark.rln
def test_validating_node_receives_message_with_proof(rln_node_factory, lez_registry):
    validating = rln_node_factory("validating", make_delivery_config(), enable_validation=True)
    validating.subscribe(CONTENT_TOPIC)

    with validating.watch() as events:
        sender = rln_node_factory(
            "sender-with-membership",
            make_delivery_config(static_peers=[node_multiaddr(validating)]),
            enable_validation=False,
        )
        register_membership(sender, lez_registry.fund, timeout=RLN_MEMBERSHIP_TIMEOUT_S)
        assert wait_for_valid_roots(validating, sender, timeout=VALID_ROOTS_TIMEOUT_S) == "valid"
        wait_for_event(events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected)
        sender.send(CONTENT_TOPIC, "with-proof")
        wait_for_event(events, "messageReceived", timeout=RECEIVED_TIMEOUT_S)

    assert validating.metric(RLN_VALID_MESSAGES, shard=PUBSUB_TOPIC) >= 1


@pytest.mark.rln
def test_validating_service_relays_lightpush_with_proof(rln_node_factory, delivery_node_factory, lez_registry):
    receiver = delivery_node_factory("receiver", make_delivery_config())
    receiver.subscribe(CONTENT_TOPIC)

    with receiver.watch() as events:
        service = rln_node_factory(
            "lightpush-service",
            make_delivery_config(lightpush=True, static_peers=[node_multiaddr(receiver)]),
            enable_validation=True,
        )
        wait_for_event(events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected)
        client = rln_node_factory(
            "lightpush-client",
            make_delivery_config(relay=False, lightpush_node=node_multiaddr(service)),
            enable_validation=False,
        )
        register_membership(client, lez_registry.fund, timeout=RLN_MEMBERSHIP_TIMEOUT_S)
        assert wait_for_valid_roots(service, client, timeout=VALID_ROOTS_TIMEOUT_S) == "valid"
        client.send(CONTENT_TOPIC, "lightpush-with-proof")
        wait_for_event(events, "messageReceived", timeout=RECEIVED_TIMEOUT_S)

    assert service.metric(RLN_VALID_MESSAGES, shard=PUBSUB_TOPIC) >= 1


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
