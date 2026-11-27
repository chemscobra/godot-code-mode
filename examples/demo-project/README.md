# Godot CM Demo Project

Deterministic sandbox for manually testing the `godot-cm` addon together with
third-party, non-builtin classes.

## Contents

| Path | Purpose |
|---|---|
| `addons/godot_cm` | **Symlink** to `../../addons/godot_cm` — always runs your dev build, no copying |
| `addons/terrain_3d` | Terrain3D v1.0.2 (GDExtension, Linux binaries only, trimmed) |
| `addons/phantom_camera` | PhantomCamera v0.12 (GDScript plugin) |
| `demo.tscn` | Node3D scene: a script-attached `MeshInstance3D` and a `RigidBody3D` |
| `spinner.gd` | `DemoSpinner` custom class for typed/untyped echo and collection calls |

`.godot/` is gitignored — Godot regenerates it on first open, so the project
does not accumulate noise in git.

## Use

1. Open this folder with the Godot editor (`godot --path examples/demo-project`
   or the project manager). First import takes a few seconds.
2. The three plugins are already enabled; the Godot CM dock appears in the
   bottom panel with the MCP URL (`http://127.0.0.1:8765/mcp`) and session
   token.
3. Non-builtin classes to exercise: native `Terrain3D`, `Terrain3DData`,
   `Terrain3DTextureAsset` (GDExtension), script classes `PhantomCamera3D`,
   `PhantomCameraHost` (try `api.describe("PhantomCamera3D")` or
   `godot.create("PhantomCamera3D")`), plus the project script `DemoSpinner`.

## Notes

- The suite fixture (`tools/test_generic_bridge.py`) stays fully isolated in
  a temp directory; this project is for *manual* exploration only — Godot may
  rewrite `project.godot` or `.godot/` state, which is ignored by git.
- To pin different addon versions, replace the vendored folders and update
  the version numbers above.
