import argparse
import shutil
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bridge_harness import BridgeSession, boot_editor


def pytest_addoption(parser):
    parser.addoption("--godot", default=shutil.which("godot") or "godot")
    parser.addoption("--terrain-addon", default=None)
    parser.addoption("--write-declarations", action="store_true")


def _args(config):
    return argparse.Namespace(
        godot=config.getoption("--godot"),
        terrain_addon=config.getoption("--terrain-addon"),
        write_declarations=config.getoption("--write-declarations"),
        baseline_tag=None,
        benchmark=False,
        benchmark_topology="minimal",
        benchmark_profile=None,
        benchmark_rendered=False,
        benchmark_display_driver=None,
        catalog_lifecycle=False,
    )


@pytest.fixture(scope="session")
def session(request):
    args = _args(request.config)
    with boot_editor(args) as boot:
        yield BridgeSession(boot.client, boot.project, terrain=bool(args.terrain_addon))


@pytest.fixture(scope="session")
def denied_session(request):
    args = _args(request.config)
    with boot_editor(args, denied=True) as boot:
        yield BridgeSession(boot.client, boot.project, denied=True, terrain=bool(args.terrain_addon))
