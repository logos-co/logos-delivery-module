"""Fixtures for the delivery_module e2e suite.

Four daemon flavours:
  * `solo_daemon` (function scope) — one fresh logosctl container per test, used
    by the single-node lifecycle/query tests. Fresh because createNode is
    once-per-context: a started node can't be re-created, so tests can't share one.
  * `node_a` / `node_b` (module scope) — two daemons in a shared docker network,
    peered directly via staticnodes (node A's container-routable multiaddr), used
    by the two-node delivery tests.
  * `rln_node_factory` (module scope) — daemons with RLN turned on through a
    presets file in each one's persistence dir, on the RLN registry of a local
    LEZ sequencer (`lez_registry`), used by the RLN tests.
  * `lez_wallet` (session scope) — one daemon whose LEZ module creates a new wallet
    without syncing; a node on the LEZ testnet starts from a copy of it.

Every prerequisite (docker, the logosctl image, the built module layout) is
gated by `_e2e_env_or_skip`, the RLN modules by `_rln_modules_or_skip`, and the
local LEZ chain by `lez_registry`, so the suite skips cleanly when run without setup.
"""

from __future__ import annotations

import ipaddress
import json
import os
import secrets
import subprocess
import sys
import time
import uuid
from collections.abc import Callable, Iterator
from contextlib import ExitStack
from pathlib import Path
from typing import NamedTuple

import pytest

from libs.constants import LEZ_SEQUENCER_PORT, NETWORK_SUBNET
from libs.helpers import (
    LEZ_RLN_MODULE,
    LEZ_SEQUENCER_ENV,
    RLN_MEMBERSHIP_TIMEOUT_S,
    RLN_MODULE,
    RLN_PRESETS_ENV,
    RLN_PRESETS_FILE,
    DeliveryNode,
    base58_to_hex,
    lez_clock_ms,
    lez_last_block_id,
    lez_rpc,
    make_delivery_config,
    make_rln_presets,
    node_multiaddr,
    register_membership,
    setup_delivery_node,
    setup_rln_node,
    wait_for_lez_wallet,
    wait_until,
)

MESH_STABILIZATION_S = 12.0

LEZ_WALLET_FILES = ("storage.json", "payer.json")
LEZ_WALLET_TIMEOUT_S = 60.0
UNREACHABLE_SEQUENCER = "http://127.0.0.1:1"

LEZ_RLN_TOOLS = ("mint_payer", "run_setup", "fund_account")
LEZ_START_TIMEOUT_S = 60.0
LEZ_CLOCK_TIMEOUT_S = 180.0
LEZ_RLN_TOOL_TIMEOUT_S = 600.0
LEZ_GENESIS_BALANCE = 10**13
# A registration needs the price (100 × 10,000) plus a fee reserve of at most 646,400,000.
LEZ_PAYER_FUNDING = 10**10


class LezRegistry(NamedTuple):
    """An RLN registry on a local LEZ sequencer. `fund` sends native balance to an
    account id, from the account that deployed the registry."""

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


@pytest.fixture(scope="session")
def _e2e_env_or_skip() -> tuple[str, Path]:
    """Single gate: skip unless docker, the logosctl image, and the built
    delivery_module layout are all present."""
    from logosctl import docker_available, image_present  # noqa: PLC0415

    if not docker_available():
        pytest.skip("docker not available")
    image = os.environ.get("LOGOSCTL_IMAGE")
    if not image:
        pytest.skip("LOGOSCTL_IMAGE not set")
    if not image_present(image):
        pytest.skip(f"LOGOSCTL_IMAGE={image!r} not present locally")
    modules_dir_env = os.environ.get("LOGOS_MODULES_DIR")
    if not modules_dir_env:
        pytest.skip("LOGOS_MODULES_DIR not set")
    modules_dir = Path(modules_dir_env)
    if not (modules_dir / "delivery_module" / "manifest.json").is_file():
        pytest.skip(
            f"LOGOS_MODULES_DIR={modules_dir} doesn't contain "
            "delivery_module/manifest.json — did you install the modules with logosctl?"
        )
    return image, modules_dir


@pytest.fixture(scope="session")
def _rln_modules_or_skip(_e2e_env_or_skip: tuple[str, Path]) -> None:
    """Skip unless LOGOS_MODULES_DIR also holds the RLN modules (CI installs them
    next to delivery_module)."""
    _, modules_dir = _e2e_env_or_skip
    for module in (RLN_MODULE, LEZ_RLN_MODULE):
        if not (modules_dir / module / "manifest.json").is_file():
            pytest.skip(f"LOGOS_MODULES_DIR={modules_dir} doesn't contain {module}/manifest.json")


@pytest.fixture(scope="session")
def rln_membership(_rln_modules_or_skip: None) -> Path:
    """The RLN membership on the LEZ testnet in E2E_RLN_MEMBERSHIP_DIR: module folders
    laid out like a daemon's persistence dir (`<module>/<instance>/`). Skips when the
    variable is unset, fails when it holds no keystore."""
    membership_dir = os.environ.get("E2E_RLN_MEMBERSHIP_DIR")
    if not membership_dir:
        pytest.skip("E2E_RLN_MEMBERSHIP_DIR not set")
    if not any(Path(membership_dir).glob(f"{RLN_MODULE}/*/rln_sealed.json")):
        pytest.fail(
            f"E2E_RLN_MEMBERSHIP_DIR={membership_dir} holds no {RLN_MODULE}/<instance>/rln_sealed.json"
        )
    return Path(membership_dir)


@pytest.fixture(scope="session")
def lez_wallet(
    _e2e_env_or_skip: tuple[str, Path],
    _rln_modules_or_skip: None,
    shared_docker_network: str,
    tmp_path_factory: pytest.TempPathFactory,
) -> Path:
    """A new LEZ wallet with `last_synced_block` at the testnet's tip, which a node on
    the testnet starts from so that its wallet skips the sync from block 0. A daemon
    whose sequencer is unreachable creates it without syncing."""
    image, modules_dir = _e2e_env_or_skip
    daemon, container_name = _new_daemon(
        image, modules_dir, shared_docker_network, "lez-wallet",
        env={LEZ_SEQUENCER_ENV: UNREACHABLE_SEQUENCER},
    )
    wallet = tmp_path_factory.mktemp("lez-wallet")
    try:
        daemon.start()
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
    """The xdist worker's /24 of NETWORK_SUBNET: worker gw<n> takes the n-th, a run
    without xdist the first."""
    index = 0 if worker_id == "master" else int(worker_id.removeprefix("gw"))
    return str(list(ipaddress.ip_network(NETWORK_SUBNET).subnets(new_prefix=24))[index])


@pytest.fixture(scope="session")
def shared_docker_network(_e2e_env_or_skip: tuple[str, Path], worker_id: str) -> Iterator[str]:
    name = f"logosdelivery-e2e-{uuid.uuid4().hex[:8]}"
    r = subprocess.run(
        ["docker", "network", "create", "--subnet", _worker_subnet(worker_id), name],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        pytest.fail(f"failed to create docker network {name!r}: {r.stderr.strip()}")
    try:
        yield name
    finally:
        subprocess.run(["docker", "network", "rm", name], capture_output=True)


def _lez_sequencer_config(payer: str) -> dict:
    """LEZ v0.3.1's `lez/sequencer/service/configs/docker/sequencer_config.json` with
    1 s blocks, so that CLOCK_50 ticks within a minute, and `payer` as the only
    genesis account."""
    return {
        "home": "/var/lib/sequencer_service",
        "max_num_tx_in_block": 20,
        "max_block_size": "1 MiB",
        "mempool_max_size": 10000,
        "block_create_timeout": "1s",
        "retry_pending_blocks_timeout": "7s",
        "bedrock_config": {
            "backoff": {"start_delay": "100ms", "max_retries": 5},
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
    """Run one of logos-lez-rln's tools with `home` as its HOME and LEZ wallet, and
    return its stdout. `run_setup` reads its guest programs relative to `lez-rln/`."""
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


@pytest.fixture(scope="session")
def lez_registry(
    _rln_modules_or_skip: None,
    shared_docker_network: str,
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[LezRegistry]:
    """An RLN registry on a local LEZ sequencer in `shared_docker_network`, deployed
    by logos-lez-rln's `run_setup` once CLOCK_50 ticks. Skips unless
    E2E_LEZ_SEQUENCER_IMAGE names a present image and E2E_LEZ_RLN_DIR a
    logos-lez-rln checkout with its tools built."""
    from logosctl import image_present  # noqa: PLC0415

    image = os.environ.get("E2E_LEZ_SEQUENCER_IMAGE")
    if not image:
        pytest.skip("E2E_LEZ_SEQUENCER_IMAGE not set")
    if not image_present(image):
        pytest.skip(f"E2E_LEZ_SEQUENCER_IMAGE={image!r} not present locally")
    lez_rln_dir = os.environ.get("E2E_LEZ_RLN_DIR")
    if not lez_rln_dir:
        pytest.skip("E2E_LEZ_RLN_DIR not set")
    lez_rln = Path(lez_rln_dir) / "lez-rln"
    for tool in LEZ_RLN_TOOLS:
        if not (lez_rln / "target" / "release" / tool).is_file():
            pytest.skip(f"E2E_LEZ_RLN_DIR={lez_rln_dir} has no lez-rln/target/release/{tool}")

    home = tmp_path_factory.mktemp("lez-rln-home")
    payer = _lez_rln_tool(lez_rln, "mint_payer", home).split()[-1]
    sequencer_config = home / "sequencer_config.json"
    sequencer_config.write_text(json.dumps(_lez_sequencer_config(payer)))

    container_name = f"lez-sequencer-{uuid.uuid4().hex[:8]}"
    r = subprocess.run(
        [
            "docker", "run", "-d", "--name", container_name, "--network", shared_docker_network,
            "-p", f"127.0.0.1::{LEZ_SEQUENCER_PORT}",
            "-v", f"{sequencer_config}:/etc/sequencer_service/sequencer_config.json:ro",
            image,
        ],
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        pytest.fail(f"failed to start the LEZ sequencer: {r.stderr.strip()}")
    try:
        port = subprocess.run(
            ["docker", "port", container_name, str(LEZ_SEQUENCER_PORT)],
            capture_output=True, text=True, check=True,
        ).stdout.split()[0].rsplit(":", 1)[1]
        host_url = f"http://127.0.0.1:{port}/"
        # Without it the tools' wallet writes one that names the public testnet. The
        # wallet sleeps `seq_poll_timeout` between checks for a transaction's inclusion.
        (home / "wallet_config.json").write_text(json.dumps({
            "sequencer": {"sequencer_addr": host_url},
            "seq_poll_timeout": "1s",
            "seq_tx_poll_max_blocks": 60,
            "seq_poll_max_retries": 10,
            "seq_block_poll_max_amount": 100,
        }))
        wait_until(
            lambda: _lez_block_produced(host_url),
            timeout=LEZ_START_TIMEOUT_S,
            message="the LEZ sequencer produced no block",
        )

        setup = _lez_rln_tool(
            lez_rln, "run_setup", home,
            env={"LEZ_RLN_PAYER": payer, "LEZ_RLN_TREE_ID_HEX": secrets.token_hex(32), "RISC0_DEV_MODE": "1"},
        )
        config_account = next(
            (line.split()[-1] for line in setup.splitlines() if line.startswith("Config account:")), None
        )
        if not config_account:
            pytest.fail(f"run_setup printed no config account:\n{setup}")
        wait_until(
            lambda: lez_clock_ms(host_url) > 0,
            timeout=LEZ_CLOCK_TIMEOUT_S,
            interval=2.0,
            message="CLOCK_50 never ticked",
        )

        def fund(account_id: str) -> None:
            _lez_rln_tool(
                lez_rln, "fund_account", home, "--to", account_id, "--amount", str(LEZ_PAYER_FUNDING),
                env={"LEZ_RLN_PAYER": payer},
            )

        yield LezRegistry(
            registry_id=f"logos:local:{base58_to_hex(config_account)}",
            sequencer_url=f"http://{container_name}:{LEZ_SEQUENCER_PORT}/",
            fund=fund,
        )
    finally:
        _save_logs(container_name)
        subprocess.run(["docker", "rm", "-f", "-v", container_name], capture_output=True)


def _new_daemon(
    image: str,
    modules_dir: Path,
    network: str,
    label: str,
    *,
    persistence_dir: Path | None = None,
    env: dict[str, str] | None = None,
):
    from logosctl import LogosctlDockerDaemon  # noqa: PLC0415

    container_name = f"logosctl-{label.lower()}-{uuid.uuid4().hex[:8]}"
    daemon = LogosctlDockerDaemon(
        image=image,
        modules_dir=modules_dir,
        container_name=container_name,
        network=network,
        persistence_dir=persistence_dir,
        env=env,
        startup_timeout=60.0,
        extra_args=["--verbose"],
    )
    return daemon, container_name


@pytest.fixture
def solo_daemon(_e2e_env_or_skip: tuple[str, Path], shared_docker_network: str) -> Iterator:
    """A fresh, started logosctl container — module mounted, nothing loaded yet.
    The test drives load_module/createNode/start/stop itself."""
    image, modules_dir = _e2e_env_or_skip
    daemon, container_name = _new_daemon(image, modules_dir, shared_docker_network, "solo")
    try:
        daemon.start()
        yield daemon
    finally:
        _save_logs(container_name)
        daemon.stop()


@pytest.fixture(scope="module")
def delivery_node_factory(
    _e2e_env_or_skip: tuple[str, Path],
    shared_docker_network: str,
) -> Iterator[Callable[[str, str], DeliveryNode]]:
    """Factory spawning a fully set-up (load+createNode+start) DeliveryNode per
    call. All teardown + log capture is registered on one ExitStack closed when
    the module's tests finish."""
    image, modules_dir = _e2e_env_or_skip

    with ExitStack() as stack:
        def _create(label: str, config_json: str) -> DeliveryNode:
            daemon, container_name = _new_daemon(image, modules_dir, shared_docker_network, label)
            stack.enter_context(daemon)
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
    peer = node_multiaddr(node_a)
    node = delivery_node_factory("B", make_delivery_config(static_peers=[peer]))
    time.sleep(MESH_STABILIZATION_S)
    return node


@pytest.fixture(scope="module")
def rln_node_factory(
    _e2e_env_or_skip: tuple[str, Path],
    _rln_modules_or_skip: None,
    shared_docker_network: str,
    lez_registry: LezRegistry,
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[Callable[..., DeliveryNode]]:
    """`delivery_node_factory` for nodes with RLN on the local registry: each daemon
    gets a presets file in its persistence dir, named by LOGOS_DELIVERY_RLN_PRESETS,
    and the local sequencer in LEZ_RLN_SEQUENCER. A node given `membership` has its
    payer funded and registers a membership of its own."""
    image, modules_dir = _e2e_env_or_skip

    with ExitStack() as stack:
        def _create(
            label: str,
            config_json: str,
            *,
            enable_validation: bool,
            membership: bool = False,
        ) -> DeliveryNode:
            persistence_dir = tmp_path_factory.mktemp(f"persistence-{label.lower()}")
            (persistence_dir / RLN_PRESETS_FILE).write_text(
                make_rln_presets(enable_validation=enable_validation, registry_id=lez_registry.registry_id)
            )
            daemon, container_name = _new_daemon(
                image, modules_dir, shared_docker_network, label,
                persistence_dir=persistence_dir,
                env={
                    RLN_PRESETS_ENV: f"/persistence/{RLN_PRESETS_FILE}",
                    LEZ_SEQUENCER_ENV: lez_registry.sequencer_url,
                },
            )
            stack.enter_context(daemon)
            stack.callback(_save_logs, container_name)
            node = setup_rln_node(daemon, config_json, label, lez_registry.registry_id)
            if membership:
                register_membership(node, lez_registry.fund, timeout=RLN_MEMBERSHIP_TIMEOUT_S)
            return node

        yield _create
