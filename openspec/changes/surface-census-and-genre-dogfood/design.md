# Design

## Context

The harness (`tools/test_dynamic_bridge.py` + `.gd`) already launches a
headless editor on a temp project and speaks real MCP over loopback HTTP
(`initialize` + `tools/call` on `/mcp`). The API index and
`godot.permissions(query?)` exist, so a full-surface sweep needs no new
bridge code — only a driver that runs inside one `execute`.

## Goals / Non-Goals

**Goals:**
- Prove every indexed member lands in a declared permission bucket, with
  zero unexplained outcomes.
- Make the census diffable so future allowlist/Godot changes regress loudly.
- Produce per-genre "friction invoices" that prioritize follow-up fixes
  (e.g. `NO_UNDO` on `set_cell`/`connect`).
- Audit the MCP contract against spec best practices.

**Non-Goals:**
- No new inverses, API surface, or allowlist changes — findings feed
  follow-up changes.
- No gameplay verification: headless validates hierarchy/resources, not
  physics/input. `scene.run` console stays `unavailable`; runtime errors
  out of scope.
- No Windows runtime verification (no runner); build-only as today.

## Decisions

- **Census inside one `execute`**: a JS loop iterates the pinned catalog
  snapshot and calls `godot.permissions` per member, returning only bucket
  counts + violations. Alternative (one `tools/call` per member) rejected:
  thousands of round-trips and context bloat for zero added coverage.
- **Versioned snapshot `tools/expected_surface.json`**: census JSON is
  diffed against it; differences fail the test. Chosen over a live-only
  sweep because it turns every allowlist edit or engine upgrade into an
  explicit, reviewable surface delta.
- **Sampling for execution truth**: const calls picked deterministically
  (every Nth indexed-const method on fixture nodes) run in read mode;
  each journaled inverse runs in write mode followed by a forced failure.
  Full execution census rejected — many mutators have side effects or
  preconditions unsuitable for blind invocation.
- **Genre fixtures, mixed**: minimal `.tscn` fixtures under
  `tools/fixtures/<genre>/` for realistic starting scenes (platformer
  TileMap, UI Control tree); mutations/assertions done via `godot.build`,
  `set`, `call`, `attachScript`. Pure `build`-generated scenes rejected —
  too synthetic to catch real friction.
- **Genres, round 1**: 2D platformer (TileMapLayer/physics 2D), UI/Control
  (theme/resources), signals (`connect`/`emit`). Terrain deferred — heavy
  and already covered by its adapter; multiplayer exercised only as
  tier-denial visibility, not connectivity.
- **Real client**: Python `mcp` SDK script (`tools/test_mcp_client.py`)
  doing its own handshake/pagination against the temp server; keeps the
  repo Python-only. Inspector (npx) left as optional manual check.
- **MCP contract audit**: assert `tools/list` returns valid input schemas,
  document presence/absence of annotations (`readOnlyHint`,
  `destructiveHint`) as findings; verify `structuredContent` parity with
  text `content`, stable domain error codes (`PERMISSION`, `NO_UNDO`,
  `INVALID_HANDLE`) mapped to correct JSON-RPC codes, and cross-session
  handle rejection.
- **NO_UNDO inventory output**: census `no-undo` bucket is dumped to the
  report — that list is the backlog input for a later
  `commitIrreversible`/inverses change.

## Risks / Trade-offs

- Census inside `execute` runs as JS — a catalog-snapshot bug could skew
  results; mitigated by real-call sampling and by the snapshot diff.
- Snapshot churn: every legitimate allowlist edit forces a snapshot
  refresh; acceptable — that is the review point.
- `mcp` SDK is a new test-only dependency; pin a released version.
- Headless `scene.run` cannot capture console errors; dogfood asserts
  hierarchy/resource state only, documented as a coverage limit.
