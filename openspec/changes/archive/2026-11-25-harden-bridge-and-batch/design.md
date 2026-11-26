# Design

## Context

Dogfooding measured op latency as frame-bound: the bridge drains at most the
queued ops once per main-thread frame, so each `await` costs ~7 ms while the
scene renders and ~100 ms in idle low-processor mode (Neon Arena: 477 ops in
3.4 s; an idle-mode build measured ~105 ms/op). The same dogfooding found two
correctness gaps: `member_allowed` only checks the exact class's metadata so
inherited members (`albedo_color` on `StandardMaterial3D`, declared in
`BaseMaterial3D`) are rejected despite `BaseMaterial3D` being allowlisted, and
only a subset of write ops record journal inverses, so a mid-flight failure on
the others produces a partial rollback that flips `write_locked` — currently
with no way out but an editor restart.

## Goals / Non-Goals

**Goals:**
- `godot.batch(ops)`: one `await`, one main-thread dispatch, N ops executed
  sequentially, N ordered results.
- Inheritance-aware member authorization using the `inherits` chain already
  stored in the API index.
- Every write op journaled or explicitly rejected at dispatch; a bounded,
  deliberate unlock path for `write_locked`.

**Non-Goals:**
- Parallel or speculative batch execution — batch elements run sequentially on
  the main thread exactly like awaited ops do today.
- Atomic batch mode — per-element errors do not abort siblings (matches
  per-op behavior); callers wanting atomicity use write mode + tx rollback.
- Changing the allowlist format or the op wire format beyond a `batch` element.

## Decisions

### D1: `godot.batch` is a sugar binding, not a new transport

`godot.batch(ops)` enqueues a single `kind: "batch"` op whose payload is the
element list. The main-thread drain executes the elements inline in the same
frame (reusing `execute_op` per element) and resolves the one promise with an
array of per-element `{ok, result, error}` results.

- Rationale: zero new worker↔main protocol machinery; the existing inflight/
  promise machinery handles it unchanged.
- Alternative considered: flattening N ops into N queue entries resolved as one
  promise — rejected because the drain's per-frame budget would still split
  them across frames in idle mode, defeating the point.
- Element count is capped (same op budget constant) so a batch cannot bypass
  the per-execution operation budget.

### D2: `member_allowed` walks the `inherits` chain

Authorization consults the class's own metadata first; on miss, walk the
`inherits` chain recorded in the API index until a base class grants the
member (or `"*"`). Cycle-safe, bounded by chain depth.

- Rationale: matches engine semantics — members are inherited; allowlisting a
  base class like `GeometryInstance3D` then correctly covers
  `MeshInstance3D`/`material_override` without duplicating entries.
- Trade-off: `"*"` on a broad base (`Object`) would authorize everything below
  it — acceptable, since writing `"*": Object` is already an explicit
  "allow the world" statement.
- The generated `.d.ts` still lists declared members only; docs gain a note
  that inherited members are usable when a base class allows them.

### D3: Journal coverage — journal or reject

Extend the journal to every write op the bridge dispatches (e.g. `rename`,
`reparent`, `call` with side effects where feasible). Ops with no safe inverse
(e.g. arbitrary `call` args producing non-idempotent engine calls) are rejected
at dispatch with a typed `NO_UNDO` error rather than applied unjournaled.

- Rationale: turns the silent "partial rollback" hazard into an explicit,
  typed failure — the spec-level guarantee "no partial mutations" becomes real.
- `call` remains allowlisted per-method; methods on the allowlist are vetted
  as side-effect-free-or-invertible, so most `call`s stay usable.

### D4: `godot.unlockWrites()` — deliberate, logged unlock

A new write-mode-only bridge op clears `write_locked`. It is itself journaled-
exempt, audited (appears in `changes`/`diagnostics`), and requires
`mode: write`. No automatic unlock — an operator (or the agent with write
mode) must invoke it.

- Rationale: restart-to-recover is a bad UX for a recoverable state; the lock's
  purpose is forcing a review *decision*, not forcing a restart.
- Alternative considered: auto-unlock on next successful read-only call —
  rejected; it would silently skip the required review.

## Risks / Trade-offs

- Batch hides per-element latency → per-element results include timing/op id so
  diagnostics stay honest.
- Inheritance walk could over-authorize on sloppy allowlists → documented as
  semantic: allowlisting a base class means "this member everywhere below it."
- `NO_UNDO` rejection may break previously-working scripts that used
  unjournaled ops → acceptable: those scripts had no rollback guarantee anyway;
  rejection is listed in the changelog.
