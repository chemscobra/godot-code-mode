"""Entry point for the Godot CM integration suite.

The default (no-mode) run executes the pytest suite in tools/test_bridge/.
The harness and the MCP client live in tools/bridge_harness.py; this file
only keeps the legacy modes that are not pytest tests (inventory,
benchmarks, catalog lifecycle, frozen artifacts, declaration output).
"""

import argparse
import hashlib
import json
import math
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bridge_harness import (
    benchmark_schedule,
    prepare_addon,
    run,
    summarize_reports,
    verify_variant_inventory,
)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--godot", default=shutil.which("godot") or "godot")
    parser.add_argument("--terrain-addon")
    parser.add_argument("--inventory-only", action="store_true")
    parser.add_argument("--write-declarations", action="store_true")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--catalog-lifecycle", action="store_true")
    parser.add_argument("--verify-worker-semantics", action="store_true")
    parser.add_argument("--summarize-benchmark", type=Path)
    parser.add_argument("--freeze-addon", type=Path)
    baseline = parser.add_mutually_exclusive_group()
    baseline.add_argument("--baseline-021", action="store_true")
    baseline.add_argument("--baseline-tag")
    baseline.add_argument("--compare-baseline")
    baseline.add_argument("--compare-artifact", type=Path)
    baseline.add_argument("--compare-profile", action="store_true")
    parser.add_argument("--benchmark-profile", choices=("0", "1"))
    parser.add_argument("--benchmark-rendered", action="store_true")
    parser.add_argument("--benchmark-display-driver", choices=("x11", "wayland"))
    parser.add_argument("--benchmark-verify-cost", action="store_true")
    parser.add_argument("--benchmark-verify-pages", action="store_true")
    parser.add_argument("--benchmark-warmup", type=int, default=10)
    parser.add_argument("--benchmark-samples", type=int, default=60)
    parser.add_argument("--benchmark-rounds", type=int, default=3)
    parser.add_argument("--benchmark-topology", choices=("minimal", "scripts", "scene1000", "all"), default="minimal")
    parser.add_argument("--benchmark-case", action="append")
    parser.add_argument("--benchmark-timeout", type=int)
    parser.add_argument("--benchmark-idle-seconds", type=float, default=1)
    parser.add_argument("--benchmark-startup-only", action="store_true")
    parser.add_argument("--benchmark-output", type=Path)
    options, pytest_args = parser.parse_known_args()
    if options.freeze_addon:
        if options.freeze_addon.exists() or not options.freeze_addon.parent.is_dir():
            parser.error("frozen addon requires a new path under an existing parent directory")
        options.freeze_addon.mkdir()
        identity = prepare_addon(Path(__file__).resolve().parents[1], options.freeze_addon, isolated=True)
        (options.freeze_addon / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
        print(json.dumps(identity, indent=2))
        raise SystemExit(0)
    if options.summarize_benchmark:
        print(json.dumps(summarize_reports(json.loads(options.summarize_benchmark.read_text())["reports"]), indent=2))
        raise SystemExit(0)
    if options.verify_worker_semantics and not options.benchmark:
        parser.error("worker semantic comparison requires --benchmark (and supports --benchmark-startup-only)")
    if options.catalog_lifecycle and (options.benchmark or options.write_declarations):
        parser.error("catalog lifecycle fixtures cannot be combined with benchmarks or shipped declarations")
    options.harness_sha256 = hashlib.sha256((Path(__file__).resolve().parent / "bridge_harness.py").read_bytes()).hexdigest()
    if options.baseline_021:
        options.baseline_tag = "v0.2.1"
    if (options.baseline_tag or options.compare_baseline or options.compare_artifact or options.compare_profile) and not options.benchmark:
        parser.error("a baseline or paired comparison requires --benchmark")
    if options.compare_profile and options.benchmark_profile is not None:
        parser.error("a paired profile comparison controls the profile flag itself")
    if options.benchmark and options.write_declarations:
        parser.error("benchmarks cannot generate declarations in the working addon")
    if options.benchmark_samples < 1 or options.benchmark_rounds < 1 or options.benchmark_warmup < 0 or not math.isfinite(options.benchmark_idle_seconds) or options.benchmark_idle_seconds < 0:
        parser.error("sample/round counts must be positive; warmup/idle must be nonnegative")
    if options.compare_baseline == "v0.2.1":
        parser.error("0.2.1 uses incompatible APIs; run --baseline-021 as a separate historical benchmark")
    if options.benchmark_output and (not options.benchmark or not options.benchmark_output.parent.is_dir()):
        parser.error("benchmark output requires --benchmark and an existing parent directory")
    verify_variant_inventory(Path(__file__).resolve().parents[1])
    if options.inventory_only:
        raise SystemExit(0)
    if options.benchmark:
        reports = []
        topologies = ("minimal", "scripts", "scene1000") if options.benchmark_topology == "all" else (options.benchmark_topology,)
        for number, source in benchmark_schedule(options.compare_baseline or options.compare_artifact or ("profile-off" if options.compare_profile else None), options.benchmark_rounds):
            for topology in topologies:
                current = argparse.Namespace(**dict(vars(options), baseline_tag=source if options.compare_baseline or options.compare_artifact else options.baseline_tag, benchmark_round=number, benchmark_topology=topology, benchmark_profile=("0" if source else "1") if options.compare_profile else options.benchmark_profile))
                reports.append(run(current))
                if options.verify_worker_semantics:
                    assert reports[-1]["semantics"] == reports[0]["semantics"], "worker result/checkpoint semantics differ across compared variants"
                if options.benchmark_output:
                    options.benchmark_output.write_text(json.dumps({"schema_version": 1, "reports": reports}, indent=2) + "\n")
        raise SystemExit(0)
    if options.catalog_lifecycle:
        run(options, denied=True)
        run(options)
        raise SystemExit(0)
    import pytest

    test_dir = Path(__file__).resolve().parent / "test_bridge"
    command = [str(test_dir), "--godot", options.godot]
    if options.terrain_addon:
        command += ["--terrain-addon", options.terrain_addon]
    if options.write_declarations:
        command += ["--write-declarations"]
    raise SystemExit(pytest.main(command + list(pytest_args)))
