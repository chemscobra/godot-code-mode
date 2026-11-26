# Tasks

## 1. Batch dispatch

- [x] 1.1 Add `kind: "batch"` op to the bridge: payload is an element list; drain executes elements inline in one frame via `execute_op` and returns ordered `{ok, result, error}` results
- [x] 1.2 Enforce the op budget across batch elements (batch element count + each element's ops count toward the per-execution budget)
- [x] 1.3 Add the `godot.batch(ops)` JS binding in the worker and regenerate `api-surface.d.ts`
- [x] 1.4 Verify: build_arena-style script using batch completes in ~1 dispatch; per-element failure marks only that element

## 2. Inheritance-aware authorization

- [x] 2.1 Change `member_allowed` to walk the index `inherits` chain on miss (cycle-safe, bounded)
- [x] 2.2 Verify: `set albedo_color` on `StandardMaterial3D` works with only `BaseMaterial3D` allowlisted; `OS` still invisible
- [x] 2.3 Simplify `allowlist.json` parent-class entries that are now redundant; regenerate `.d.ts`; document the semantic in README

## 3. Journal coverage + unlock

- [x] 3.1 Extend journal inverses to remaining write ops (`rename`, `reparent`, others the bridge exposes)
- [x] 3.2 Reject write ops with no safe inverse at dispatch time with typed `NO_UNDO` error
- [x] 3.3 Add `godot.unlockWrites()` write-mode op: clears `write_locked`, appears in diagnostics
- [x] 3.4 Verify: unjournaled op → `NO_UNDO` with no mutation; forced partial rollback → lock → `unlockWrites` → writes work again

## 4. Regression + docs

- [x] 4.1 Re-run all existing demos (collect2d, collect3d, neon_arena) — unchanged behavior
- [x] 4.2 Update `docs/benchmarks.md` with batch latency measurement
- [x] 4.3 `openspec validate`

## Workflow follow-up

- Archive this change after review
