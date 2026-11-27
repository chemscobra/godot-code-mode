# Benchmarks — MCP code-mode vs .tscn file editing

Reproducible with `tools/bench_scene_build.py` against a live editor.
Scenario: "add N MeshInstance3D children to the edited scene and save".

## Measured results (Godot 4.7.2, linux-x86_64, N=20)

| Metric                    | MCP `execute`        | Agent edits `.tscn`     |
|---------------------------|----------------------|-------------------------|
| Agent-emitted text        | ~102 tokens of JS    | ~440 tokens of scene    |
| Round-trips               | 1 `tools/call`       | ~3 (write, reload,      |
|                           |                      | verify)                 |
| Wall time                 | 886 ms               | editor reload + manual  |
|                           | (7.3 ms/op × 121)    | verification            |
| Reversibility             | 1 editor Undo step   | none — file overwrite   |
| Partial-failure safety    | rollback leaves 0    | syntax/uid error breaks |
|                           | partial nodes        | the whole file          |
| Ground truth              | live edited scene,   | text the agent must     |
|                           | real ObjectIDs       | keep consistent itself  |

## Reading the numbers

- **Per-op latency is frame-bound** (design OQ3): ~7 ms/op while the editor
  renders, ~100 ms/op when idle (low-processor mode). Awaited ops are the
  only sequential cost; pure JS work is sub-millisecond.
- **Token footprint**: the JS the agent emits scales linearly like scene
  text, but it never has to author uids, ext_resource ids, or scene-format
  syntax — the classes of error that make hand-edited `.tscn` fail to load.
- **Correctness surface**: file editing bypasses `EditorUndoRedoManager`,
  ownership rules, and dirty-state tracking entirely. The MCP path gets
  undo grouping, rollback, and external-change detection for free.
- **Verification**: after a file write the agent must re-load and re-inspect
  to confirm success (extra round-trips); `execute` returns structured
  `changes` as ground truth in the same call.

## Scenario 2 — Neon Arena (elaborate game level, N=34 nodes)

Built via `examples/neon_arena/build_arena.js` on Godot 4.7.2:
arena floor + glowing walls, 12 pillars, player sphere, 12 collectible
cells, 8 patrolling sentries, HUD layer, sun + camera. A playable
win/lose game (script pre-attached to the scene root).

| Metric                    | MCP `execute`        | Agent edits `.tscn`     |
|---------------------------|----------------------|-------------------------|
| Agent-emitted text        | ~925 tokens of JS    | ~3,500 tokens of scene  |
|                           | (94 lines)           | format (~350 lines: 34  |
|                           |                      | nodes + ~44 subresources|
|                           |                      | with generated uids)    |
| Round-trips               | 1 `tools/call`       | ~3 (write, reload,      |
|                           |                      | verify + fix cycle)     |
| Ops                       | 477 bridge ops       | —                       |
| Wall time                 | 3.4 s (~7 ms/op,     | reload + manual         |
|                           | rendering active)    | verification            |
| Reversibility             | 1 editor Undo step   | none                    |
| Sub-resource handling     | handles minted per   | agent must author       |
|                           | object; no uids      | SubResource ids + refs  |
|                           | written by hand      | by hand                 |
| Error surface             | structured diag per  | one bad line breaks     |
|                           | op, rollback         | the whole scene file    |

The JS script stays roughly linear in the scene's complexity because it
*describes intent* (loops, helpers, tagged types) while the `.tscn` grows
with physical layout — every node, every sub-resource, every uid.

## Scenario 3: `godot.batch` vs awaited ops

Same op-dense workload, one dispatch: `godot.batch` with 200 `create`
elements vs 200 sequential `await`s.

| Metric              | 200 awaited ops            | `godot.batch` (200)  |
|---------------------|----------------------------|----------------------|
| Main-thread frames  | ~1 per op (~200 frames)    | 1                    |
| Wall (rendering)    | ~1.4 s @ ~7 ms/op          | **0.02 s measured**  |
| Wall (idle editor)  | ~20 s @ ~100 ms/op         | ~0.02 s              |
| Result shape        | 200 awaits                 | ordered `{ok,result}`|

Measured on the same build: batch executes all elements inside a single
drain tick, so frame-bound latency disappears entirely. Per-element errors
are isolated — a failing element marks only its own result.

## Suggested scenarios to extend

- `search` vs reading `class_*.xml` docs: index hit vs multi-MB doc scan.
- Introspection-heavy tasks (`get_children` walk) where file parsing must
  reconstruct the node tree from text.
- Conflict scenario: user edits the scene mid-write — detected via undo
  version vs silently clobbered file.

Honest caveat: code-mode wins on safety and ground truth, and `godot.batch`
now closes the raw-speed gap too — sequential `await`s remain the readable
default, but op-dense scripts should prefer batch.

## Data-reliability codec comparison against 0.2.1

Godot 4.7.2 Linux x86-64, isolated headless editor projects, five samples per
operation (median end-to-end milliseconds). Activation and readiness are
separate measurements; declaration generation is disabled in these runs.

| Metric | Tagged 0.2.1 binary | Reliability implementation |
|---|---:|---:|
| Plugin activation checkbox | 7 ms | 8 ms |
| MCP readiness after activation | 819 ms | 874 ms |
| Direct service startup | 727 ms | 785 ms |
| Batch of 100 position reads | 214.553 ms | 296.770 ms |
| PackedStringArray, 128 elements | 6.575 ms / opaque 48-byte result | 6.771 ms / inline 1061-byte result |
| PackedStringArray, 10,000 elements | 7.223 ms / opaque 49-byte result | 7.420 ms / opaque 65-byte result |
| 256 strings via a trusted fixture script | 6.691 ms | 7.204 ms |
| Passive 256-string value page | unavailable | 14.366 ms / 2186-byte result |

The checked-in 0.2.1 batch implementation cannot resolve ordinary nested
object handles. Its benchmark uses the legacy internal `$handle` representation
of an already-issued handle, solely to measure the native batch path. The new
implementation uses normal opaque object handles; the old internal form is not
a new public API recommendation. The baseline does not have passive pagination;
the script-page row measures an explicitly authorized generic fixture method,
not an equivalent built-in read API.

The 100-read batch regressed by about 38% in this sample. Batching still uses
one bridge dispatch, not a dispatch per vector component, but native metadata,
validation and encoding have costs; these results do not establish a speedup.
Large collections remain opaque instead of expanding 10,000 strings into JSON.
Page and inline output limits are 256 elements/16 KiB. Results vary with editor
load, rendering and CPU scheduling; these are observations, not latency limits.

Reproduce without opening an example project:

```bash
python3 tools/test_generic_bridge.py --benchmark --baseline-021
python3 tools/test_generic_bridge.py --benchmark
```

The baseline command extracts the tagged addon into its temporary test project;
it does not checkout the tag or overwrite the working addon. Normal integration
also checks count/memory quotas, exact checkpoint boundaries and bounded output
with an explicitly incomplete effect list.

## Reproducible 0.3.0 performance protocol

The current harness copies the working addon for benchmark runs and extracts a
selected baseline into a separate temporary project. It records the resolved
commit, declared/MCP version, SHA-256 of each native binary, harness hash, engine,
platform, renderer, topology, requested profiling and timeouts. It never checks
out a tag or generates declarations in the working addon during a benchmark.
Binary hashes identify the build; build flags not embedded in an archived binary
are reported as unavailable rather than guessed.

```bash
python3 -B -m unittest discover -s tools -p test_bridge_benchmark.py -v
python3 -B tools/test_generic_bridge.py --benchmark --baseline-tag v0.3.0 --benchmark-topology all --benchmark-output /tmp/godot-cm-baseline-030.json
python3 -B tools/test_generic_bridge.py --benchmark --compare-baseline v0.3.0 --benchmark-topology all --benchmark-output /tmp/godot-cm-comparison.json
```

Defaults are 10 discarded warmups and 60 measured requests per case, in three
rounds. Comparisons alternate baseline/candidate, candidate/baseline, then
baseline/candidate. Each process uses an isolated project and port; topology is
`minimal` by default, `scripts` adds 20 preloaded script classes, and `scene1000`
adds 1000 child nodes. All topologies retain the common integration fixtures,
including their named and anonymous scripts; minimal does not mean no scripts.
`--benchmark-topology all` runs all three. `--benchmark-case NAME` can be repeated
for focused experiments; omitted cases are not evidence for the full matrix.

Cases include batches of 1/10/100/500, native get/set/call, a mixed success/error
batch, small/large packed strings, passive pages of 16/64/256 and dictionary pages
at offsets 0 and 9000. Returned large packed references are released outside the
measured interval to avoid exhausting quotas merely by repeating the benchmark.
Main setup references are retained for page tests and released at the end.

The JSON output retains every warmup and sample, including failures, elapsed time,
validation outcome, diagnostic codes and JSON result size, without storing
argument/result contents. Median and nearest-rank p95 use successful samples;
failed counts and raw failed durations remain present. A run with failures is not
proof of a speedup. No-success series have null median/p95. Encoded result sizes
refer to compact result JSON, not the entire HTTP response or both MCP envelopes.
`--benchmark-output` is updated after every completed process; without it, full
reports are printed to stdout. Its parent directory must already exist.

For an inexpensive harness smoke test, use `--benchmark-warmup 1
--benchmark-samples 2 --benchmark-rounds 1`; the report marks this sampling as
noncompliant with the full performance protocol. `--benchmark-timeout MS` selects
an explicit execute timeout equally for a baseline/candidate comparison; omitted
uses the server default. Failures and timeouts must not be discarded or hidden
by raising product limits.

Activation checkbox, readiness and direct service startup are separate values.
For independent cold-start observations use `--benchmark-startup-only
--benchmark-rounds 10` (and optionally `--compare-baseline v0.3.0`). Do not infer a
robust startup p95 from ten processes. Each run also samples one second of idle
process CPU time on Linux; change this with `--benchmark-idle-seconds`. Unsupported
platforms report idle CPU as null. Idle CPU is not main-thread occupancy, and HTTP
latency is not an editor pause. Until native profiling is implemented, occupancy
and profiling availability are explicitly unknown; an environment flag alone
does not establish that an archived binary supports profiling.

`--baseline-021` remains a historical five-sample, no-warmup measurement using its
legacy target workaround, not the compatible 0.3.0 protocol. It is rejected as a
paired comparison baseline. No new performance gain is claimed by these harness
changes alone.

### Captured uninstrumented baseline

The first complete capture used Godot `4.7.2.stable.arch_linux.ed1daf0bf`, Linux
x86-64, 16 logical CPUs, headless `gl_compatibility`, declarations disabled and
no profiling. The tag resolved to `b483d41a8b243ea2a2b8773a8eae1813d5fe952d`;
Linux binary SHA-256 was
`73d5c52147fb737405350f3b7570a49f00bf26db0e7e9d74af37c48dcb1bdc09`.
The harness hash was
`e4e1f8a80486f179f43a97a5c95ddbdfcb0f7b6469b6c0df450f98a316e75f30`.

Nine completed processes cover three rounds of all three topologies. Every one
of the 15 cases has 10 recorded warmups and 60 measured samples per process:
8100 measured requests and 1350 warmups, with no functional-validation failures
or request timeouts. Topology assertions confirmed 1000 scene children and 20
additional script classes where requested. These are end-to-end HTTP timings,
not measurements of continuous main-thread occupancy.

| Topology | Round | Batch 100 median / p95 (ms) | Batch 500 median / p95 (ms) |
|---|---:|---:|---:|
| Minimal | 1 | 285.259 / 303.681 | 1421.694 / 1496.552 |
| Minimal | 2 | 230.011 / 248.055 | 1122.128 / 1442.752 |
| Minimal | 3 | 225.439 / 233.555 | 1105.138 / 1132.096 |
| 20 extra scripts | 1 | 371.631 / 477.370 | 1810.682 / 2263.934 |
| 20 extra scripts | 2 | 367.740 / 389.563 | 1558.748 / 1928.822 |
| 20 extra scripts | 3 | 312.152 / 320.449 | 1530.876 / 1704.371 |
| 1000 children | 1 | 304.697 / 404.310 | 1329.565 / 1402.191 |
| 1000 children | 2 | 266.038 / 273.932 | 1304.137 / 1318.830 |
| 1000 children | 3 | 265.986 / 275.723 | 1298.304 / 1471.248 |

Passive 256-string page medians ranged from 13.812 to 14.074 ms; dictionary page
medians ranged from 43.821 to 47.768 ms at offset 0 and 46.761 to 50.356 ms at
offset 9000. These ranges are across process-level medians, not pooled p95s.
The variation between rounds reinforces the need for alternating comparisons;
none of these observations establishes an optimization gain or a causal phase.

A separate series of ten independent fixture processes recorded checkbox latency
6–9 ms (median 7), readiness 775–911 ms (median 786), and direct service startup
713–802 ms (median 723.5). The fixture also performs activation/disable probes;
these values must retain those labels, not be advertised as a pure untouched
engine cold-start measurement. Startup p95 remains inconclusive at this size.
Idle process CPU is recorded independently in every raw report; it cannot be
converted into main-thread occupancy.

Raw session artifacts are `/tmp/godot-cm-perf-baseline-030-full.json` and
`/tmp/godot-cm-perf-baseline-030-startup.json`. They contain all individual samples,
errors, result byte counts and environment metadata and can be regenerated with
the commands above. These temporary paths are not packaged release artifacts.

### Development profiling in progress

`GODOT_CM_PROFILE=1` enables aggregate native counters and nanosecond timers on
the development log channel `[godot-cm-perf]`; no MCP tool or result field is added.
The current reader accepts only fixed metric names, numeric counters and fixed
record kinds, retains at most 512 records per process, counts rejected/omitted
records, and preserves the process pump maximum even when records are omitted.
It never copies arbitrary log fields into the benchmark report.

Both instrumented binaries compile. The Linux smoke matrix passes with profiling
enabled in all three topologies, and the full generic integration passes with it
disabled and enabled. The native scope test checks exact inclusive/exclusive
partitioning and idempotent stop, without initialized engine callbacks, for both
flag values. It requires the configured Linux editor SDK library in `build/bin`
and `c++`. Same-thread nested scope time is subtracted for exclusive metrics;
main-thread and worker wall intervals can still overlap and are not CPU time.
Per-request counter deltas can straddle a main-thread pump completion; cumulative
records and the separate frame sampler must be used for completed pump spans.

The runtime privacy marker traverses a checkpoint, dictionary conversion and a
script invocation. It does not occur on the profiling channel. A 1000-request
stress run retained 512 records, explicitly omitted 511 and rejected zero;
the pump maximum survives omission. Generic integration passes with profiling
off and on, including passive-data rejection/getter regressions. Windows was
cross-built, not runtime-tested.

### Instrumentation-only attribution before optimization

The measured instrumentation-only Linux binary had SHA-256
`439813df563db926ddadbdc84c48511631caf89deb0e19bf8b1a7ae5fc5304e8`.
No catalog/page/worker algorithm optimization was present. Two independent paired
experiments used three alternating rounds, 10 warmups and 60 samples per case.
Each table cell below pools the 180 successful measured requests for its side.
All measured requests passed. Headless engine/renderer conditions match the
baseline above; do not compare separate experiments as if they were paired.

| Case | Tag 0.3.0, profile off median / p95 | Instrumented, profile off median / p95 |
|---|---:|---:|
| Batch 100 | 269.325 / 306.714 ms | 262.609 / 302.927 ms |
| Native get | 7.181 / 7.464 ms | 7.134 / 7.486 ms |
| Packed 128 | 6.599 / 7.708 ms | 6.663 / 7.711 ms |
| Passive page 256 | 13.610 / 16.287 ms | 13.749 / 16.170 ms |

| Case | Same instrumented binary, profile off median / p95 | Profile on median / p95 |
|---|---:|---:|
| Batch 100 | 297.664 / 312.737 ms | 267.826 / 307.520 ms |
| Native get | 7.223 / 7.446 ms | 7.250 / 11.979 ms |
| Packed 128 | 6.702 / 12.910 ms | 6.712 / 7.815 ms |
| Passive page 256 | 13.589 / 16.365 ms | 13.554 / 16.478 ms |

Inactive instrumentation changed medians by approximately -2.5% to +1.0% in its
paired comparison. Active profiling includes additional clocks, atomics, UTF-8
byte accounting and aggregate log I/O. Frame scheduling/noise prevents deriving
a single CPU-overhead percentage from these HTTP timings: a faster batch median
with profiling is not a gain. The native-get p95 increase with profiling is real
in this sample and must not be hidden. Final optimization comparisons must use
profiling off, with structural counters collected in separate profiling runs.

The profiling run attributes the repeated work directly (medians across retained
request summaries, including warmups; inclusive timers are not additive):

- Batch 100: 101 global discoveries and 202 script reflections, about 257.927 ms
  in catalog work, 0.092 ms native invocation and 0.261 ms result encoding, against
  about 266.217 ms worker request wall time. The extra global discovery can be
  periodic maintenance. Later development counters separate maintenance explicitly.
- Native get: one global discovery and two script reflections per request; the
  catalog interval was about 3.331 ms against 0.0024 ms native invocation.
- Passive page 256: 514 JSON encodings and 290214 encoded bytes inspected per
  request, about 9.177 ms in the page interval. No catalog discovery was needed.
  Those bytes count repeated size checks, not bytes delivered over HTTP.
- Packed 128: three JSON encodings and 3441 encoded bytes inspected per request;
  the native result remains a 1061-byte compact tagged result, not three payloads.

Reproduce and inspect without adding an MCP tool:

```bash
python3 -B tools/test_generic_bridge.py --benchmark --compare-baseline v0.3.0 --benchmark-profile 0 --benchmark-case batch_100 --benchmark-case native_get --benchmark-case packed_128 --benchmark-case passive_page_256 --benchmark-output /tmp/godot-cm-perf-instrumentation-disabled.json
python3 -B tools/test_generic_bridge.py --benchmark --compare-profile --benchmark-case batch_100 --benchmark-case native_get --benchmark-case packed_128 --benchmark-case passive_page_256 --benchmark-output /tmp/godot-cm-perf-instrumentation-overhead.json
python3 -B tools/test_generic_bridge.py --summarize-benchmark /tmp/godot-cm-perf-instrumentation-overhead.json
```

`--compare-profile` alternates 0/1, 1/0, 0/1 using the same working binary; it
cannot be combined with a baseline selector or a fixed `--benchmark-profile`.
The summarizer groups by source, topology, profile flag and binary hashes rather
than merging incompatible builds. Native metric medians include retained warmup
requests; encoded latency medians/p95 exclude warmups and retain failures.

### Rendered editor and continuous process-frame span

`--benchmark-rendered` opens only the temporary fixture editor, requests editor
UI redraws while sampling, and records bounded raw wall intervals between fixture
process callbacks. Two `SceneTree.process_frame` observers connect before/after
the bridge pump and bracket its continuous signal work. Their connections are
removed when the fixture exits. This continuous span excludes rendering and
other frame work outside the bracketing signal; it is not total editor CPU time.
The interval series includes rendering/waiting/scheduling as well as bridge work.
Both series are bounded at 100000 observations; omissions are explicit.

```bash
python3 -B tools/test_generic_bridge.py --benchmark --benchmark-rendered --benchmark-profile 1 --benchmark-case batch_100 --benchmark-case native_block_30ms --benchmark-warmup 1 --benchmark-samples 3 --benchmark-rounds 1 --benchmark-output /tmp/godot-cm-perf-rendered-continuous.json
```

The Linux Wayland/X11-session smoke run used headless=false and the temporary
compatibility-renderer project. It drew 17 frames. Batch-100 HTTP median was
225.832 ms, while the maximum sampled frame interval was 232.535 ms and maximum
continuous process-frame signal span was 228.889 ms. The generic fixture's
explicitly authorized 30 ms blocking method had HTTP median 38.241 ms; native
invocation timers identify the non-interruptible call separately. Three measured
requests are functional/sampler evidence, not the full latency protocol or a
reliable rendered p95 comparison. The extra blocking method is test-only; it
changes neither product budgets nor batch scheduling.

`main_thread_occupancy_ms` is explicitly labelled as the process-lifetime pump
maximum, which can include work before frame sampling. Never substitute it for
the sampled-workload `continuous_process_frame_max_ms`, and never substitute
HTTP latency for either. Frame sampling includes setup, warmups, measurements and
cleanup, not just the measured requests.

### Structural catalog regression

`--benchmark-verify-cost` checks measured native batch summaries after discarding
the configured warmups. Separate maintenance counters identify discoveries and
script reflections performed by the periodic pump reconciler, so these are not
mistaken for per-element work. Idle CPU and maintenance timing remain separate
controls. A test verifies both maintenance subtraction and detection of repeated
work. The instrumentation-only 0.3.0 algorithm fails this regression for batch
100 with exactly 100 operation-driven global discoveries and 200 operation-driven
script reflections, independently of the one maintenance discovery/two maintenance
reflections observed in the same request window. This is the intended failing
regression, not a functional API error.

```bash
python3 -B tools/test_generic_bridge.py --benchmark --benchmark-profile 1 --benchmark-verify-cost --benchmark-case batch_100 --benchmark-warmup 1 --benchmark-samples 3 --benchmark-rounds 1 --benchmark-output /tmp/godot-cm-perf-cost-check.json
```

The focused three-sample command checks structure, not D6/D7 latency acceptance.

### Catalog freshness and lifetime checks

Stable native operations reuse the current index and generation-local member
lookups. A periodic main-thread reconciliation still runs at the existing 100 ms
cadence. Native normalized metadata is reused unless native membership, singleton
aliases or extension notifications change. Script targets are checked using
passive script/base metadata and encoded defaults before descriptor reuse.
Potentially mutating dispatched operations also reconcile before returning;
a whole batch is never assumed to have unchanged metadata. Authorization and
argument values are checked per operation, not memoized.

Filesystem/class-list and extension load/unload/reload notifications invalidate
relevant caches. Startup/shutdown and service restart tests count exactly one
pump subscription per signal while active, zero while stopped, and one after
restart. Source-only script changes need no discovery load: same-identity reload
and a Vector3-to-Color signature change inside one authorized batch are covered.
The next operation uses Color, while the request's `api.*` snapshot stays pinned.
Explicitly authorized loads, script replacement, mutable argument objects,
revoked/restored filesystem grants inside a batch and synchronously freed handles
are covered without changing the shipped allowlist.

```bash
GODOT_CM_PROFILE=1 python3 -B tools/test_generic_bridge.py --catalog-lifecycle
```

This optional Linux fixture requires `c++` and the configured editor SDK archive
in `build/bin`. It compiles two tiny test-only extensions in the temporary
project, atomically replaces its own test library, and performs a real extension
reload with the same class name but a changed Vector3/Color signature. No
third-party implementation, product adapter or additional MCP operation is used.
It cannot be combined with benchmark capture or shipped declaration generation.

The run passed the full codec/integration suite, a real native reload and 24
script invalidation/publication cycles. RSS samples after warmup, in KiB, were
`1247408, 1247408, 1247420, 1247424, 1247424, 1247424`. This is resident editor
memory after the broad allocation/quota suite, not the index's isolated size.
The development gauges ended at `catalog_live=2`, `catalog_peak=4`,
`script_cache_entries=5`. Catalog gauges count the mutable index's base plus
published immutable catalog objects; their return to two demonstrates old
request-held generations becoming releasable. They are absolute gauges, not
request deltas. The bounded reader retains their latest values even when earlier
record retention has reached its cap. This fixture evidence is not a D7 latency
acceptance result; the paired catalog matrix is separate.

### Catalog-only paired matrix

Before compiling the page/worker changes, the catalog-only Linux candidate
`e96b592d9b9e5e7bfcd483e32761e6dd71482e042e838700f969fd80c1515c2e`
was compared with the unchanged `v0.3.0` Linux binary. Windows was cross-built,
not run. The same captured harness/fixtures were used for both variants; later
fixture additions are not included in this dataset. Capture commands:

```bash
python3 -B tools/test_generic_bridge.py --benchmark --compare-baseline v0.3.0 --benchmark-profile 0 --benchmark-topology all --benchmark-output /tmp/godot-cm-perf-catalog-comparison.json
python3 -B tools/test_generic_bridge.py --benchmark --benchmark-profile 1 --benchmark-topology all --benchmark-verify-cost --benchmark-case batch_1 --benchmark-case batch_10 --benchmark-case batch_100 --benchmark-case batch_500 --benchmark-case native_get --benchmark-case native_set --benchmark-case native_call --benchmark-case packed_128 --benchmark-output /tmp/godot-cm-perf-catalog-profile.json
```

The unprofiled comparison has three alternating rounds, ten warmups and sixty
measurements per case/variant/topology: 17,280 measured samples and 2,880 warmups,
with no failed functional checks or timeouts. The separate profile series has
4,320 measured samples and 720 warmups. The files contain per-round samples,
identities, environment and flags. The table pools the three rounds for compact
presentation; milliseconds are median / p95, not main-thread CPU time.

| Topology | Case | 0.3.0 | Catalog-only |
|---|---|---:|---:|
| minimal | batch 10 | 31.555 / 34.235 | 6.607 / 7.665 |
| minimal | batch 100 | 253.387 / 267.374 | 6.802 / 11.148 |
| minimal | batch 500 | 1237.507 / 1335.589 | 21.374 / 27.673 |
| scripts | batch 10 | 40.343 / 43.199 | 6.553 / 7.578 |
| scripts | batch 100 | 344.685 / 370.777 | 6.754 / 8.226 |
| scripts | batch 500 | 1674.588 / 1780.398 | 21.222 / 27.380 |
| scene1000 | batch 10 | 35.146 / 37.885 | 6.636 / 7.596 |
| scene1000 | batch 100 | 289.122 / 303.256 | 6.788 / 8.259 |
| scene1000 | batch 500 | 1406.974 / 1478.237 | 22.368 / 27.867 |

The batch improvement occurs in every round/topology, well outside round
variation. Single native get/call remain around 7.0–7.2 ms and native set around
20.9–21.0 ms; these controls do not demonstrate a latency gain. Pages are still
on the old prefix-sizing algorithm in this capture and stay around 7/7/14 ms
for 16/64/256 strings and 43–49 ms for dictionary pages. No page improvement is
attributed to the catalog optimization.

There are real costs, not universal speedups. With twenty extra cached scripts,
`packed_128` changes from 7.392 / 13.991 to 13.735 / 14.345 ms; its bimodal frame
pacing makes the median particularly sensitive. `packed_10000` changes from
11.315 / 12.552 to 12.899 / 14.638 ms. Unknown-effect script calls now reconcile
after dispatch so another operation cannot use a stale signature. Their profile
shows one global reconciliation and 23 script reflections, versus zero of each
for a stable native batch. Catalog time for `packed_128` is 3.763 ms with extra
scripts (2.595 ms minimal, 2.937 ms with 1000 scene nodes); native invocation is
only about 0.004 ms. The bounded reader reaches its 512-record cap before
measured `packed_128` records in this eight-case profile: these particular
packed-call phase figures describe available warmup probes, not measured
acceptance samples. The unprofiled control comparison above uses all sixty
measured samples in each round. A final targeted profile below captures the
control cases without exhausting the reader. The extra control cost is the
conservative freshness path, not the native string-array method. Removing that check to recover the control
median would invalidate the in-batch reload guarantee. Script calls and
invalidation are not advertised as the stable-native fast path.

Cold cache construction also costs more: startup is 790–812 ms versus
704–729 ms in these runs, while checkbox activation stays 6–7 ms and readiness
is 845–885 versus 791–832 ms. This is one-time materialization of reusable
catalog/member data. Checkbox return latency does not establish smooth editor
frames during the subsequent native initialization. These nine starts per variant
are a control observation, not the required final ten-process cold series.
One-second idle windows consume 50–80 ms process CPU for the candidate and
50–70 ms for baseline; their coarse scheduler/clock resolution does not support
a claimed idle improvement. Maintenance still runs, without repeated identical
publication. Startup, script-call and invalidation costs remain explicit limits
of the catalog-only result and must be reviewed again in the final matrix.

Profiled stable batch 100 records zero global discoveries and zero script
reflections in all three topologies after separating maintenance. Median pump
work is 1.17–1.20 ms; batch 500 pump work is 5.07–5.30 ms. Process-lifetime pump
maxima are 10.66–14.63 ms across the nine profiled runs, including setup and
maintenance, not just a selected batch. HTTP/request time is larger and is not
reported as main-thread occupancy. A batch still occupies one dispatch and
calls remain non-preemptible; the final rendered-editor frame series is reported
separately, not inferred from these headless measurements.

### Incremental pages and passive worker result reuse

For attribution, the catalog-only addon was frozen before recompilation with
`python3 -B tools/test_generic_bridge.py --freeze-addon /tmp/godot-cm-catalog-only-artifact`.
The harness copies the addon, records its commit/version/binary hashes, rejects
an existing destination, and verifies the hashes when using `--compare-artifact`.
This is a local test artifact, not a tag, commit, package or replacement for the
primary `v0.3.0` reference. Its report ID is `artifact:2b26dfb33ad7`, with the
catalog-only Linux hash recorded above. The page/worker candidate Linux hash is
`f73b38a26ee324fc117b13336e2934e15f985d76e0d74c9f3d3f857877367fce`;
Windows cross-build hash is
`e0b53685e0fc96356c47485ae8da2d3ec8b52a9ab0538abf167fc587ae016697`.

```bash
python3 -B tools/test_generic_bridge.py --benchmark --compare-artifact /tmp/godot-cm-catalog-only-artifact --benchmark-profile 0 --benchmark-case passive_page_16 --benchmark-case passive_page_64 --benchmark-case passive_page_256 --benchmark-case dictionary_page_0 --benchmark-case dictionary_page_9000 --benchmark-case unicode_page_256 --benchmark-case large_page_3 --benchmark-case worker_strings --benchmark-case worker_integers --benchmark-output /tmp/godot-cm-page-worker-comparison.json
python3 -B tools/test_generic_bridge.py --benchmark --benchmark-profile 1 --benchmark-case passive_page_256 --benchmark-case dictionary_page_0 --benchmark-case dictionary_page_9000 --benchmark-case unicode_page_256 --benchmark-case large_page_3 --benchmark-verify-pages --benchmark-output /tmp/godot-cm-pages-profile.json
python3 -B tools/test_generic_bridge.py --benchmark --compare-artifact /tmp/godot-cm-catalog-only-artifact --benchmark-profile 1 --benchmark-case worker_strings --benchmark-case worker_integers --benchmark-output /tmp/godot-cm-worker-profile.json
```

Each series uses minimal topology, three rounds, ten warmups and sixty measured
samples per case/process. Comparison order alternates baseline/candidate. There
are 3,240 measured samples and 540 warmups in the unprofiled comparison, 900/150
in the page profile and 720/120 in the worker profile, with no functional failures
or timeouts. Native phase summaries exclude warmups and are separate from the
uninstrumented acceptance timings. Raw artifacts retain all available records;
the bounded reader does not make omitted records into zero-cost samples.

| Case | Catalog-only median / p95 ms | Pages + worker median / p95 ms |
|---|---:|---:|
| strings, limit 16 | 7.100 / 7.341 | 7.122 / 7.326 |
| strings, limit 64 | 7.143 / 7.412 | 7.164 / 7.464 |
| strings, limit 256 | 13.984 / 16.121 | 6.660 / 7.728 |
| dictionary, offset 0 | 44.372 / 46.915 | 7.041 / 7.653 |
| dictionary, offset 9000 | 46.856 / 49.916 | 7.227 / 14.005 |
| Unicode/escapes, limit 256 | 27.831 / 33.406 | 6.673 / 7.676 |
| large boundary page, limit 3 | 6.745 / 7.769 | 6.633 / 7.654 |
| worker, 1024 strings + checkpoint | 5.477 / 5.970 | 5.237 / 5.702 |
| worker, 1024 integer records + checkpoint | 13.321 / 14.667 | 12.781 / 13.702 |

The 16/64-element and large-boundary controls do not establish a latency gain.
For the 256-string page, the real counter regression rejected the old binary's
514 JSON encodings and passes the new 260. Profiled bytes encoded per request
fall from the observed 290,214-byte old-prefix regression to 8,568 bytes for the
same 256 strings; response content is unchanged. Page work is about 0.440 ms in
the candidate profile, versus the observed 8.393 ms in the old-prefix regression
sample. Those two phase values are attribution evidence, not an independently
paired latency distribution. Larger-page latency gains occur in all comparison
rounds, while small pages remain near the transport/frame scheduling floor.

Unicode includes multibyte characters, newlines, tabs, quotes and backslashes.
Its page stops at 133 items, not at the requested 256. The large boundary fixture
accepts two strings, tentatively retains the oversized third item, rejects that
addition and rolls its retention back; `next_offset` remains 2. Functional tests
compare page counts/continuation against actual Godot JSON encoding for arrays,
packed strings and Unicode dictionary entries, including empty/end pages. They
also exhaust the reference quota and verify that failure after one tentative
retention leaks nothing and leaves older references valid.

Dictionary seeking remains linear. Candidate median page work is 1.551 ms at
offset 0 and 2.636 ms at offset 9000; the separately scoped initial skip is about
0.00003 ms versus 1.285 ms. Byte sizing no longer re-encodes every accumulated
prefix, but reaching an offset still walks preceding keys. There is no cursor,
full key snapshot or claim of constant-time random access. The deep-offset p95
still shows frame scheduling sensitivity and must not be replaced by the
front-page p95 when describing the result.

Worker-only cases perform no native dispatch in their timed JavaScript bodies,
so the frozen catalog candidate isolates the worker change from catalog reuse
and page formatting. String-case round medians change from
5.481/5.464/5.490 to 5.131/5.138/5.319 ms; integer-record medians change from
13.724/13.266/13.219 to 12.750/12.700/12.873 ms. The modest improvement repeats in
all three alternating rounds; it is not a batch-scale speedup. Profiled request
time changes from 4.165 to 3.895 ms for strings and 9.059 to 8.784 ms for integer
records. Passive decode time itself is not claimed to improve.

The worker retains the authoritative passive Variant, removes the intermediate
QuickJS string round trip and skips JSON parsing on equivalent data. It still
encodes JSON for the exact output-byte check and final envelope budget. INT
values are normalized to the FLOAT representation produced by Godot JSON.parse
before reuse, preserving downstream sizing. Any JavaScript floating-point tag
keeps the old validated parse route, including finite fractional/large numbers
and special float representations; this avoids changing the old formatter's
rounding behavior. It does not promise a new numeric precision contract.

For both worker fixtures the new counters report one reuse and zero parses.
JSON encodings and encoded bytes remain exactly 3 / 94,420 for strings and
3 / 102,268 for integer records, including checkpoint/envelope serialization.
The saving is the removed conversion/parse, not fewer mandatory encodings or
smaller responses. Against `v0.3.0`, canonical results and checkpoints match for
BigInt/int64, special floats, fractional values, void, Unicode/escapes, reserved
keys and nested integer data. Passive accessor/proxy/cycle rejection, primary
errors, bounded checkpoints and global-envelope truncation also pass the full
isolated integration suite. The two semantic comparison captures are
`/tmp/godot-cm-worker-semantics-before.json` and
`/tmp/godot-cm-worker-semantics-after.json`.

### Final combined headless matrix

```bash
python3 -B tools/test_generic_bridge.py --benchmark --compare-baseline v0.3.0 --benchmark-profile 0 --benchmark-topology all --benchmark-output /tmp/godot-cm-perf-final-comparison.json
python3 -B tools/test_generic_bridge.py --summarize-benchmark /tmp/godot-cm-perf-final-comparison.json
```

The final combined binary has the page/worker candidate hashes above. The primary
comparison uses twenty cases, all three verified topologies, three alternating
rounds, ten warmups and sixty measurements per case/process: 21,600 measured
samples and 3,600 warmups in eighteen independent editor processes. All measured
functional checks pass with zero failed samples or timeouts. Both sides use the
same fixture source, grants and requests, without a benchmark timeout override
(`request_timeout_ms: null`, so product defaults apply). The raw
report records environment/engine/platform, binary identity, actual topology,
flags, each warmup/sample/error, activation and idle CPU. The summary's `runs`
contain each round's median/p95; pooling below is only a compact presentation.

| Topology | Case | 0.3.0 median / p95 ms | Combined median / p95 ms |
|---|---|---:|---:|
| minimal | batch 1 | 7.076 / 7.399 | 7.074 / 7.383 |
| minimal | batch 10 | 32.378 / 34.572 | 6.701 / 7.644 |
| minimal | batch 100 | 263.374 / 277.378 | 6.825 / 7.891 |
| minimal | batch 500 | 1294.640 / 1397.288 | 20.941 / 27.213 |
| scripts | batch 1 | 7.079 / 8.438 | 7.088 / 7.333 |
| scripts | batch 10 | 41.956 / 50.261 | 6.677 / 7.572 |
| scripts | batch 100 | 357.083 / 423.470 | 6.768 / 7.820 |
| scripts | batch 500 | 1733.978 / 2027.662 | 21.338 / 27.659 |
| scene1000 | batch 1 | 7.077 / 8.687 | 7.059 / 7.320 |
| scene1000 | batch 10 | 36.306 / 39.062 | 6.603 / 7.573 |
| scene1000 | batch 100 | 305.562 / 317.741 | 6.780 / 7.878 |
| scene1000 | batch 500 | 1478.904 / 1530.940 | 20.623 / 27.596 |

Batch-100 round medians are 260.271–266.000 ms baseline versus 6.774–6.855 ms
combined in minimal topology, 347.173–408.403 versus 6.706–6.813 ms with extra
scripts, and 299.055–307.445 versus 6.778–6.790 ms with 1000 children. For batch
500 the corresponding ranges are 1285.826–1317.145 versus 20.866–21.106 ms,
1700.592–2004.129 versus 21.006–23.127 ms, and 1452.487–1509.123 versus
20.516–21.553 ms. The gain repeats in every round despite baseline dispersion;
no hardware-independent SLA or latency beyond this workload is inferred.
Single reads remain near 7 ms, not zero cost.

| Minimal-topology case | 0.3.0 median / p95 ms | Combined median / p95 ms |
|---|---:|---:|
| mixed valid/error batch | 13.762 / 16.856 | 7.168 / 7.440 |
| passive page 256 | 13.988 / 16.085 | 6.715 / 7.706 |
| dictionary offset 0 | 45.661 / 48.849 | 6.932 / 8.127 |
| dictionary offset 9000 | 47.865 / 51.073 | 7.900 / 14.206 |
| Unicode page | 27.078 / 30.915 | 6.710 / 7.705 |
| worker strings/checkpoint | 5.507 / 6.339 | 5.221 / 5.813 |
| worker integer records/checkpoint | 13.215 / 15.028 | 12.902 / 13.853 |

The same page gains occur in all topologies: 256-string medians are
6.674–6.715 ms versus 13.988–14.028 ms, front dictionary pages are
6.716–7.019 versus 43.917–45.661 ms, and deep pages are 7.381–7.900 versus
46.218–47.865 ms. Deep-page p95 remains about 14.1–14.2 ms because the initial
walk was not removed. Small pages and the two-item large-boundary page remain
controls, not claimed improvements. Worker median gains are modest; p95 is
mixed in some topologies and is not presented as a universal improvement.

The freshness trade-off persists in the combined build. With extra scripts,
packed-128 is 13.760 / 14.344 ms versus 7.387 / 13.899 ms. Packed-10000 is
12.535 / 13.692 versus 11.358 / 12.590 ms; it also regresses in minimal and
1000-child controls. The authorized 30 ms native sleep takes 42.17–43.18 ms
HTTP time versus 40.36–41.47 ms. These unknown-effect calls pay the conservative
post-dispatch catalog check already attributed in the catalog-only profile;
the native method was not made faster and the check was not removed. Ordinary
native get/call stay near 7 ms and set near 21 ms. All controls, including their
p95 and round results, remain in the raw dataset rather than being filtered out.

### Final cold-start and instrumentation controls

```bash
python3 -B tools/test_generic_bridge.py --benchmark --compare-baseline v0.3.0 --benchmark-startup-only --benchmark-profile 0 --benchmark-rounds 10 --benchmark-output /tmp/godot-cm-perf-final-cold.json
python3 -B tools/test_generic_bridge.py --benchmark --compare-profile --benchmark-topology all --benchmark-case batch_100 --benchmark-case batch_500 --benchmark-case native_get --benchmark-case passive_page_256 --benchmark-case worker_strings --benchmark-output /tmp/godot-cm-perf-final-overhead.json
```

Cold startup uses ten independent minimal-topology editor processes per variant,
with alternating order and no warmed request distribution substituted for
startup. There are no startup failures. Values are median [min, max] in ms;
ten starts do not support a robust startup p95.

| Cold metric | 0.3.0 | Combined candidate |
|---|---:|---:|
| checkbox activation | 7 [6, 8] | 7 [6, 7] |
| MCP readiness | 795.5 [778, 862] | 856.5 [847, 896] |
| direct service startup | 704.5 [698, 745] | 798 [779, 824] |

The roughly 94 ms median startup increase is retained, not called an optimization:
reusable catalog/member lookup construction costs cold time to avoid repeated
work. This change does not redesign startup. Equal checkbox latency is not proof
that later native initialization cannot occupy the editor thread. Readiness
also regresses by about 61 ms in this series. One-second post-ready idle windows
consume 50–80 ms process CPU on baseline and 50–70 ms on candidate; coarse
10 ms accounting and process variation do not establish an idle speedup.

The final instrumentation comparison is a separate three-round, ten-warmup,
sixty-measurement series on each topology, with off/on order alternated. Its
5,400 measured samples and 900 warmups pass all functional checks. No baseline
latency was inferred from a profiled-only run. Values below are pooled
median / p95 milliseconds for the same final candidate binary.

| Topology | Case | Profile off | Profile on |
|---|---|---:|---:|
| minimal | batch 100 | 6.809 / 8.100 | 6.836 / 8.317 |
| minimal | batch 500 | 24.642 / 29.112 | 23.744 / 28.458 |
| minimal | native get | 7.068 / 7.380 | 7.146 / 7.446 |
| minimal | page 256 | 6.686 / 7.760 | 6.749 / 7.730 |
| minimal | worker strings | 5.306 / 6.058 | 5.448 / 6.070 |
| scripts | batch 100 | 6.793 / 11.851 | 6.849 / 7.862 |
| scripts | batch 500 | 24.640 / 31.899 | 22.562 / 28.569 |
| scripts | native get | 7.074 / 7.388 | 7.116 / 7.472 |
| scripts | page 256 | 6.706 / 7.743 | 6.735 / 7.790 |
| scripts | worker strings | 5.376 / 6.414 | 5.364 / 6.039 |
| scene1000 | batch 100 | 6.812 / 7.961 | 6.817 / 7.855 |
| scene1000 | batch 500 | 21.081 / 27.396 | 21.241 / 26.980 |
| scene1000 | native get | 7.063 / 7.288 | 7.127 / 7.363 |
| scene1000 | page 256 | 6.687 / 7.653 | 6.725 / 7.776 |
| scene1000 | worker strings | 5.347 / 5.988 | 5.406 / 6.009 |

Small controls show tens to hundreds of microseconds of latency difference.
Batch-500 and p95 off/on differences are scheduling-sensitive and sometimes
reverse sign: they do not mean profiling speeds the program up or has zero
cost. This series does not resolve a single stable overhead percentage for
that path. The independent unprofiled primary comparison remains decisive for
the much larger catalog/page gains. Process-lifetime pump maxima in the nine
profiled runs are 10.73–13.90 ms, including setup/maintenance, not per-batch
latency or renderer time. Internal exclusive/inclusive intervals remain
non-additive across threads. Profiling stays opt-in and off in normal use.

### Final rendered-editor frame series

The first final rendered attempt on the current Wayland session failed the
harness's positive-drawing check: process callbacks ran, but its sampled drawn
frame count was zero. It was not accepted as responsiveness evidence. The
reason for rendering suppression in that session was not established. The
failure stopped that comparison before a valid six-process report was written;
its first-process timings are not folded into the successful series below.
No editor/desktop settings or product limits were changed to bypass the check.

A smoke test on an existing private Xvfb/X11 display path confirmed positive
frame counts for both binaries, then the entire required series was rerun with
the same display for both variants:

```bash
xvfb-run -a -s "-screen 0 1280x720x24" python3 -B tools/test_generic_bridge.py --benchmark --compare-baseline v0.3.0 --benchmark-rendered --benchmark-display-driver x11 --benchmark-profile 0 --benchmark-case batch_100 --benchmark-case batch_500 --benchmark-case native_block_30ms --benchmark-output /tmp/godot-cm-perf-final-rendered-x11.json
```

This uses the real non-headless editor with `gl_compatibility`, X11 and a private
1280x720x24 framebuffer, not the user's desktop/project. There are three
alternating rounds with ten warmups and sixty measurements per case/variant:
1,080 measured samples and 180 warmups, all successful. Each process confirms
positive drawn frames. This tests rendering and main-loop contention, not
physical monitor scanout, real-desktop FPS or a hardware-independent frame SLA.

| Rendered request | 0.3.0 median / p95 ms | Combined median / p95 ms |
|---|---:|---:|
| batch 100 | 287.763 / 299.043 | 12.275 / 17.120 |
| batch 500 | 1382.258 / 1406.497 | 36.389 / 40.598 |
| native sleep 30 ms | 45.445 / 49.166 | 46.302 / 49.255 |

Frame sampling spans setup, warmups, measurements and cleanup, requests an
editor UI redraw every process frame, and independently brackets the bridge
pump's process-frame callbacks. It is not restricted to a best-case batch.
Intervals include frame scheduling/render overhead. The continuous bracket
includes callback work around the pump, not total renderer time or GPU time.

| Variant / round | Drawn frames | Interval p95 / max ms | Continuous bracket p95 / max ms |
|---|---:|---:|---:|
| 0.3.0 / 0 | 293 | 1383.663 / 1440.165 | 1372.557 / 1428.379 |
| 0.3.0 / 1 | 289 | 1383.888 / 1411.044 | 1372.133 / 1398.763 |
| 0.3.0 / 2 | 289 | 1383.432 / 1477.517 | 1371.975 / 1466.010 |
| combined / 0 | 333 | 47.925 / 49.553 | 37.543 / 38.503 |
| combined / 1 | 332 | 47.954 / 50.359 | 37.461 / 38.701 |
| combined / 2 | 333 | 48.021 / 50.381 | 37.550 / 38.713 |

The long-batch stall reduction repeats in all rounds. The optimized series
still deliberately includes a non-preemptible 30 ms native call; its roughly
38 ms continuous maxima and 50 ms frame intervals are not hidden by a claim
that every frame now fits a budget. Batches remain ordered single dispatches,
not split/cooperative jobs. A different batch scheduler would require another
contract/design decision.

### Final control attribution, memory and verification

```bash
python3 -B tools/test_generic_bridge.py --benchmark --benchmark-profile 1 --benchmark-topology all --benchmark-case packed_128 --benchmark-case packed_10000 --benchmark-case native_block_30ms --benchmark-output /tmp/godot-cm-perf-final-write-controls.json
```

This targeted series keeps every case inside the reader's cap: three rounds,
ten warmups and sixty measured samples for each of three cases/topologies,
1,620 measured samples and 270 warmups, all successful. Native phase summaries
exclude warmups. It supplies measured attribution for the conservative-call
controls instead of relying on the earlier cap-limited warmup probes.

| Topology | Case | Catalog / native invocation / pump median ms |
|---|---|---:|
| minimal | packed 128 | 2.486 / 0.004 / 4.356 |
| scripts | packed 128 | 3.610 / 0.004 / 5.628 |
| scene1000 | packed 128 | 2.598 / 0.004 / 4.131 |
| minimal | packed 10000 | 2.544 / 0.043 / 4.704 |
| scripts | packed 10000 | 3.704 / 0.044 / 5.960 |
| scene1000 | packed 10000 | 2.697 / 0.059 / 4.562 |
| minimal | native sleep 30 | 3.194 / 30.074 / 35.063 |
| scripts | native sleep 30 | 4.473 / 30.075 / 36.365 |
| scene1000 | native sleep 30 | 4.196 / 30.075 / 35.535 |

Packed calls show one global check and median 3/23/3 script reflections for
minimal/scripts/scene1000 respectively. Their native method bodies take
microseconds while catalog work takes milliseconds. The sleep body still
occupies about 30 ms and cannot be interrupted by a budget check made after
it returns. These are inclusive phase medians, not numbers to add to HTTP time.
The regression mechanism is the added conservative freshness check; removing
it would break the verified in-batch reload behavior. It is a documented
trade-off of the stable-native fast path, not unexplained degradation or a
claim that arbitrary script calls became faster.

The final integrated lifecycle run uses the same combined binaries as these
measurements. It passes the 39-type suite, current-tier/handle/value checks,
script/filesystem and native-extension freshness, request-pinned snapshots,
reversible writes, Undo/Redo, failed rollback/write locking, cancellation,
checkpoints, passive serialization and output/retention quotas. In 24 script
invalidation cycles, six post-warmup RSS samples are all 1,252,140 KiB; the
process's final gauges show two live catalogs, peak four, five script-cache
entries. This demonstrates stabilization in the tested cycle, not a promise of
constant OS RSS for every project. Reference-quota and page-failure tests verify
release/rollback without invalidating earlier references.

Both native binaries were rebuilt after all C++ changes; Linux integration and
nineteen harness unit tests pass. The existing declarations typecheck with
TypeScript 5.8.3; no public declaration format or generator change requires a
regeneration. Windows was cross-built only and was not run. The measured
candidate retained its 0.3.0 version label; there is no permission-policy,
transport or batch scheduling change. Cold startup and
unknown-effect calls have the measured costs above; single-operation latency,
small pages, idle CPU and some worker p95 controls have no general improvement
claim. The demonstrated gains are stable multi-read batches, larger page
encoding and modest equivalent-data worker finalization, under the recorded
conditions.

### Release 0.3.1 verification

The performance matrices above measured the optimized working tree before its
version label was changed. For release 0.3.1, only the plugin/server version
labels changed after those measurements; both binaries were rebuilt. Historical
candidate hashes and timings above remain unchanged rather than being relabeled
as a new benchmark capture.

Release binary SHA-256:
- Linux: `abbdb8d1a581ed1a032b0b810639027b04bcfd3ff19222a379c1612835154ca3`.
- Windows: `8fd89e75117f044dcd9e9e6265d430bfc19c6114c8ea807b250975e7769e5f44`.

The rebuilt Linux binary passes the full isolated suite, native reload and
24-cycle lifetime checks, with final catalog gauges 2 live / 4 peak / 5 script
entries and six stable post-warmup RSS samples of 1,252,700 KiB. All nineteen
harness tests, TypeScript 5.8.3, strict OpenSpec validation and whitespace checks
pass. A separate startup smoke capture at `/tmp/godot-cm-release-031-smoke.json`
checks that the MCP server reports the same 0.3.1 version as `plugin.cfg`.
That one-process capture is not a replacement performance matrix. Linux ELF
and Windows PE32+ formats are verified; Windows execution remains unverified.
