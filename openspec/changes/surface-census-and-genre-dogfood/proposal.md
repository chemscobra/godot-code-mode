# Proposal

## Why

Case-by-case tests cannot prove "no silent blocks remain": they only cover
the paths someone thought to try. The single-genre dogfood (3D scene build)
never exercises TileMap, UI/theme, signals, resources, or network-tier
surfaces — exactly where untested `NO_UNDO` and unexplained denials hide.

## What Changes

- **Surface census**: a single `execute` sweeps every indexed class/member
  through `godot.permissions`, classifying each into declared buckets
  (`allowed-read`, `allowed-write`, `tier-locked`, `denied`, `no-undo`).
  Invariant: the unexplained bucket MUST be empty.
- **Census snapshot**: the census emits JSON, diffed against a versioned
  expectation file, so allowlist or Godot upgrades surface unintended
  bucket changes as regressions.
- **Real-call sampling**: N const calls in read mode and every journaled
  inverse in write mode (with forced rollback) prove classification matches
  execution.
- **Multi-genre dogfood**: per-genre scenarios (2D platformer/TileMap,
  UI/Control, signals/connect; fixtures + `godot.build` mix) reporting a
  "friction invoice": calls needed, blocks hit with `reason`, response
  bytes. `NO_UNDO` findings are reported as gaps, not auto-fixed.
- **MCP contract audit**: `tools/list` schema, tool annotations
  (`readOnlyHint`/`destructiveHint`), `structuredContent` + text parity,
  JSON-RPC error codes, cross-session handle isolation.
- **Real MCP client**: a script using the official MCP SDK exercises
  transport concerns the in-process harness invents identically to the
  server (version negotiation, sessions, list paging).
- Findings (`NO_UNDO` gaps like `set_cell`/`connect`, missing annotations)
  feed follow-up changes; this change measures and documents, it does not
  add new inverses or API surface.

## Capabilities

### New Capabilities
- `surface-verification`: census invariant, snapshot diffing, real-call
  sampling, per-genre dogfood scenarios, and the MCP contract audit as a
  durable, repeatable verification capability.

### Modified Capabilities
- (none — this change adds verification, no runtime requirement changes)

## Impact

- `tools/test_dynamic_bridge.py` or a sibling sweep script; new
  `tools/expected_surface.json` snapshot; `tools/fixtures/<genre>/`.
- Official MCP SDK added as a test-only dependency (Python or TS).
- No changes to shipped addon code expected; findings may spawn follow-up
  changes.
