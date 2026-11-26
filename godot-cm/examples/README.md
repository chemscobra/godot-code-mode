# Example games — dogfooding the addon

Two minimal games with real core loops. Each ships as:

- a root `.tscn` with the loop script already attached, and
- a `build_*.js` script meant to be sent through MCP `execute`
  (`mode:"write"`) while the scene is open in the editor.

The MCP call constructs the whole level — nodes, meshes, materials, camera —
as ONE undoable editor action, then saves via `godot.saveScene()`.

## Coin Rush (2D) — `collect2d/`

Move a square with arrow keys, collect coins, score goes up, coin respawns.

```
1. Open  examples/collect2d/collect2d.tscn  in the editor
2. execute { mode:"write", code: <contents of build_2d.js> }
3. F6 / play scene — WASD/arrows to move
```

Builds: `Player` (green ColorRect), `Hud` (Label), `Camera2D`,
`Coin_0..7` (yellow ColorRects in a ring).

## Orb Run (3D) — `collect3d/`

Move a sphere on a plane, touch floating orbs, they respawn elsewhere.

```
1. Open  examples/collect3d/collect3d.tscn
2. execute { mode:"write", code: <contents of build_3d.js> }
3. Play scene — arrows to move on the XZ plane
```

Builds: `Ground` (PlaneMesh), `Player` + `Orb_0..7` (SphereMesh +
StandardMaterial3D), `Sun` (DirectionalLight3D), `Camera3D`.

## Neon Arena (3D, elaborate) — `neon_arena/`

Collect 12 energy cells while dodging 8 patrolling sentries; 3 hits = game
over. Win/lose loop with HUD, lives and patrol AI.

```
1. Open  examples/neon_arena/neon_arena.tscn
2. execute { mode:"write", code: <contents of build_arena.js> }
3. Play scene — arrows to move
```

Builds (477 ops): floor + glowing walls, 12 pillars, player, 12 cells,
8 sentries, CanvasLayer HUD, sun, camera. Benchmarked in
`docs/benchmarks.md` Scenario 2.

## Why this demonstrates the pattern

Each build is ~40 awaited ops in a single `execute` call — one MCP
round-trip produces a runnable level, journaled as one Undo step with
rollback on failure. The `.tscn` equivalent would be ~200 lines of
scene-format text the agent would have to author (uids included) plus a
reload step. See `docs/benchmarks.md`.

Requires the allowlist additions in `addon/godot_cm/allowlist.json`
(`ColorRect`, `Label`, `Camera*`, `DirectionalLight3D`, `Control`) — the
.d.ts regenerates on plugin load.
