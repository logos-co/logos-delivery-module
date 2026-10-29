"""Kademlia service discovery, plugin-hosted on libp2p_module (`pluginKadDiscovery`) or
in-process on the node's own switch (`enableKadDiscovery`): each test runs a libp2p_module
node as the DHT bootstrap on the test's docker network. A second node starts only once the
first one's advert is on the bootstrap, so its first lookups find it."""

from __future__ import annotations

import time

import pytest

from libs.constants import CLUSTER_ID, CONTENT_TOPIC, LOGOS_DELIVERY_SERVICE_ID, NODE_TCP_PORT
from libs.helpers import (
    LIBP2P_MODULE,
    NODE_STARTED_TIMEOUT_S,
    NODE_STOPPED_TIMEOUT_S,
    SUBSCRIBE_GRACE_S,
    DeliveryNode,
    container_ip,
    event_arg,
    event_payload,
    event_request_id,
    is_connected,
    make_delivery_config,
    node_multiaddr,
    self_advertisement_data,
    setup_libp2p_bootstrap,
    start_node,
    stop_node,
    wait_for_advert,
    wait_for_event,
    wait_until,
)

ADVERT_TIMEOUT_S = 30.0
CONNECTED_TIMEOUT_S = 60.0
LOOKUP_TIMEOUT_S = 30.0
RECEIVED_TIMEOUT_S = 20.0
# logos-delivery gives up a send after its 60 s retry time window.
SEND_ERROR_TIMEOUT_S = 90.0
# Longer than a test runs: a node given it looks up only in its first 8 s.
LONG_LOOKUP_INTERVAL_S = 600

# logos-delivery's `Capabilities.Relay` bit.
RELAY = 0b1

PEER_STORE_SIZE = "logos_delivery_peer_store_size"

pytestmark = pytest.mark.usefixtures("_libp2p_module_or_skip")


def _advert_of(node: DeliveryNode, libp2p_bootstrap) -> dict:
    """`node`'s `/logos/delivery` advert, once the bootstrap holds it."""
    return wait_for_advert(
        libp2p_bootstrap.client(), LOGOS_DELIVERY_SERVICE_ID, node.get_node_info("PeerId"), timeout=ADVERT_TIMEOUT_S
    )


def _connected_node(delivery_node_factory, label: str, config: str, peer: DeliveryNode) -> DeliveryNode:
    """A started node, once `peer` reports its first connection."""
    with peer.watch() as events:
        node = delivery_node_factory(label, config)
        wait_for_event(events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected)
    return node


def _assert_delivered(sender: DeliveryNode, receiver: DeliveryNode, payload: str) -> None:
    receiver.subscribe(CONTENT_TOPIC)
    with receiver.watch() as events:
        time.sleep(SUBSCRIBE_GRACE_S)
        sender.send(CONTENT_TOPIC, payload)
        event = wait_for_event(events, "messageReceived", timeout=RECEIVED_TIMEOUT_S)
    assert event_payload(event) == payload.encode()


def test_relay_node_advertises_through_libp2p_module(daemon_factory, delivery_node_factory):
    """Another libp2p_module node finds the relay node's own peer id, announced address,
    cluster, capabilities and shard."""
    libp2p_bootstrap = daemon_factory("libp2p-bootstrap")
    config = make_delivery_config(
        plugin_kad_discovery=True, kad_bootstrap_nodes=[setup_libp2p_bootstrap(libp2p_bootstrap)]
    )
    node = delivery_node_factory("A", config)

    record = _advert_of(node, libp2p_bootstrap)

    address = f"/ip4/{container_ip(node.daemon)}/tcp/{NODE_TCP_PORT}"
    assert address in record["addrs"], f"advert addrs {record['addrs']!r}"
    assert self_advertisement_data(record) == {
        "version": 1,
        "clusterId": int(CLUSTER_ID),
        "capabilities": RELAY,
        "shards": [0],
    }


def test_two_plugin_nodes_connect_without_static_peers(daemon_factory, delivery_node_factory):
    libp2p_bootstrap = daemon_factory("libp2p-bootstrap")
    config = make_delivery_config(
        plugin_kad_discovery=True, kad_bootstrap_nodes=[setup_libp2p_bootstrap(libp2p_bootstrap)]
    )
    node_a = delivery_node_factory("A", config)
    _advert_of(node_a, libp2p_bootstrap)

    node_b = _connected_node(delivery_node_factory, "B", config, node_a)

    _assert_delivered(node_a, node_b, "a-to-b")


def test_plugin_node_finds_in_process_node(daemon_factory, delivery_node_factory):
    libp2p_bootstrap = daemon_factory("libp2p-bootstrap")
    bootstrap = setup_libp2p_bootstrap(libp2p_bootstrap)
    in_process = delivery_node_factory(
        "in-process",
        make_delivery_config(
            enable_kad_discovery=True,
            kad_bootstrap_nodes=[bootstrap],
            kad_service_lookup_interval_sec=LONG_LOOKUP_INTERVAL_S,
        ),
    )
    _advert_of(in_process, libp2p_bootstrap)

    plugin = _connected_node(
        delivery_node_factory,
        "plugin",
        make_delivery_config(plugin_kad_discovery=True, kad_bootstrap_nodes=[bootstrap]),
        in_process,
    )

    _assert_delivered(in_process, plugin, "in-process-to-plugin")
    _assert_delivered(plugin, in_process, "plugin-to-in-process")


def test_in_process_node_finds_plugin_node(daemon_factory, delivery_node_factory):
    libp2p_bootstrap = daemon_factory("libp2p-bootstrap")
    bootstrap = setup_libp2p_bootstrap(libp2p_bootstrap)
    plugin = delivery_node_factory(
        "plugin",
        make_delivery_config(
            plugin_kad_discovery=True,
            kad_bootstrap_nodes=[bootstrap],
            kad_service_lookup_interval_sec=LONG_LOOKUP_INTERVAL_S,
        ),
    )
    _advert_of(plugin, libp2p_bootstrap)

    in_process = _connected_node(
        delivery_node_factory,
        "in-process",
        make_delivery_config(enable_kad_discovery=True, kad_bootstrap_nodes=[bootstrap]),
        plugin,
    )

    _assert_delivered(plugin, in_process, "plugin-to-in-process")
    _assert_delivered(in_process, plugin, "in-process-to-plugin")


def test_plugin_node_without_libp2p_module_fails_to_start(daemon_factory):
    daemon = daemon_factory("A", without_modules=(LIBP2P_MODULE,))
    node = DeliveryNode(daemon.client(), daemon, "A")
    node.load()
    node.create_node(make_delivery_config(plugin_kad_discovery=True))

    with node.watch() as events:
        time.sleep(SUBSCRIBE_GRACE_S)
        node.start()
        started = wait_for_event(events, "nodeStarted", timeout=NODE_STARTED_TIMEOUT_S)
        stopped = wait_for_event(events, "nodeStopped", timeout=NODE_STOPPED_TIMEOUT_S)

    assert event_arg(started, 0) is False
    assert "libp2p_module is not available" in event_arg(started, 1)
    assert event_arg(stopped, 0) is True


def test_edge_node_finds_lightpush_node_but_send_fails(daemon_factory, delivery_node_factory):
    """The Edge node's lookup adds the light push node to its peer store, but only the
    relay connectivity loop dials found peers, so the Edge node's send fails."""
    libp2p_bootstrap = daemon_factory("libp2p-bootstrap")
    bootstrap = setup_libp2p_bootstrap(libp2p_bootstrap)
    # The light push node relays to it, so a push from the Edge node would succeed.
    receiver = delivery_node_factory("receiver", make_delivery_config())
    service = _connected_node(
        delivery_node_factory,
        "lightpush-service",
        make_delivery_config(
            lightpush=True,
            static_peers=[node_multiaddr(receiver)],
            plugin_kad_discovery=True,
            kad_bootstrap_nodes=[bootstrap],
        ),
        receiver,
    )
    _advert_of(service, libp2p_bootstrap)
    edge = delivery_node_factory(
        "edge", make_delivery_config(relay=False, plugin_kad_discovery=True, kad_bootstrap_nodes=[bootstrap])
    )
    wait_until(
        lambda: edge.metric(PEER_STORE_SIZE) == 1,
        timeout=LOOKUP_TIMEOUT_S,
        message="edge: lookup added no peer",
    )

    with edge.watch() as events:
        time.sleep(SUBSCRIBE_GRACE_S)
        request_id = edge.send(CONTENT_TOPIC, "edge")
        event = wait_for_event(events, "messageError", timeout=SEND_ERROR_TIMEOUT_S)

    assert (event_request_id(event), event_arg(event, 2)) == (
        request_id,
        "Unable to send within retry time window",
    )


def test_restarted_node_reconnects_while_old_advert_is_served(daemon_factory, delivery_node_factory):
    """A restarts and B reconnects to it; the bootstrap still returns the advert A signed
    before the restart."""
    libp2p_bootstrap = daemon_factory("libp2p-bootstrap")
    config = make_delivery_config(
        plugin_kad_discovery=True, kad_bootstrap_nodes=[setup_libp2p_bootstrap(libp2p_bootstrap)]
    )
    node_a = delivery_node_factory("A", config)
    advert = _advert_of(node_a, libp2p_bootstrap)
    node_b = _connected_node(delivery_node_factory, "B", config, node_a)

    with node_b.watch() as events:
        time.sleep(SUBSCRIBE_GRACE_S)
        stop_node(node_a)
        wait_for_event(
            events,
            "connectionStateChanged",
            timeout=CONNECTED_TIMEOUT_S,
            predicate=lambda event: not is_connected(event),
        )
        start_node(node_a)
        wait_for_event(events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected)

    assert _advert_of(node_a, libp2p_bootstrap)["seqNo"] == advert["seqNo"]
    _assert_delivered(node_a, node_b, "after-restart")
