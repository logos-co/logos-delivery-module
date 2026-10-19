"""Drive `delivery_module` over the logoscore RPC client.

Unlike chat_module, delivery_module's public methods are SYNCHRONOUS: each
returns a StdLogosResult `{success, value, error}` that `client.call` hands back
directly — there are no per-call result events. Only message delivery is async,
surfacing as the typed events messageReceived / messageQueued /
messagePropagated / messageSent / messageError / connectionStateChanged.
"""

from __future__ import annotations

import json
import logging
import re
import subprocess
import time
from typing import Any, Optional

from logos_integration_test_framework import subscribe as _watch_events

from libs.constants import (
    CLUSTER_ID,
    NODE_TCP_PORT,
    NUM_SHARDS_IN_NETWORK,
    RLN_EPOCH_SIZE_SEC,
    RLN_REGISTRY_ID,
)

logger = logging.getLogger(__name__)

MODULE = "delivery_module"
RLN_MODULE = "liblogos_rln_module"

SUBSCRIBE_GRACE_S = 0.5

RLN_PRESETS_ENV = "LOGOS_DELIVERY_RLN_PRESETS"
RLN_PRESETS_FILE = "rln-presets.json"
RLN_READY_TIMEOUT_S = 120.0

# getConnectionStatus / connectionStateChanged values for a node with at least
# one relay peer.
CONNECTED_STATUSES = ("PartiallyConnected", "Connected")


def make_delivery_config(*, tcp_port: int = NODE_TCP_PORT, static_peers: Optional[list] = None) -> str:
    """JSON config for `createNode` — relay-only, single-shard, discovery-off,
    matching the two-node relay setup proven by logos-delivery-interop-tests."""
    cfg: dict[str, Any] = {
        "logLevel": "DEBUG",
        "listenAddress": "0.0.0.0",
        "tcpPort": tcp_port,
        "clusterId": CLUSTER_ID,
        "numShardsInNetwork": NUM_SHARDS_IN_NETWORK,
        "relay": True,
        "store": False,
        "filter": False,
        "lightpush": False,
        "peerExchange": False,
        "discv5Discovery": False,
        "reliabilityEnabled": True,
    }
    if static_peers:
        cfg["staticnodes"] = static_peers
    return json.dumps(cfg)


def make_rln_presets(*, enable_validation: bool) -> str:
    """Contents of the RLN presets file for a test node.

    delivery_module reads this file from the path in `LOGOS_DELIVERY_RLN_PRESETS` and
    lets it override its built-in RLN settings. The `""` entry applies to nodes created
    without a `preset`, like the ones in these tests. It turns RLN on, using the same
    LEZ testnet registry as the `logos.test` network, and sets whether the node checks
    the RLN proofs of incoming messages. The built-in `logos.test` settings always leave
    that check off, which is why the tests use this file instead.
    """
    return json.dumps({
        "": {
            "enabled": True,
            "registry-id": RLN_REGISTRY_ID,
            "epoch-size-sec": RLN_EPOCH_SIZE_SEC,
            "enable-validation": enable_validation,
        },
    })


def call_result(client, method: str, *args: Any, timeout: Optional[float] = None) -> dict:
    """Call a delivery method and return its StdLogosResult dict. No assertion."""
    result = client.call(MODULE, method, *args, timeout=timeout)
    if not isinstance(result, dict):
        raise AssertionError(f"{method} returned non-dict result: {result!r}")
    return result


def call_ok(client, method: str, *args: Any, timeout: Optional[float] = None) -> Any:
    """Call a delivery method, assert StdLogosResult.success, return its value."""
    result = call_result(client, method, *args, timeout=timeout)
    if not result.get("success"):
        raise AssertionError(f"{method}({args!r}) failed: {result.get('error')!r}")
    return result.get("value")


class LogosDelivery:
    """RPC-facing interface to delivery_module on one logoscore client."""

    def __init__(self, client):
        self.client = client

    def load(self):
        return self.client.load_module(MODULE)

    def create_node(self, config_json: str, *, timeout: Optional[float] = 90.0):
        return call_ok(self.client, "createNode", config_json, timeout=timeout)

    def create_node_result(self, config_json: str, *, timeout: Optional[float] = 90.0) -> dict:
        return call_result(self.client, "createNode", config_json, timeout=timeout)

    def start(self, *, timeout: Optional[float] = 90.0):
        return call_ok(self.client, "start", timeout=timeout)

    def stop(self, *, timeout: Optional[float] = 90.0):
        return call_ok(self.client, "stop", timeout=timeout)

    def subscribe(self, content_topic: str, *, timeout: Optional[float] = None):
        return call_ok(self.client, "subscribe", content_topic, timeout=timeout)

    def unsubscribe(self, content_topic: str, *, timeout: Optional[float] = None):
        return call_ok(self.client, "unsubscribe", content_topic, timeout=timeout)

    def send(self, content_topic: str, payload: str, *, timeout: Optional[float] = None):
        return call_ok(self.client, "send", content_topic, payload, timeout=timeout)

    def get_available_configs(self, *, timeout: Optional[float] = None):
        return call_ok(self.client, "getAvailableConfigs", timeout=timeout)

    def get_available_node_info_ids(self, *, timeout: Optional[float] = None):
        return call_ok(self.client, "getAvailableNodeInfoIDs", timeout=timeout)

    def get_node_info(self, info_id: str, *, timeout: Optional[float] = None):
        return call_ok(self.client, "getNodeInfo", info_id, timeout=timeout)

    def rln_state(self, *, timeout: Optional[float] = None) -> dict:
        return call_ok(self.client, "rlnState", timeout=timeout)

    def connection_status(self, *, timeout: Optional[float] = None) -> str:
        return call_ok(self.client, "getConnectionStatus", timeout=timeout)

    def metric(self, name: str) -> float:
        """Value of the unlabelled metric `name` in the node's Prometheus text, 0 if absent."""
        for line in (self.get_node_info("Metrics") or "").splitlines():
            metric_name, _, value = line.partition(" ")
            if metric_name == name:
                return float(value)
        return 0.0

    def watch(self, event_name: Optional[str] = None):
        return _watch_events(self.client, MODULE, event_name)


class DeliveryNode(LogosDelivery):
    def __init__(self, client, daemon, label: str):
        super().__init__(client)
        self.daemon = daemon
        self.label = label


def setup_delivery_node(daemon, config_json: str, label: str) -> DeliveryNode:
    """load_module → createNode → start. Returns a started node.

    createNode/start get a generous timeout: cold-starting liblogosdelivery can
    take tens of seconds, and the module itself waits up to 30s on the FFI callback.
    """
    node = DeliveryNode(daemon.client(), daemon, label)
    node.load()
    node.create_node(config_json)
    node.start()
    return node


def setup_rln_node(daemon, config_json: str, label: str) -> DeliveryNode:
    """`setup_delivery_node` for a node whose presets file turns RLN on: the RLN
    module is loaded before createNode, and RLN is `Ready` before start."""
    node = DeliveryNode(daemon.client(), daemon, label)
    node.client.load_module(RLN_MODULE)
    node.load()
    node.create_node(config_json)
    wait_for_rln_ready(node, timeout=RLN_READY_TIMEOUT_S)
    node.start()
    return node


def wait_until(condition, *, timeout: float, interval: float = 0.5, message: str = "") -> Any:
    """Call `condition` until it returns a truthy value, and return that value.
    Raises AssertionError with `message` after `timeout` seconds."""
    deadline = time.monotonic() + timeout
    while True:
        result = condition()
        if result:
            return result
        if time.monotonic() >= deadline:
            raise AssertionError(message or f"condition not met within {timeout}s")
        time.sleep(interval)


def wait_for_rln_ready(node: DeliveryNode, *, timeout: float) -> None:
    """Poll `rlnState` until it leaves `Initializing`; RLN comes up after
    createNode returns. Raises unless the state is `Ready`."""
    def settled():
        state = node.rln_state()
        return state if state.get("state") != "Initializing" else None

    state = wait_until(settled, timeout=timeout, message=f"{node.label}: RLN still Initializing")
    if state.get("state") != "Ready":
        raise AssertionError(f"{node.label}: rlnState {state!r}")


def is_connected(event: dict) -> bool:
    """A connectionStateChanged event reporting at least one relay peer."""
    return event_arg(event, 0) in CONNECTED_STATUSES


def node_multiaddr(node: DeliveryNode) -> str:
    """The node's libp2p multiaddr with the container IP substituted in.

    Multiaddrs advertises the configured listen address (0.0.0.0), which a
    peer can't dial; rewrite the /ip4/ segment to the container's address on the
    shared docker network so the peer reaches it container-to-container.
    """
    raw = node.get_node_info("Multiaddrs")
    addr = _first_multiaddr(raw)
    ip = _container_ip(node.daemon)
    routable = re.sub(r"/ip4/[^/]+/", f"/ip4/{ip}/", addr, count=1)
    if "/ip4/" not in routable:
        raise AssertionError(f"multiaddr has no /ip4/ segment to rewrite: {addr!r}")
    return routable


def _first_multiaddr(raw: str) -> str:
    addr = (raw or "").strip().strip("@[]")
    for part in re.split(r"[,\n]", addr):
        part = part.strip()
        if part.startswith("/"):
            return part
    raise AssertionError(f"no multiaddr in Multiaddrs: {raw!r}")


def _container_ip(daemon) -> str:
    fmt = '{{(index .NetworkSettings.Networks "' + str(daemon.network) + '").IPAddress}}'
    r = subprocess.run(
        ["docker", "inspect", "-f", fmt, daemon.container_id],
        capture_output=True, text=True,
    )
    ip = r.stdout.strip()
    if r.returncode != 0 or not ip:
        raise AssertionError(
            f"could not resolve container IP for {daemon.container_name}: {r.stderr.strip()!r}"
        )
    return ip


def wait_for_event(waiter, event_name: str, *, timeout: float, predicate=None):
    """Return the next event named `event_name` (subscribe() doesn't filter by
    event name, only by module, so re-filter here). Raises on timeout."""
    def _match(e):
        if e.get("event") != event_name:
            return False
        return predicate(e) if predicate else True

    return waiter.next(predicate=_match, timeout=timeout)


def parse_event(event: dict) -> dict:
    """The event's positional args as a dict {arg0, arg1, ...}.

    The logoscore CLI serializes a typed event as
    {"event": <name>, "data": {"arg0": <1st arg>, "arg1": <2nd arg>, ...}}
    (logos-logoscore-cli client.cpp watchModuleEvents), so each codegen event arg
    lands at argN by position. The vector<uint8_t> payload arg of messageReceived
    is the one whose serialization is still unverified.
    """
    data = event.get("data", {})
    return data if isinstance(data, dict) else {}


def event_arg(event: dict, index: int) -> Any:
    """Value of positional arg `index`, or None if absent."""
    return parse_event(event).get(f"arg{index}")


def event_request_id(event: dict) -> Optional[str]:
    """requestId of a messageSent/messagePropagated/messageError event (arg 0)."""
    rid = event_arg(event, 0)
    return str(rid) if rid is not None else None


def event_content_topic(event: dict) -> Optional[str]:
    """contentTopic of a messageReceived event (arg 1)."""
    ct = event_arg(event, 1)
    return str(ct) if ct is not None else None


def event_payload(event: dict) -> Any:
    """payload of a messageReceived event (arg 2)."""
    return event_arg(event, 2)


def event_source(event: dict) -> Optional[str]:
    """source of a messageReceived event (arg 3): "live" or "history"."""
    src = event_arg(event, 3)
    return str(src) if src is not None else None
