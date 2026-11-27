import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_generic_bridge import benchmark_schedule, measure_benchmark, parse_performance_log, prepare_addon, summarize_reports, summarize_samples, verify_catalog_cost, verify_page_cost


class BaselineTests(unittest.TestCase):
    repository = Path(__file__).resolve().parents[1]

    def test_tag_is_extracted_without_modifying_working_addon(self):
        binary = self.repository / "addons/godot_cm/bin/libgodot_cm.linux.editor.x86_64.so"
        before = hashlib.sha256(binary.read_bytes()).hexdigest()
        head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.repository)
        with tempfile.TemporaryDirectory() as directory:
            addons = Path(directory) / "addons"
            addons.mkdir()
            identity = prepare_addon(self.repository, addons, "v0.3.0", True)
            self.assertEqual(identity["plugin_version"], "0.3.0")
            self.assertEqual(identity["source"], "v0.3.0")
            self.assertEqual(len(identity["revision"]), 40)
            copied = addons / "godot_cm/bin/libgodot_cm.linux.editor.x86_64.so"
            self.assertFalse((addons / "godot_cm").is_symlink())
            self.assertEqual(identity["binary_sha256"][copied.name], hashlib.sha256(copied.read_bytes()).hexdigest())
        self.assertEqual(hashlib.sha256(binary.read_bytes()).hexdigest(), before)
        self.assertEqual(subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=self.repository), head)

    def test_frozen_artifact_keeps_binary_identity_and_detects_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory) / "artifact"
            artifact.mkdir()
            identity = prepare_addon(self.repository, artifact, isolated=True)
            (artifact / "identity.json").write_text(json.dumps(identity))
            copied = Path(directory) / "copied"
            copied.mkdir()
            actual = prepare_addon(self.repository, copied, artifact, True)
            self.assertTrue(actual["source"].startswith("artifact:"))
            self.assertEqual(actual["binary_sha256"], identity["binary_sha256"])
            (artifact / "godot_cm/bin/libgodot_cm.linux.editor.x86_64.so").write_bytes(b"modified fixture")
            rejected = Path(directory) / "rejected"
            rejected.mkdir()
            with self.assertRaisesRegex(AssertionError, "artifact changed"):
                prepare_addon(self.repository, rejected, artifact, True)

    def test_unknown_ref_fails_before_creating_an_addon(self):
        with tempfile.TemporaryDirectory() as directory:
            addons = Path(directory) / "addons"
            addons.mkdir()
            with self.assertRaises(subprocess.CalledProcessError):
                prepare_addon(self.repository, addons, "refs/tags/nonexistent-benchmark-baseline", True)
            self.assertEqual(list(addons.iterdir()), [])


class StatisticsTests(unittest.TestCase):
    def test_nearest_rank_p95_and_median(self):
        summary = summarize_samples([{"ok": True, "elapsed_ms": value} for value in range(1, 101)])
        self.assertEqual(summary["median_ms"], 50.5)
        self.assertEqual(summary["p95_ms"], 95)
        self.assertEqual(summary["failed"], 0)

    def test_failed_samples_are_not_success_latency(self):
        records = [{"ok": False, "elapsed_ms": 9000, "codes": ["INTERRUPTED"]}, {"ok": True, "elapsed_ms": 2}]
        summary = summarize_samples(records)
        self.assertEqual(summary["failed"], 1)
        self.assertEqual(summary["median_ms"], 2)
        self.assertEqual(summary["samples"], records)
        self.assertIsNone(summarize_samples(records[:1])["p95_ms"])

    def test_warmups_and_failures_are_recorded_without_payloads(self):
        calls = []
        def operation():
            calls.append(1)
            if len(calls) == 3:
                raise TimeoutError("private timeout content")
            return {"ok": True, "result": "private result", "diagnostics": []}
        ticks = iter(range(20))
        result = measure_benchmark(operation, 2, 3, lambda result: isinstance(result, str), clock=lambda: next(ticks))
        self.assertEqual(len(calls), 5)
        self.assertEqual(len(result["warmups"]), 2)
        self.assertEqual(len(result["samples"]), 3)
        self.assertEqual(result["failed"], 1)
        self.assertEqual(result["samples"][0]["codes"], ["TimeoutError"])
        self.assertNotIn("private", str(result))
        self.assertEqual(result["median_ms"], 1000)

    def test_validation_failure_is_preserved(self):
        result = measure_benchmark(lambda: {"ok": True, "result": []}, 0, 1, lambda value: len(value) == 100)
        self.assertEqual(result["failed"], 1)
        self.assertEqual(result["samples"][0]["codes"], ["INVALID_RESULT"])

    def test_report_groups_do_not_mix_profile_modes_or_binaries(self):
        def report(profile, binary, elapsed):
            return {"addon": {"source": "working-tree", "binary_sha256": {"native.so": binary}}, "topology": "minimal", "environment": {"profiling_requested": profile}, "metrics": {"read": {"samples": [{"ok": True, "elapsed_ms": elapsed}]}}, "native_profile": {"records": [{"benchmark_case": "read", "metrics": {"json_encodings": 2}}]}}
        groups = summarize_reports([report("0", "old", 1), report("0", "old", 3), report("1", "old", 100), report("0", "new", 200)])
        self.assertEqual(len(groups), 3)
        self.assertEqual(groups[0]["metrics"]["read"]["median_ms"], 2)
        self.assertEqual(groups[0]["metrics"]["read"]["sample_count"], 2)
        self.assertEqual(groups[0]["native"]["read"]["json_encodings"], 2)
        self.assertNotIn("samples", groups[0]["metrics"]["read"])

    def test_native_summary_excludes_warmups(self):
        report = {"addon":{"source":"working-tree","binary_sha256":{}},"topology":"minimal","environment":{"profiling_requested":"1"},"warmup":1,"metrics":{"page":{"samples":[{"ok":True,"elapsed_ms":1}]}},"native_profile":{"records":[{"benchmark_case":"page","metrics":{"json_encodings":999}},{"benchmark_case":"page","metrics":{"json_encodings":20}}]}}
        self.assertEqual(summarize_reports([report])[0]["native"]["page"]["json_encodings"], 20)

    def test_alternating_schedule(self):
        self.assertEqual(benchmark_schedule("v0.3.0", 3), [(0, "v0.3.0"), (0, None), (1, None), (1, "v0.3.0"), (2, "v0.3.0"), (2, None)])
        self.assertEqual(benchmark_schedule(None, 2), [(0, None), (1, None)])


class ProfilingTests(unittest.TestCase):
    def test_catalog_cost_excludes_maintenance_and_rejects_repeated_discovery(self):
        def record(global_count, reflection_count):
            return {"benchmark_case": "batch_100", "metrics": {"global_discovery": global_count, "script_reflections": reflection_count, "maintenance_global_discovery": 1, "maintenance_script_reflections": 2}}
        verify_catalog_cost({"records": [record(101, 202), record(1, 2)]}, warmups=1)
        with self.assertRaises(AssertionError):
            verify_catalog_cost({"records": [record(101, 202)]}, warmups=0)
        with self.assertRaises(AssertionError):
            verify_catalog_cost({"records": []}, warmups=0)

    def test_native_scope_partitions_without_engine_callbacks(self):
        repository = Path(__file__).resolve().parents[1]
        code = '''
#include "performance.hpp"
#include <cassert>
#include <thread>
using namespace godot_cm;
int main() {
    { PerfScope outer(PerfMetric::REQUEST_NS);
      { PerfScope inner(PerfMetric::CONTEXT_NS); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
      outer.stop(); outer.stop(); }
    auto data = Performance::snapshot();
    auto request = (size_t)PerfMetric::REQUEST_NS, context = (size_t)PerfMetric::CONTEXT_NS;
    if (Performance::enabled()) {
        assert(data.values[request] >= data.values[context] && data.values[context] > 0);
        assert(data.values[request] == data.exclusive[request] + data.values[context]);
        assert(data.values[context] == data.exclusive[context]);
    } else { for (auto value : data.values) assert(value == 0); for (auto value : data.exclusive) assert(value == 0); }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "profile-scope-test"
            subprocess.run(["c++", "-std=c++17", "-O2", "-pthread", "-x", "c++", "-", "-I", str(repository / "src"), "-I", str(repository / "godot-cpp/include"), "-I", str(repository / "godot-cpp/gdextension"), "-I", str(repository / "build/godot-cpp/gen/include"), "-x", "none", str(repository / "build/bin/libgodot-cpp.linux.editor.x86_64.a"), "-o", str(executable)], input=code, text=True, check=True)
            for active in ("0", "1"):
                subprocess.run([str(executable)], env=dict(os.environ, GODOT_CM_PROFILE=active), check=True)

    def record(self, **changes):
        data = {"schema_version": 1, "kind": "execute", "timing_semantics": "inclusive; cross-thread intervals can overlap", "metrics": {"page_items": 256, "json_bytes": 1000}}
        data.update(changes)
        return "[godot-cm-perf] " + json.dumps(data)

    def test_bounded_records_and_omission_count(self):
        result = parse_performance_log("\n".join(["unrelated output", self.record(), self.record(), self.record()]), limit=2)
        self.assertEqual(len(result["records"]), 2)
        self.assertEqual(result["omitted"], 1)
        self.assertEqual(result["invalid"], 0)

    def test_private_or_malformed_fields_are_not_retained(self):
        lines = [self.record(private="secret project content"), self.record(metrics={"private": "secret"}), self.record(kind="secret"), self.record(metrics={"page_items": "secret"}), "[godot-cm-perf] {broken"]
        result = parse_performance_log("\n".join(lines))
        self.assertEqual(result["records"], [])
        self.assertEqual(result["invalid"], 5)
        self.assertNotIn("secret", str(result))

    def test_bad_numeric_values_and_oversized_records_are_rejected(self):
        lines = [self.record(metrics={"page_items": -1}), self.record(metrics={"page_items": True}), self.record(metrics={"page_items": 1.5}), "[godot-cm-perf] " + "x" * 9000]
        result = parse_performance_log("\n".join(lines))
        self.assertEqual(result["invalid"], 4)
        self.assertEqual(result["records"], [])

    def test_uninstrumented_binary_has_no_metrics(self):
        self.assertEqual(parse_performance_log("normal startup output"), {"records": [], "omitted": 0, "invalid": 0, "process_pump_max_ns": 0, "latest_gauges": {}})

    def test_page_cost_rejects_real_prefix_encoding_counts(self):
        def profile(items, encodings):
            return {"records": [{"benchmark_case": f"passive_page_{items}", "metrics": {"page_items": items, "json_encodings": encodings}}]}
        for items in (16, 64, 256):
            with self.assertRaises(AssertionError):
                verify_page_cost(profile(items, 2 * items + 2), warmups=0)
            verify_page_cost(profile(items, items + 4), warmups=0)

    def test_latest_gauges_survive_record_omission(self):
        records = [self.record(metrics={"catalog_live": 3, "catalog_peak": 3, "script_cache_entries": 4}), self.record(metrics={"catalog_live": 2, "catalog_peak": 3, "script_cache_entries": 4})]
        result = parse_performance_log("\n".join(records), limit=1)
        self.assertEqual(result["latest_gauges"], {"catalog_live": 2, "catalog_peak": 3, "script_cache_entries": 4})
        self.assertEqual(result["omitted"], 1)
        self.assertEqual(result["invalid"], 0)

    def test_maximum_survives_record_omission(self):
        result = parse_performance_log("\n".join(self.record(metrics={"process_pump_max_ns": value}) for value in (1, 999)), limit=1)
        self.assertEqual(len(result["records"]), 1)
        self.assertEqual(result["omitted"], 1)
        self.assertEqual(result["process_pump_max_ns"], 999)


if __name__ == "__main__":
    unittest.main()
