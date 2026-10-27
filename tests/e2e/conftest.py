"""Fixtures for the delivery_module e2e suite.

Four daemon flavours:
  * `solo_daemon` (function scope) — one fresh logosctl container per test, used
    by the single-node lifecycle/query tests. Fresh because createNode is
    once-per-context: a started node can't be re-created, so tests can't share one.
  * `node_a` / `node_b` (module scope) — two daemons in a shared docker network,
    peered directly via staticnodes (node A's container-routable multiaddr), used
    by the two-node delivery tests.
  * `rln_node_factory` (module scope) — daemons on the RLN registry of `lez_registry`,
    used by the RLN tests.
  * `fleet_node_factory` (function scope) — daemons on the logos.test fleet, each
    in its own docker network, used by the fleet tests.
  * `lez_wallet` (session scope) — one daemon whose LEZ module creates a new wallet
    without syncing; a node on the LEZ testnet starts from a copy of it.

`_e2e_env` and `_rln_modules` fail the test when a prerequisite is missing.
"""

from __future__ import annotations

import ipaddress
import json
import os
import secrets
import shutil
import subprocess
import sys
import time
import uuid
from collections.abc import Callable, Iterator
from contextlib import ExitStack
from pathlib import Path
from typing import NamedTuple

import base58
import pytest

from libs.constants import FLEET_PRESET, FLEET_REGISTRY_ID, LEZ_SEQUENCER_PORT, NETWORK_SUBNET
from libs.helpers import (
    LEZ_RLN_MODULE,
    LEZ_SEQUENCER_ENV,
    RLN_MODULE,
    RLN_PRESETS_ENV,
    RLN_PRESETS_FILE,
    DeliveryNode,
    current_epoch,
    is_connected,
    lez_clock_ms,
    lez_last_block_id,
    lez_rpc,
    make_delivery_config,
    make_rln_presets,
    node_multiaddr,
    seconds_until_epoch,
    setup_delivery_node,
    setup_rln_node,
    wait_for_event,
    wait_for_lez_wallet,
    wait_until,
)

CONNECTED_TIMEOUT_S = 10.0
DAEMON_START_ATTEMPTS = 3

LEZ_WALLET_FILES = ("storage.json", "payer.json")
LEZ_WALLET_TIMEOUT_S = 60.0
UNREACHABLE_SEQUENCER = "http://127.0.0.1:1"

LEZ_START_TIMEOUT_S = 20.0
LEZ_CLOCK_TIMEOUT_S = 75.0
LEZ_RLN_TOOL_TIMEOUT_S = 120.0
LEZ_GENESIS_BALANCE = 10**13
# A registration needs the price (100 × 10,000) plus a fee reserve of at most 646,400,000.
LEZ_PAYER_FUNDING = 10**10


class LezRegistry(NamedTuple):
    """An RLN registry on a local LEZ sequencer. `fund` sends native balance to an
    account id once CLOCK_50 has ticked."""

    registry_id: str
    sequencer_url: str
    fund: Callable[[str], None]


def _docker_logs(container_name: str) -> str:
    r = subprocess.run(["docker", "logs", container_name], capture_output=True, text=True)
    return (r.stdout or "") + (r.stderr or "")


def _save_logs(container_name: str) -> None:
    log_dir = Path(os.environ.get("E2E_LOG_DIR", "/tmp"))
    try:
        log_dir.mkdir(parents=True, exist_ok=True)
        (log_dir / f"{container_name}.log").write_text(_docker_logs(container_name))
    except OSError as e:
        sys.stderr.write(f"warning: failed to save log for {container_name}: {e}\n")


def _announce(config: pytest.Config, text: str) -> None:
    """Write `text` to the terminal past pytest's output capture."""
    reporter = config.pluginmanager.getplugin("terminalreporter")
    if reporter is None:
        return
    with config.pluginmanager.getplugin("capturemanager").global_and_fixture_disabled():
        reporter.write_line(text)
        reporter.flush()


def _create_network(name: str, *options: str) -> None:
    r = subprocess.run(
        ["docker", "network", "create", *options, name],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        pytest.fail(f"failed to create docker network {name!r}: {r.stderr.strip()}")


def _remove_network(name: str) -> None:
    subprocess.run(["docker", "network", "rm", name], capture_output=True)


def _remove_container(name: str) -> None:
    subprocess.run(["docker", "rm", "-f", name], capture_output=True)


def _copy_keystore_back(container_name: str, membership: Path) -> None:
    """Replace the RLN keystore in `membership` with the node's, without its lock.
    Raises if the node's `rln_allocations.json` is the one it started from."""
    keystore = membership / RLN_MODULE
    allocations, = keystore.glob("*/rln_allocations.json")
    started_with = allocations.read_bytes()
    # Removed first: if the copy fails, no later node starts from counters this one has used.
    shutil.rmtree(keystore)
    subprocess.run(
        ["docker", "cp", f"{container_name}:/persistence/{RLN_MODULE}", str(keystore)],
        check=True, capture_output=True,
    )
    for lock in keystore.glob("*/rln_keystore.lock"):
        lock.unlink()
    if allocations.read_bytes() == started_with:
        raise AssertionError(f"{container_name}: {allocations.name} unchanged after its sends")


@pytest.fixture(scope="session")
def _e2e_env() -> tuple[str, Path]:
    """The logosctl image (LOGOSCTL_IMAGE) and the built module layout
    (LOGOS_MODULES_DIR). Fails if docker, the image or the layout is missing."""
    from logosctl import docker_available, image_present  # noqa: PLC0415

    if not docker_available():
        pytest.fail("docker not available")
    image = os.environ["LOGOSCTL_IMAGE"]
    if not image_present(image):
        pytest.fail(f"LOGOSCTL_IMAGE={image!r} not present locally")
    modules_dir = Path(os.environ["LOGOS_MODULES_DIR"])
    if not (modules_dir / "delivery_module" / "manifest.json").is_file():
        pytest.fail(
            f"LOGOS_MODULES_DIR={modules_dir} doesn't contain "
            "delivery_module/manifest.json — did you install the modules with logosctl?"
        )
    return image, modules_dir


@pytest.fixture(scope="session")
def _rln_modules(_e2e_env: tuple[str, Path]) -> None:
    """Fails unless LOGOS_MODULES_DIR also holds the RLN modules."""
    _, modules_dir = _e2e_env
    for module in (RLN_MODULE, LEZ_RLN_MODULE):
        if not (modules_dir / module / "manifest.json").is_file():
            pytest.fail(f"LOGOS_MODULES_DIR={modules_dir} doesn't contain {module}/manifest.json")


@pytest.fixture(scope="session")
def rln_membership(_rln_modules: None) -> Path:
    """The RLN membership on the LEZ testnet in E2E_RLN_MEMBERSHIP_DIR: module folders
    laid out like a daemon's persistence dir (`<module>/<instance>/`). Fails if it
    holds no keystore."""
    membership_dir = Path(os.environ["E2E_RLN_MEMBERSHIP_DIR"])
    if not any(membership_dir.glob(f"{RLN_MODULE}/*/rln_sealed.json")):
        pytest.fail(
            f"E2E_RLN_MEMBERSHIP_DIR={membership_dir} holds no {RLN_MODULE}/<instance>/rln_sealed.json"
        )
    return membership_dir


@pytest.fixture(scope="session")
def fleet_membership(
    rln_membership: Path,
    tmp_path_factory: pytest.TempPathFactory,
    request: pytest.FixtureRequest,
    worker_id: str,
) -> Iterator[Path]:
    """A copy of the RLN membership for the nodes that send through the fleet. Each
    sender's keystore is copied back here when its test ends, so the next sender goes
    on from its `message_id`s. Across runs, which restore the same counters, the file
    named by E2E_FLEET_EPOCH_FILE holds the last epoch a run sent in, and this run
    sends only in a later one; without the file it waits for the next epoch."""
    if worker_id != "master" and not request.config.getoption("loadgroup"):
        pytest.fail(
            "the fleet tests go on from each other's message_ids, so they need one xdist "
            "worker: run them with --dist=loadgroup, or without -n"
        )
    membership = tmp_path_factory.mktemp("fleet-membership")
    shutil.copytree(rln_membership, membership, dirs_exist_ok=True)
    epoch_file = os.environ.get("E2E_FLEET_EPOCH_FILE")
    last_epoch = (
        int(Path(epoch_file).read_text())
        if epoch_file and Path(epoch_file).is_file()
        else current_epoch()
    )
    wait_s = seconds_until_epoch(last_epoch + 1)
    if wait_s:
        starts = time.strftime("%H:%M:%S", time.localtime(time.time() + wait_s))
        _announce(
            request.config,
            f"Waiting {wait_s:.0f} s, until {starts}, for an RLN epoch no earlier fleet run sent in",
        )
        time.sleep(wait_s)
    yield membership
    if epoch_file:
        Path(epoch_file).parent.mkdir(parents=True, exist_ok=True)
        Path(epoch_file).write_text(f"{current_epoch()}\n")


@pytest.fixture(scope="session")
def lez_wallet(
    _e2e_env: tuple[str, Path],
    _rln_modules: None,
    shared_docker_network: str,
    tmp_path_factory: pytest.TempPathFactory,
) -> Path:
    """A new LEZ wallet with `last_synced_block` at the testnet's tip, which a node on
    the testnet starts from so that its wallet skips the sync from block 0. A daemon
    whose sequencer is unreachable creates it without syncing."""
    image, modules_dir = _e2e_env
    daemon, container_name = _start_daemon(
        image, modules_dir, shared_docker_network, "lez-wallet",
        env={LEZ_SEQUENCER_ENV: UNREACHABLE_SEQUENCER},
    )
    wallet = tmp_path_factory.mktemp("lez-wallet")
    try:
        node = DeliveryNode(daemon.client(), daemon, "lez-wallet")
        node.client.load_module(LEZ_RLN_MODULE)
        wait_for_lez_wallet(node, timeout=LEZ_WALLET_TIMEOUT_S)
        subprocess.run(
            ["docker", "cp", f"{container_name}:/persistence/{LEZ_RLN_MODULE}", str(wallet / LEZ_RLN_MODULE)],
            check=True, capture_output=True,
        )
    finally:
        _save_logs(container_name)
        daemon.stop()
    wallet_home, = wallet.glob(f"{LEZ_RLN_MODULE}/*/wallet-home")
    # wallet_config.json names the unreachable sequencer; each node writes its own.
    for path in wallet_home.iterdir():
        if path.name not in LEZ_WALLET_FILES:
            path.unlink()
    storage_path = wallet_home / "storage.json"
    storage = json.loads(storage_path.read_text())
    storage["last_synced_block"] = lez_last_block_id()
    storage_path.write_text(json.dumps(storage))
    return wallet


def _worker_subnet(worker_id: str) -> str:
    """The n-th /24 of NETWORK_SUBNET for xdist worker gw<n>, the first for a run
    without xdist."""
    index = 0 if worker_id == "master" else int(worker_id.removeprefix("gw"))
    return str(list(ipaddress.ip_network(NETWORK_SUBNET).subnets(new_prefix=24))[index])


@pytest.fixture(scope="session")
def shared_docker_network(_e2e_env: tuple[str, Path], worker_id: str) -> Iterator[str]:
    name = f"logosdelivery-e2e-{uuid.uuid4().hex[:8]}"
    _create_network(name, "--subnet", _worker_subnet(worker_id))
    try:
        yield name
    finally:
        _remove_network(name)


def _lez_sequencer_config(payer: str) -> dict:
    """`lez/sequencer/service/configs/docker/sequencer_config.json`, with
    `payer` as the only genesis account and 500 ms blocks so CLOCK_50 ticks within half
    a minute."""
    return {
        "home": "/var/lib/sequencer_service",
        "max_num_tx_in_block": 20,
        "mempool_max_size": 10000,
        "block_create_timeout": "500ms",
        "retry_pending_blocks_timeout": "7s",
        "bedrock_config": {
            "channel_id": "01" * 32,
            "node_url": "http://host.docker.internal:18080",
            "funding_key": "2e03b2eff5a45478e7e79668d2a146cf2c5c7925bce927f2b1c67f2ab4fc0d26",
            "channel_params": {
                "minimum_sequencer_stake": 149,
                "posting_timeframe": 300,
                "posting_timeout": 25,
                "exit_delay": 100,
            },
        },
        "genesis": [{"supply_account": {"account_id": payer, "balance": LEZ_GENESIS_BALANCE}}],
    }


def _lez_rln_tool(lez_rln: Path, tool: str, home: Path, *args: str, env: dict[str, str] | None = None) -> str:
    """Run one of logos-lez-rln's tools with `home` as HOME and wallet home, and return
    its stdout. `run_setup` reads its guest programs relative to `lez-rln/`."""
    r = subprocess.run(
        [str(lez_rln / "target" / "release" / tool), *args],
        cwd=lez_rln, capture_output=True, text=True, timeout=LEZ_RLN_TOOL_TIMEOUT_S,
        env={**os.environ, "HOME": str(home), "LEE_WALLET_HOME_DIR": str(home), **(env or {})},
    )
    if r.returncode != 0:
        pytest.fail(f"{tool} exited {r.returncode}:\n{r.stdout}{r.stderr}")
    return r.stdout


def _lez_block_produced(sequencer_url: str) -> bool:
    try:
        return lez_rpc(sequencer_url, "getLastBlockId") >= 1
    except AssertionError:
        return False


def _lez_wallet_config(sequencer_url: str) -> dict:
    """The tools' LEZ wallet config, pointed at the local sequencer instead of the
    public testnet. The wallet polls for a transaction's inclusion every
    `seq_poll_timeout`."""
    return {
        "sequencer": {"sequencer_addr": sequencer_url},
        "seq_poll_timeout": "500ms",
        "seq_tx_poll_max_blocks": 60,
        "seq_poll_max_retries": 10,
        "seq_block_poll_max_amount": 100,
    }


def _start_lez_sequencer(image: str, network: str, sequencer_config: Path) -> str:
    container_name = f"lez-sequencer-{uuid.uuid4().hex[:8]}"
    r = subprocess.run(
        [
            "docker", "run", "-d", "--name", container_name, "--network", network,
            "-p", f"127.0.0.1::{LEZ_SEQUENCER_PORT}",
            "-v", f"{sequencer_config}:/etc/sequencer_service/sequencer_config.json:ro",
            image,
        ],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        pytest.fail(f"failed to start the LEZ sequencer: {r.stderr.strip()}")
    return container_name


def _lez_host_url(container_name: str) -> str:
    port = subprocess.run(
        ["docker", "port", container_name, str(LEZ_SEQUENCER_PORT)],
        capture_output=True, text=True, check=True,
    ).stdout.split()[0].rsplit(":", 1)[1]
    return f"http://127.0.0.1:{port}/"


def _run_setup(lez_rln: Path, home: Path, payer: str) -> str:
    """Deploy a registry on a fresh tree, paid by `payer`, and return its config account."""
    setup = _lez_rln_tool(
        lez_rln, "run_setup", home,
        env={"LEZ_RLN_PAYER": payer, "LEZ_RLN_TREE_ID_HEX": secrets.token_hex(32), "RISC0_DEV_MODE": "1"},
    )
    config_account = next(
        (line.split()[-1] for line in setup.splitlines() if line.startswith("Config account:")), None
    )
    if not config_account:
        pytest.fail(f"run_setup printed no config account:\n{setup}")
    return config_account


@pytest.fixture(scope="session")
def lez_registry(
    _rln_modules: None,
    shared_docker_network: str,
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[LezRegistry]:
    """An RLN registry deployed with `run_setup` on a local LEZ sequencer in
    `shared_docker_network`. `fund` waits for CLOCK_50, because the registry refuses
    registrations before it."""
    image = os.environ["E2E_LEZ_SEQUENCER_IMAGE"]
    lez_rln = Path(os.environ["E2E_LEZ_RLN_DIR"]) / "lez-rln"
    home = tmp_path_factory.mktemp("lez-rln-home")
    payer = _lez_rln_tool(lez_rln, "mint_payer", home).split()[-1]
    sequencer_config = home / "sequencer_config.json"
    sequencer_config.write_text(json.dumps(_lez_sequencer_config(payer)))
    container_name = _start_lez_sequencer(image, shared_docker_network, sequencer_config)
    try:
        host_url = _lez_host_url(container_name)
        (home / "wallet_config.json").write_text(json.dumps(_lez_wallet_config(host_url)))
        wait_until(
            lambda: _lez_block_produced(host_url),
            timeout=LEZ_START_TIMEOUT_S,
            message="the LEZ sequencer produced no block",
        )
        config_account = _run_setup(lez_rln, home, payer)

        def fund(account_id: str) -> None:
            wait_until(
                lambda: lez_clock_ms(host_url) > 0,
                timeout=LEZ_CLOCK_TIMEOUT_S,
                interval=1.0,
                message="CLOCK_50 never ticked",
            )
            _lez_rln_tool(
                lez_rln, "fund_account", home, "--to", account_id, "--amount", str(LEZ_PAYER_FUNDING),
                env={"LEZ_RLN_PAYER": payer},
            )

        yield LezRegistry(
            registry_id=f"logos:local:{base58.b58decode(config_account).hex()}",
            sequencer_url=f"http://{container_name}:{LEZ_SEQUENCER_PORT}/",
            fund=fund,
        )
    finally:
        _save_logs(container_name)
        subprocess.run(["docker", "rm", "-f", "-v", container_name], capture_output=True)


def _start_daemon(
    image: str,
    modules_dir: Path,
    network: str,
    label: str,
    *,
    persistence_dir: Path | None = None,
    env: dict[str, str] | None = None,
):
    """A started logosctl container. A start that docker refuses with a host port
    already allocated is retried under a new container name.
    TODO: logos-logoscore-py#37"""
    from logosctl import LogosctlDockerDaemon, LogosctlError  # noqa: PLC0415

    for attempt in range(1, DAEMON_START_ATTEMPTS + 1):
        container_name = f"logosctl-{label.lower()}-{uuid.uuid4().hex[:8]}"
        daemon = LogosctlDockerDaemon(
            image=image,
            modules_dir=modules_dir,
            container_name=container_name,
            network=network,
            persistence_dir=persistence_dir,
            env=env,
            startup_timeout=30.0,
            extra_args=["--verbose"],
        )
        try:
            daemon.start()
            return daemon, container_name
        except LogosctlError as e:
            daemon.stop()
            _remove_container(container_name)
            if "port is already allocated" not in str(e) or attempt == DAEMON_START_ATTEMPTS:
                raise


@pytest.fixture
def solo_daemon(_e2e_env: tuple[str, Path], shared_docker_network: str) -> Iterator:
    """A fresh, started logosctl container — module mounted, nothing loaded yet.
    The test drives load_module/createNode/start/stop itself."""
    image, modules_dir = _e2e_env
    daemon, container_name = _start_daemon(image, modules_dir, shared_docker_network, "solo")
    try:
        yield daemon
    finally:
        _save_logs(container_name)
        daemon.stop()


@pytest.fixture(scope="module")
def delivery_node_factory(
    _e2e_env: tuple[str, Path],
    shared_docker_network: str,
) -> Iterator[Callable[[str, str], DeliveryNode]]:
    """Factory spawning a fully set-up (load+createNode+start) DeliveryNode per
    call. All teardown + log capture is registered on one ExitStack closed when
    the module's tests finish."""
    image, modules_dir = _e2e_env

    with ExitStack() as stack:
        def _create(label: str, config_json: str) -> DeliveryNode:
            daemon, container_name = _start_daemon(image, modules_dir, shared_docker_network, label)
            stack.callback(daemon.stop)
            stack.callback(_save_logs, container_name)
            return setup_delivery_node(daemon, config_json, label)

        yield _create


@pytest.fixture(scope="module")
def node_a(delivery_node_factory: Callable[[str, str], DeliveryNode]) -> DeliveryNode:
    return delivery_node_factory("A", make_delivery_config())


@pytest.fixture(scope="module")
def node_b(
    delivery_node_factory: Callable[[str, str], DeliveryNode],
    node_a: DeliveryNode,
) -> DeliveryNode:
    with node_a.watch() as events:
        node = delivery_node_factory("B", make_delivery_config(static_peers=[node_multiaddr(node_a)]))
        wait_for_event(events, "connectionStateChanged", timeout=CONNECTED_TIMEOUT_S, predicate=is_connected)
    return node


@pytest.fixture(scope="module")
def rln_node_factory(
    _e2e_env: tuple[str, Path],
    _rln_modules: None,
    shared_docker_network: str,
    lez_registry: LezRegistry,
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[Callable[..., DeliveryNode]]:
    """`delivery_node_factory` for nodes with RLN on `lez_registry`, through a presets
    file named by LOGOS_DELIVERY_RLN_PRESETS and the sequencer's URL in
    LEZ_RLN_SEQUENCER."""
    image, modules_dir = _e2e_env

    with ExitStack() as stack:
        def _create(
            label: str,
            config_json: str,
            *,
            enable_validation: bool,
        ) -> DeliveryNode:
            persistence_dir = tmp_path_factory.mktemp(f"persistence-{label.lower()}")
            (persistence_dir / RLN_PRESETS_FILE).write_text(
                make_rln_presets(enable_validation=enable_validation, registry_id=lez_registry.registry_id)
            )
            daemon, container_name = _start_daemon(
                image, modules_dir, shared_docker_network, label,
                persistence_dir=persistence_dir,
                env={
                    RLN_PRESETS_ENV: f"/persistence/{RLN_PRESETS_FILE}",
                    LEZ_SEQUENCER_ENV: lez_registry.sequencer_url,
                },
            )
            stack.callback(daemon.stop)
            stack.callback(_save_logs, container_name)
            return setup_rln_node(daemon, config_json, label, lez_registry.registry_id)

        yield _create


@pytest.fixture
def fleet_node_factory(
    _e2e_env: tuple[str, Path],
    _rln_modules: None,
    lez_wallet: Path,
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[Callable[..., DeliveryNode]]:
    """Factory for nodes on the logos.test fleet, each in its own docker network so
    that a test's nodes reach each other only through the fleet. Every node starts
    with a copy of `lez_wallet`, and gets a presets file whose `logos.test` entry turns
    proof validation on, which the module's built-in entry leaves off. A node given
    `membership` starts with a copy of it and copies its keystore back when the test
    ends."""
    image, modules_dir = _e2e_env

    with ExitStack() as stack:
        def _create(
            label: str,
            config_json: str,
            *,
            membership: Path | None = None,
        ) -> DeliveryNode:
            network = f"logosdelivery-fleet-{uuid.uuid4().hex[:8]}"
            _create_network(network)
            stack.callback(_remove_network, network)
            persistence_dir = tmp_path_factory.mktemp(f"persistence-{label.lower()}")
            shutil.copytree(lez_wallet, persistence_dir, dirs_exist_ok=True)
            if membership:
                shutil.copytree(membership, persistence_dir, dirs_exist_ok=True)
            (persistence_dir / RLN_PRESETS_FILE).write_text(
                make_rln_presets(enable_validation=True, registry_id=FLEET_REGISTRY_ID, preset=FLEET_PRESET)
            )
            daemon, container_name = _start_daemon(
                image, modules_dir, network, label,
                persistence_dir=persistence_dir,
                env={RLN_PRESETS_ENV: f"/persistence/{RLN_PRESETS_FILE}"},
            )
            stack.callback(daemon.stop)
            if membership:
                stack.callback(_copy_keystore_back, container_name, membership)
            stack.callback(_save_logs, container_name)
            return setup_rln_node(daemon, config_json, label, FLEET_REGISTRY_ID)

        yield _create
