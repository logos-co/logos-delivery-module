"""Fixtures for the delivery_module e2e suite.

Four daemon flavours:
  * `solo_daemon` (function scope) — one fresh logosctl container per test, used
    by the single-node lifecycle/query tests. Fresh because createNode is
    once-per-context: a started node can't be re-created, so tests can't share one.
  * `node_a` / `node_b` (module scope) — two daemons in a shared docker network,
    peered directly via staticnodes (node A's container-routable multiaddr), used
    by the two-node delivery tests.
  * `rln_node_factory` (module scope) — daemons with RLN turned on through a
    presets file in each one's persistence dir, used by the RLN tests.
  * `fleet_node_factory` (function scope) — daemons on the logos.test fleet, each
    in its own docker network, used by the fleet tests.

Every prerequisite (docker, the logosctl image, the built module layout) is
gated by `_e2e_env_or_skip`, and the RLN modules by `_rln_modules_or_skip`, so the
suite skips cleanly when run without setup.
"""

from __future__ import annotations

import ipaddress
import os
import shutil
import subprocess
import sys
import time
import uuid
from collections.abc import Callable, Iterator
from contextlib import ExitStack
from pathlib import Path

import pytest

from libs.constants import FLEET_PRESET, NETWORK_SUBNET
from libs.helpers import (
    LEZ_RLN_MODULE,
    RLN_MODULE,
    RLN_PRESETS_ENV,
    RLN_PRESETS_FILE,
    DeliveryNode,
    current_epoch,
    make_delivery_config,
    make_rln_presets,
    node_multiaddr,
    seconds_until_epoch,
    setup_delivery_node,
    setup_rln_node,
)

MESH_STABILIZATION_S = 12.0


def _docker_logs(container_name: str) -> str:
    r = subprocess.run(["docker", "logs", container_name], capture_output=True, text=True)
    return (r.stdout or "") + (r.stderr or "")


def _save_logs(container_name: str) -> None:
    log_dir = Path(os.environ.get("E2E_LOG_DIR", "/tmp"))
    log_dir.mkdir(parents=True, exist_ok=True)
    try:
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


def _copy_keystore_back(container_name: str, membership: Path) -> None:
    """Replace the RLN keystore in `membership` with the node's, without its lock."""
    keystore = membership / RLN_MODULE
    # Removed first: if the copy fails, no later node starts from counters this one has used.
    shutil.rmtree(keystore)
    subprocess.run(
        ["docker", "cp", f"{container_name}:/persistence/{RLN_MODULE}", str(keystore)],
        check=True, capture_output=True,
    )
    for lock in keystore.glob("*/rln_keystore.lock"):
        lock.unlink()


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
    """The RLN membership in E2E_RLN_MEMBERSHIP_DIR: module folders laid out like a
    daemon's persistence dir (`<module>/<instance>/`). Skips when the variable is
    unset, fails when it holds no keystore."""
    membership_dir = os.environ.get("E2E_RLN_MEMBERSHIP_DIR")
    if not membership_dir:
        pytest.skip("E2E_RLN_MEMBERSHIP_DIR not set")
    if not any(Path(membership_dir).glob(f"{RLN_MODULE}/*/rln_sealed.json")):
        pytest.fail(
            f"E2E_RLN_MEMBERSHIP_DIR={membership_dir} holds no {RLN_MODULE}/<instance>/rln_sealed.json"
        )
    return Path(membership_dir)


@pytest.fixture(scope="session")
def fleet_membership(
    rln_membership: Path,
    tmp_path_factory: pytest.TempPathFactory,
    request: pytest.FixtureRequest,
) -> Iterator[Path]:
    """A copy of the RLN membership for the nodes that send through the fleet. Each
    sender's keystore is copied back here when its test ends, so the next sender goes
    on from its `message_id`s. Across runs, which restore the same counters, the file
    named by E2E_FLEET_EPOCH_FILE holds the last epoch a run sent in, and this run
    sends only in a later one; without the file it waits for the next epoch."""
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


def _worker_subnet(worker_id: str) -> str:
    """The xdist worker's /24 of NETWORK_SUBNET: worker gw<n> takes the n-th, a run
    without xdist the first."""
    index = 0 if worker_id == "master" else int(worker_id.removeprefix("gw"))
    return str(list(ipaddress.ip_network(NETWORK_SUBNET).subnets(new_prefix=24))[index])


@pytest.fixture(scope="session")
def shared_docker_network(_e2e_env_or_skip: tuple[str, Path], worker_id: str) -> Iterator[str]:
    name = f"logosdelivery-e2e-{uuid.uuid4().hex[:8]}"
    _create_network(name, "--subnet", _worker_subnet(worker_id))
    try:
        yield name
    finally:
        _remove_network(name)


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
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[Callable[..., DeliveryNode]]:
    """`delivery_node_factory` for nodes with RLN on the LEZ testnet registry: each
    daemon gets a presets file in its persistence dir, named by
    LOGOS_DELIVERY_RLN_PRESETS. A node given `membership` starts with a copy of it
    there. Copies of one membership share its `message_id` counters, so two nodes
    started from it must never send through the same validator."""
    image, modules_dir = _e2e_env_or_skip

    with ExitStack() as stack:
        def _create(
            label: str,
            config_json: str,
            *,
            enable_validation: bool,
            membership: Path | None = None,
        ) -> DeliveryNode:
            persistence_dir = tmp_path_factory.mktemp(f"persistence-{label.lower()}")
            if membership:
                shutil.copytree(membership, persistence_dir, dirs_exist_ok=True)
            (persistence_dir / RLN_PRESETS_FILE).write_text(
                make_rln_presets(enable_validation=enable_validation)
            )
            daemon, container_name = _new_daemon(
                image, modules_dir, shared_docker_network, label,
                persistence_dir=persistence_dir,
                env={RLN_PRESETS_ENV: f"/persistence/{RLN_PRESETS_FILE}"},
            )
            stack.enter_context(daemon)
            stack.callback(_save_logs, container_name)
            return setup_rln_node(daemon, config_json, label)

        yield _create


@pytest.fixture
def fleet_node_factory(
    _e2e_env_or_skip: tuple[str, Path],
    _rln_modules_or_skip: None,
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[Callable[..., DeliveryNode]]:
    """Factory for nodes on the logos.test fleet, each in its own docker network so
    that a test's nodes reach each other only through the fleet. A node given
    `enable_validation` gets a presets file whose `logos.test` entry turns proof
    validation on. A node given `membership` starts with a copy of it and copies its
    keystore back when the test ends."""
    image, modules_dir = _e2e_env_or_skip

    with ExitStack() as stack:
        def _create(
            label: str,
            config_json: str,
            *,
            enable_validation: bool = False,
            membership: Path | None = None,
        ) -> DeliveryNode:
            network = f"logosdelivery-fleet-{uuid.uuid4().hex[:8]}"
            _create_network(network)
            stack.callback(_remove_network, network)
            persistence_dir = tmp_path_factory.mktemp(f"persistence-{label.lower()}")
            if membership:
                shutil.copytree(membership, persistence_dir, dirs_exist_ok=True)
            env = None
            if enable_validation:
                (persistence_dir / RLN_PRESETS_FILE).write_text(
                    make_rln_presets(enable_validation=True, preset=FLEET_PRESET)
                )
                env = {RLN_PRESETS_ENV: f"/persistence/{RLN_PRESETS_FILE}"}
            daemon, container_name = _new_daemon(
                image, modules_dir, network, label,
                persistence_dir=persistence_dir,
                env=env,
            )
            stack.enter_context(daemon)
            if membership:
                stack.callback(_copy_keystore_back, container_name, membership)
            stack.callback(_save_logs, container_name)
            return setup_rln_node(daemon, config_json, label)

        yield _create
