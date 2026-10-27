"""Kademlia service discovery hosted on libp2p_module (`pluginKadDiscovery`): each
test runs a libp2p_module node as the DHT bootstrap on the test's docker network."""

from __future__ import annotations

import pytest

from libs.constants import CLUSTER_ID, LOGOS_DELIVERY_SERVICE_ID, NODE_TCP_PORT
from libs.helpers import (
    container_ip,
    make_delivery_config,
    self_advertisement_data,
    setup_kad_bootstrap,
    wait_for_advert,
)

ADVERT_TIMEOUT_S = 30.0

# logos-delivery's `Capabilities.Relay` bit.
RELAY = 0b1

pytestmark = pytest.mark.usefixtures("_libp2p_module_or_skip")


def test_relay_node_advertises_through_libp2p_module(daemon_factory, delivery_node_factory):
    """Another libp2p_module node finds the relay node's own peer id, announced address,
    cluster, capabilities and shard."""
    bootstrap = daemon_factory("bootstrap")
    config = make_delivery_config(plugin_kad_discovery=True, kad_bootstrap_nodes=[setup_kad_bootstrap(bootstrap)])
    node = delivery_node_factory("A", config)

    record = wait_for_advert(
        bootstrap.client(), LOGOS_DELIVERY_SERVICE_ID, node.get_node_info("PeerId"), timeout=ADVERT_TIMEOUT_S
    )

    address = f"/ip4/{container_ip(node.daemon)}/tcp/{NODE_TCP_PORT}"
    assert address in record["addrs"], f"advert addrs {record['addrs']!r}"
    assert self_advertisement_data(record) == {
        "version": 1,
        "clusterId": int(CLUSTER_ID),
        "capabilities": RELAY,
        "shards": [0],
    }
