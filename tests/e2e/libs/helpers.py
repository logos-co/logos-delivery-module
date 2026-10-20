"""Drive `delivery_module` over the logosctl RPC client.

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
import urllib.request
from collections.abc import Callable
from typing import Any, Optional

from logos_integration_test_framework import subscribe as _watch_events

from libs.constants import (
    CLUSTER_ID,
    LEZ_CLOCK_50_ACCOUNT,
    LEZ_CLOCK_SHARD,
    NODE_TCP_PORT,
    NUM_SHARDS_IN_NETWORK,
    RLN_EPOCH_SIZE_SEC,
    RLN_IDENTIFIER,
)

logger = logging.getLogger(__name__)

MODULE = "delivery_module"
RLN_MODULE = "liblogos_rln_module"
LEZ_RLN_MODULE = "liblogos_lez_rln_module"

SUBSCRIBE_GRACE_S = 0.5

RLN_PRESETS_ENV = "LOGOS_DELIVERY_RLN_PRESETS"
RLN_PRESETS_FILE = "rln-presets.json"
RLN_READY_TIMEOUT_S = 30.0
NODE_STARTED_TIMEOUT_S = 90.0
RLN_MEMBERSHIP_TIMEOUT_S = 180.0
LEZ_SEQUENCER_ENV = "LEZ_RLN_SEQUENCER"
LEZ_RPC_TIMEOUT_S = 10.0
BASE58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
# liblogos_rln_module error classes that clear up by themselves, e.g. while the LEZ
# wallet syncs or the valid-root window fills.
RLN_RETRY_CLASSES = ("not_ready", "transient")
RLN_PROBE_SIGNAL = "0x" + b"valid-roots-probe".hex()

CONNECTED_STATUSES = ("PartiallyConnected", "Connected")


def make_delivery_config(
    *,
    tcp_port: int = NODE_TCP_PORT,
    static_peers: Optional[list] = None,
    relay: bool = True,
    lightpush: bool = False,
    lightpush_node: Optional[str] = None,
) -> str:
    """JSON config for `createNode` — relay-only, single-shard, discovery-off,
    matching the two-node relay setup proven by logos-delivery-interop-tests.
    `lightpush` serves light push; a node without relay sends through
    `lightpush_node`."""
    cfg: dict[str, Any] = {
        "logLevel": "DEBUG",
        "listenAddress": "0.0.0.0",
        "tcpPort": tcp_port,
        "clusterId": CLUSTER_ID,
        "numShardsInNetwork": NUM_SHARDS_IN_NETWORK,
        "relay": relay,
        "store": False,
        "filter": False,
        "lightpush": lightpush,
        "peerExchange": False,
        "discv5Discovery": False,
        "reliabilityEnabled": True,
        "nat": "none",
    }
    if static_peers:
        cfg["staticnodes"] = static_peers
    if lightpush_node:
        cfg["lightpushnode"] = lightpush_node
    return json.dumps(cfg)


def make_rln_presets(*, enable_validation: bool, registry_id: str) -> str:
    """Contents of the RLN presets file for a test node: its `""` entry applies to
    nodes created without a `preset`."""
    return json.dumps({
        "": {
            "enabled": True,
            "registry-id": registry_id,
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
    """RPC-facing interface to delivery_module on one logosctl client."""

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

    def metric(self, name: str, **labels: str) -> float:
        """Value of the metric `name` with `labels` in the node's Prometheus text, 0 if absent."""
        label_text = ",".join(f'{key}="{value}"' for key, value in labels.items())
        series = f"{name}{{{label_text}}}" if labels else name
        for line in (self.get_node_info("Metrics") or "").splitlines():
            metric_name, _, value = line.partition(" ")
            if metric_name == series:
                return float(value)
        return 0.0

    def watch(self, event_name: Optional[str] = None):
        return _watch_events(self.client, MODULE, event_name)


class DeliveryNode(LogosDelivery):
    def __init__(self, client, daemon, label: str, registry_id: Optional[str] = None):
        super().__init__(client)
        self.daemon = daemon
        self.label = label
        self.registry_id = registry_id


def setup_delivery_node(daemon, config_json: str, label: str) -> DeliveryNode:
    """load_module → createNode → start. Returns a started node.

    createNode/start get a generous timeout: cold-starting liblogosdelivery can
    take tens of seconds, and the module itself waits up to 30s on the FFI callback.
    """
    node = DeliveryNode(daemon.client(), daemon, label)
    node.load()
    node.create_node(config_json)
    start_node(node)
    return node


def setup_rln_node(daemon, config_json: str, label: str, registry_id: str) -> DeliveryNode:
    """`setup_delivery_node` for a node whose presets file turns RLN on for
    `registry_id`: the RLN module is loaded before createNode, and RLN is `Ready`
    before start."""
    node = DeliveryNode(daemon.client(), daemon, label, registry_id)
    node.client.load_module(RLN_MODULE)
    node.load()
    node.create_node(config_json)
    wait_for_rln_ready(node, timeout=RLN_READY_TIMEOUT_S)
    start_node(node)
    return node


def start_node(node: DeliveryNode) -> None:
    """`start` returns once the request is dispatched; wait for its `nodeStarted`.
    Raises unless the node started."""
    with node.watch() as events:
        time.sleep(SUBSCRIBE_GRACE_S)
        node.start()
        event = wait_for_event(events, "nodeStarted", timeout=NODE_STARTED_TIMEOUT_S)
    if event_arg(event, 0) is not True:
        raise AssertionError(f"{node.label}: start failed: {event_arg(event, 1)!r}")


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


def wait_for_lez_wallet(node: DeliveryNode, *, timeout: float) -> None:
    """Poll the node's LEZ wallet until its first sync is over; until then every
    chain read of the RLN modules blocks or answers empty. Raises unless the
    wallet is `ready`."""
    def settled():
        status = _from_json_text(node.client.call(LEZ_RLN_MODULE, "wallet_status"))
        return status if status.get("state") != "pending" else None

    status = wait_until(settled, timeout=timeout, message=f"{node.label}: LEZ wallet still pending")
    if status.get("state") != "ready":
        raise AssertionError(f"{node.label}: LEZ wallet {status!r}")


def wait_for_valid_roots(node: DeliveryNode, prover: DeliveryNode, *, timeout: float) -> str:
    """Poll `node`'s `validate_proof` with a proof from `prover`'s membership until
    its valid-root window holds the proof's root, and return the verdict. The window
    fills in the background and nothing else reports it; a warm window answers
    `invalid` until it holds the root. The proof spends one of `prover`'s messages in
    the current epoch."""
    wait_for_lez_wallet(prover, timeout=timeout)
    timestamp = str(int(time.time()))
    proof = wait_until(
        lambda: _rln_answer(prover, "generate_proof", RLN_PROBE_SIGNAL, timestamp),
        timeout=timeout,
        message=f"{prover.label}: no RLN proof",
    )
    proof_json = json.dumps({"proof": proof["proof_canonical"]})

    def settled():
        answer = _rln_answer(node, "validate_proof", RLN_PROBE_SIGNAL, timestamp, proof_json)
        return answer if answer and answer["verdict"] != "invalid" else None

    answer = wait_until(
        settled,
        timeout=timeout,
        message=f"{node.label}: valid-root window still cold, or answers invalid",
    )
    return answer["verdict"]


def rln_membership_state(node: DeliveryNode) -> dict:
    """`get_membership_state` of the node's membership on its registry, read live
    from the chain: `{"state": "active" | "pending" | "failed" | "unknown" | …}`;
    while the module registers, `unknown` carries `{"provisioning": {"step": …}}`."""
    return _from_json_text(_rln_call(node, "get_membership_state"))


def register_membership(node: DeliveryNode, fund: Callable[[str], None], *, timeout: float) -> None:
    """Fund the node's LEZ payer so that its RLN module registers a membership by
    itself. Raises unless the membership becomes `active`."""
    wait_for_lez_wallet(node, timeout=timeout)
    fund(_from_json_text(node.client.call(LEZ_RLN_MODULE, "wallet_status"))["payer"])

    def settled():
        state = rln_membership_state(node)
        return state if state.get("state") in ("active", "failed") else None

    state = wait_until(settled, timeout=timeout, interval=2.0, message=f"{node.label}: membership not registered")
    if state.get("state") != "active":
        raise AssertionError(f"{node.label}: membership {state!r}")


def _rln_call(node: DeliveryNode, method: str, *args: str) -> Any:
    """Call a liblogos_rln_module method on the node's registry and the RLN
    identifier. Every argument is text, sent with the CLI's `str:` prefix so that a
    digit-only timestamp stays a string."""
    texts = [f"str:{arg}" for arg in (node.registry_id, RLN_IDENTIFIER, *args)]
    return node.client.call(RLN_MODULE, method, *texts)


def _rln_answer(node: DeliveryNode, method: str, *args: str) -> Optional[dict]:
    """The value of a liblogos_rln_module result call, or None while the module
    answers with a retryable error class. The module returns `error` as JSON text,
    and `value` can arrive as JSON text too."""
    result = _rln_call(node, method, *args)
    if result.get("success"):
        return _from_json_text(result["value"])
    error = _from_json_text(result["error"])
    if error.get("class") not in RLN_RETRY_CLASSES:
        raise AssertionError(f"{node.label}: {method} failed: {error!r}")
    return None


def _from_json_text(value: Any) -> Any:
    return json.loads(value) if isinstance(value, str) else value


def lez_rpc(sequencer_url: str, method: str, *params: Any) -> Any:
    """The result of a LEZ sequencer JSON-RPC call."""
    request = urllib.request.Request(
        sequencer_url,
        data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": list(params)}).encode(),
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=LEZ_RPC_TIMEOUT_S) as response:
            reply = json.load(response)
    except OSError as e:
        raise AssertionError(f"{method} at {sequencer_url}: {e}") from e
    if "result" not in reply:
        raise AssertionError(f"{method} returned no result: {reply!r}")
    return reply["result"]


def lez_clock_ms(sequencer_url: str) -> int:
    """CLOCK_50's timestamp; the RLN registry refuses to register while it is 0."""
    account = lez_rpc(sequencer_url, "getAccount", LEZ_CLOCK_50_ACCOUNT)
    shard = account["data"]["shards"].get(LEZ_CLOCK_SHARD, [])
    return int.from_bytes(bytes(shard[8:16]), "little")


def base58_to_hex(text: str) -> str:
    """A base58 LEZ account id as the 64-hex text the RLN modules take."""
    value = 0
    for char in text:
        value = value * 58 + BASE58_ALPHABET.index(char)
    return value.to_bytes(32, "big").hex()


def is_connected(event: dict) -> bool:
    """A connectionStateChanged event reporting at least one mesh peer on every subscribed shard."""
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

    `logosctl watch` serializes a typed event as
    {"event": <name>, "data": {"arg0": <1st arg>, "arg1": <2nd arg>, ...}},
    so each codegen event arg
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
