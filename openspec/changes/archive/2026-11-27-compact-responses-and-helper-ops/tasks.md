# Tasks

## 1. Compact responses

- [x] 1.1 Add `verbose` tool arg + `ExecRequest`/`TxState` flag; `base_result` keeps only `ok`/`result`
- [x] 1.2 Summarize writes as `{counts: {"Class.op": n}, total, status}` with `committed|rolled_back`; `verbose:true` restores per-op log
- [x] 1.3 Keep external `effects` visible even after JS failure; omit empty `diagnostics`/`changes`/`effects`/`truncated`
- [x] 1.4 Verify: compact response is empty-field-free; 24 sets → one count entry; rollback reports `rolled_back`

## 2. attachScript

- [x] 2.1 Register `editor.attach_script` (trusted, filesystem tier, capture/restore journal) in `editor_operations.gd`
- [x] 2.2 Validate canonical `res://*.gd`, exists, `Script`, non-@tool, base-type compatible
- [x] 2.3 Add `godot.attachScript(node, path)` facade + `.d.ts`
- [x] 2.4 Verify: attach works and rolls back; @tool/incompatible/missing paths rejected

## 3. build + scene helpers

- [x] 3.1 `godot.build(spec,{parent})` prelude over create/set/call; owners auto (scene root when attached, built root when detached)
- [x] 3.2 `godot.scene.new` waits on `editor.state` for the real root with typed timeout; `scene.reload(path?)` reload+reopen via `editor.reload_scene`
- [x] 3.3 `scene_tree` bridge op: passive name/class/script-path walk with depth/maxNodes bounds and explicit truncation
- [x] 3.4 `scene.run`/`scene.stop` via play/stop plugin actions; `console:"unavailable"` reported honestly
- [x] 3.5 Verify: build children count correct; tree reflects names/classes; scene.new opens `gen_scene.tscn` headless

## 4. Type + error consistency

- [x] 4.1 `set_x(v)` routes to journaled `set` when property authorized (`set_material_override` ≡ `set material_override`)
- [x] 4.2 Tagged values: Basis, Transform3D, Quaternion, Vector4/i, Vector2i/3i, Rect2, AABB, Plane, NodePath, StringName
- [x] 4.3 `api.example(type)` returns the accepted wire literal (available in `search` too)

## 5. Regression + docs

- [x] 5.1 Regression checks in `tools/test_dynamic_bridge.py`; suite green
- [x] 5.2 `cmake --build build -j 4` and `cmake --build build-windows -j 4` pass; `.d.ts` regenerated
- [x] 5.3 README + `execute` tool description updated
