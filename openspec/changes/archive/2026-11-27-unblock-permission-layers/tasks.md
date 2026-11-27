# Tasks

## 1. Const-correct classification

- [x] 1.1 Store `const` flag in index method sigs (dump `is_const` or runtime `flags & METHOD_FLAG_CONST`)
- [x] 1.2 Forward read-mode `call` ops to the bridge; downgrade indexed-const calls to reads
- [x] 1.3 Exclude const-downgraded calls from change counts
- [x] 1.4 Verify: `can_process` runs in read mode; non-const still `PERMISSION`

## 2. Extended journaled inverses

- [x] 2.1 Transform mutators (`translate`/`rotate`/`scale*`/`global_*`) inverted via captured `global_transform`
- [x] 2.2 `show`/`hide` via `visible`; `add_to_group`/`remove_from_group`; `move_child` via prior index; `add_sibling` via captured parent; `grab_focus`/`release_focus`
- [x] 2.3 `set_x(v)` routes to journaled `set` when property authorized
- [x] 2.4 Verify: rollback restores transform/groups/sibling position

## 3. EditorInterface surface + packed arrays

- [x] 3.1 Widen `restrict` with `get_open_scenes`, `get_selected_paths`, `get_resource_filesystem`, `get_editor_paths`, `get_script_editor`
- [x] 3.2 Flatten all packed array types to JS arrays in `to_wire` and `variant_to_js`
- [x] 3.3 Verify: `get_open_scenes` returns a real array; `get_resource_filesystem` object stays tier-gated

## 4. Permission introspection

- [x] 4.1 `permissions_summary` + `permission_info` in the index; `permissions` bridge op
- [x] 4.2 `godot.permissions(query?)` facade + `.d.ts`; passive — never grants
- [x] 4.3 Verify: reasons `restricted_class`, `member_denied`, `meta_method_denied`, `tier_locked`, `member_not_indexed`

## 5. Regression

- [x] 5.1 New checks in `tools/test_dynamic_bridge.py`; suite green
- [x] 5.2 `cmake --build build -j 4` and `cmake --build build-windows -j 4` pass
