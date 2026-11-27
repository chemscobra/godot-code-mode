# Design

## D1: Compact reporting lives in the worker epilogue

`TxState` gains `change_counts` (`"Class.op" -> n`) and `verbose`. Writes are
counted at record time in `Bridge::drain` and inside batch elements, so no
per-op dictionaries accumulate. The worker epilogue emits `{counts, total,
status}`; `status` distinguishes committed vs rolled_back so a rollback is
never presented as persisted changes. `base_result` keeps only `ok`/`result`;
`diagnostics`/`changes`/`effects`/`truncated` appear only when meaningful.
External `effects` are emitted even when JS fails later — non-rollbackable
actions must stay visible.

## D2: Script attachment is a trusted plugin operation, not a property

`godot.attachScript` is a facade over `editor.attach_script`, registered by
the addon's own editor code with `capture`/`restore` journal methods and the
filesystem tier. Validation: canonical `res://*.gd`, file exists, loads as
`Script`, not `@tool` (tool scripts run editor code and are not honestly
rollbackable), and `get_instance_base_type()` is compatible with the node
class. `Script`/meta-methods stay denied; discovery of a script class grants
nothing.

## D3: Helpers are a JS prelude over existing primitives

`godot.build` and `godot.scene.*` are defined in an evaluated prelude inside
each execute context, so every constituent op still passes through the
normal bridge path: per-op budgets, transaction journal, undo versioning,
and external-action gating all apply unchanged. Deferred editor actions
(`open_scene_from_path.call_deferred`) are awaited by polling
`editor.state` until the expected root appears, with a typed timeout —
`"opening": true` is never reported as completion.

## D4: Owner policy in build

Attached trees get the edited scene root as owner when the parent actually
lives in it (`is_ancestor_of` check); detached trees own their descendants
relative to the built root. Owners of instanced scenes are never guessed or
overwritten.

## D5: Const truth comes from the API dump, not name prefixes

The prefix heuristic misclassified 1,558 const methods as writes. Method
signatures in the index now carry `const` (from `is_const` or
`flags & METHOD_FLAG_CONST`). Read-mode `call` ops are forwarded to the
bridge, which downgrades indexed-const calls to reads and rejects the rest —
the JS worker lacks the index, so the decision belongs on the main thread.

## D6: Journal coverage extended by category, not per method

- Transform mutators (`translate`, `rotate*`, `scale*`, `global_*`,
  `orthonormalize`): capture `global_transform`, inverse is `set`.
- `show`/`hide`: capture `visible`, inverse is `set`.
- `add_to_group`/`remove_from_group`: inverse pair, args reused.
- `move_child`: capture prior `get_index()`, inverse `callv`.
- `add_sibling`: inverse is `remove_child` on the captured parent.
- `grab_focus`/`release_focus`: inverse pair.
- `set_x(v)`: routed through the journaled `set` path when the property is
  authorized — same mutation, same undo, either spelling.

Mutators without a safe inverse still fail `NO_UNDO` at dispatch.

## D7: Permission introspection is passive metadata

The index retains `denied_classes`, `denied_members`, `denied_global`, and
`restrict` captured at load. `godot.permissions` never grants and never
executes: no-arg returns the tier/policy summary; `{class, member}` returns
`{allowed, reason, tier, policy, unlock}` with reasons `class_denied`,
`meta_method_denied`, `member_denied`, `restricted_class`, `tier_locked`,
`member_not_indexed`.

## Limitations kept explicit

- `scene.run` reports `console: "unavailable"`: `EditorDebuggerPlugin` only
  receives messages on registered capture channels; the standard console is
  not exposed by the public debugger API in 4.7.
- `NOT_STATIC` from the field report has no reproducible path in this
  revision (no static-call surface exists); the ResourceLoader guidance is
  carried by `load_resource` errors and `godot.permissions`.
