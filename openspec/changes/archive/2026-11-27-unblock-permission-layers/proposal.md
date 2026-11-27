# Proposal

## Why

After the helper-op work, real usage still hit "something is always
blocked". Investigation showed the allowlist was not the problem — four
independent layers were silently denying calls:

- `restrict` on `EditorInterface` blocked 41/71 methods, including
  `get_open_scenes` and `get_resource_filesystem`.
- The name-prefix heuristic classified 1,558 `const` methods as writes,
  denying them in read mode and failing them `NO_UNDO` in write mode.
- Only 7 journaled inverses existed, so common mutators (`show`, `hide`,
  `move_child`, `add_to_group`, `translate`, `grab_focus`…) failed
  `NO_UNDO`.
- Packed array returns (e.g. `get_open_scenes`) reached JS as strings, not
  arrays — a working call looked broken.

Denials were also opaque: there was no way to ask *why* a member was
blocked without provoking the error.

## What Changes

- Read/write classification uses the `const` flag (`is_const` from the
  dump, `flags & METHOD_FLAG_CONST` at runtime) instead of name prefixes;
  const calls execute in read mode and are excluded from change counts.
- Journaled inverse coverage extended to transform mutators (restored via
  `global_transform`), `show`/`hide`, `add_to_group`/`remove_from_group`,
  `move_child`, `add_sibling`, `grab_focus`/`release_focus`.
- `EditorInterface` `restrict` list widened with read-only members:
  `get_open_scenes`, `get_selected_paths`, `get_resource_filesystem`,
  `get_editor_paths`, `get_script_editor`. Returned objects keep their own
  class/tier rules.
- `godot.permissions(query?)` — passive introspection: tier/policy/denial
  summary or per-member `{allowed, reason, tier, policy, unlock}`.
- Packed arrays flatten to JS arrays in both wire directions.
- `set_x(v)` routes to the journaled `set` path when the property is
  authorized (kept here with the inverse work it depends on).

Intentionally still blocked: escape-hatch classes and descendants
(`OS`, `Engine`, `Script`, `ProjectSettings`, `SceneTree`…), tier-gated
classes until unlock, meta-methods, inherited `deny_members`, and mutators
with no reasonable inverse (`queue_free`, `play`… → `NO_UNDO`).

## Capabilities

### Modified Capabilities
- `godot-editor-bridge`: const downgrade, `permissions` op, packed-array
  flattening, widened EditorInterface surface.
- `editor-transactions`: extended journaled inverse set.
- `api-discovery`: `const` flag stored in method signatures.

## Impact

- `src/bridge.cpp/.hpp`, `src/js_worker.cpp`, `src/api_index.cpp/.hpp`:
  classification, inverses, `permissions`, packed arrays.
- `addons/godot_cm/allowlist.json`: widened EditorInterface `restrict`.
- `tools/test_dynamic_bridge.py`: regression checks for const calls,
  new inverses, permission reasons, packed arrays.
