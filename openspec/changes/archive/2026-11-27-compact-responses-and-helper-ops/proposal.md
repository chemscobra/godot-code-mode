# Proposal

## Why

Dogfooding measured the dominant cost in response noise and retry-inducing
gaps: `execute` emitted a per-op `changes`/`effects` entry for every write
(~17k chars in one scene build), `set script` had no authorized path,
building a node needed 4-6 calls, and several engine types had no clear
wire representation, forcing trial-and-error retries.

## What Changes

- Compact responses by default: empty fields omitted; writes summarized as
  `changes: {counts, total, status}` with `status: committed|rolled_back`;
  `verbose: true` tool arg restores the ordered per-op log. External
  `effects` always remain visible.
- `godot.attachScript(node, 'res://x.gd')` — undoable script assignment via a
  trusted registered operation (`editor.attach_script`, filesystem tier);
  rejects @tool scripts, incompatible bases, and non-canonical paths.
- `godot.build({type,name,props,children},{parent})` — one-call tree
  construction inside the transaction with automatic owners.
- `godot.scene.new/reload/tree/run/stop` — create/open scenes and wait for
  the real root, reload-and-reopen, passive tree inspection with script
  paths, playback control.
- Type consistency: `set_x(v)` routes to the journaled `set` path when the
  property is authorized; tagged values for Basis, Transform3D, Quaternion,
  Vector4/i, Vector2i/3i, Rect2, AABB, Plane, NodePath, StringName;
  `api.example(type)` returns the accepted wire form.

## Capabilities

### New Capabilities
- `scene-helpers`: build/scene lifecycle/tree inspection helpers layered on
  the bridge primitives.

### Modified Capabilities
- `godot-editor-bridge`: `scene_tree` op; expanded tagged-type conversion.
- `code-execution`: compact result format + `verbose` arg.
- `editor-transactions`: `editor.attach_script` trusted operation.
- `api-discovery`: `api.example`; helper surface documentation.

## Impact

- `src/bridge.cpp/.hpp`, `src/js_worker.cpp/.hpp`, `src/api_index.cpp/.hpp`,
  `src/mcp_service.cpp`: all changes listed above.
- `addons/godot_cm/editor_operations.gd`: `editor.attach_script`,
  `editor.reload_scene` trusted operations.
- `tools/test_dynamic_bridge.py/.gd`: regression checks for compaction,
  attachScript, build/tree, scene lifecycle, and tagged values.
- `api-surface.d.ts` regenerated; Linux/Windows binaries rebuilt.
