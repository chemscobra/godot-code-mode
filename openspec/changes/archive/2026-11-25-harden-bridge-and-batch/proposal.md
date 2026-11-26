# Proposal

## Why

Dogfooding the addon (Neon Arena: 477 ops) measured ~7 ms/op with the editor
rendering and ~100 ms/op while idle, because every awaited op pays one main-thread
frame. Op-dense scripts are the addon's worst case. Dogfooding also exposed two
spec-level gaps: `"*"` allowlist entries only cover members declared on the
exact class (inherited members like `albedo_color` are silently rejected), and
write ops without a journaled inverse can produce partial rollbacks that lock
writes with no recovery path.

## What Changes

- Add `godot.batch(ops)` — apply a list of bridge operations in one main-thread
  dispatch, returning all results in one awaited promise.
- Make per-operation authorization inheritance-aware: a member allowlisted on a
  base class is permitted on subclasses; `"*"` covers the full inheritance
  chain.
- Extend the transaction journal so every supported write op records an inverse;
  add an explicit way to clear the write lock after a partial rollback.

## Capabilities

### New Capabilities
<!-- none — batch is a bridge behavior, not a new capability -->

### Modified Capabilities
- `godot-editor-bridge`: batched dispatch op; inheritance-aware member authorization.
- `editor-transactions`: all write ops journaled; write-lock recovery path.

## Impact

- `src/bridge.cpp` / `src/bridge.hpp`: batch dispatch, `member_allowed` walks
  `inherits` chain, journal inverses for remaining write ops, unlock op.
- `src/js_worker.cpp`: `godot.batch` binding + batching of awaited results.
- `addon/godot_cm/allowlist.json`: parent-class entries may become unnecessary
  (kept harmless); `.d.ts` regenerated.
- Latency: op-dense scripts move from frame-bound (~7–100 ms/op) to
  one dispatch per batch.
