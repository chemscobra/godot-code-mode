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
