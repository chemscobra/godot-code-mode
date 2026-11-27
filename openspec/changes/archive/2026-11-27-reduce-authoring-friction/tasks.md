# Tasks

## 1. Policy hot-reload

- [x] 1.1 In `src/api_index.hpp`/`api_index.cpp`, store `policy_path` and `policy_mtime` in `ApiIndex::load`; verify `load` still returns false on a bad file
- [x] 1.2 At the top of `ApiIndex::refresh`, re-parse `allowlist.json` when `FileAccess::get_modified_time` differs: on success replace `policy_document`, reset `tier_policy` from the file, additively apply `unlocked_tiers`/`auto_grant_tiers`, set `dirty`, and print a reload log; on parse failure keep the old policy
- [x] 1.3 Verify against the running editor: edit `allowlist.json` (e.g. move `filesystem` to `allow`) and confirm `godot.permissions()` / `requestTier` behavior changes without a plugin restart, and that a mid-save malformed file keeps the prior policy

## 2. Trusted `editor.create_script` operation

- [x] 2.1 In `addons/godot_cm/editor_operations.gd`, register `editor.create_script` (`tier: filesystem`, `capture`/`restore`) implementing: canonical `.gd` path validation, `@tool` source refusal, `FileAccess` write, `EditorFileSystem.update_file`, refuse overwrite without `overwrite:true`
- [x] 2.2 Implement `capture`/`restore` so a created file is deleted on rollback and a pre-existing file is restored byte-for-byte; verify via the generic bridge test fixture
- [x] 2.3 Add `godot.createScript(path, source)` facade + `input_schema`/`arguments`/`description` metadata and the `api-surface.d.ts` declaration; verify `String(godot.createScript)` and `api.helpers()` (task 4) expose it
- [x] 2.4 Verify end-to-end in an isolated fixture: create `.gd` via MCP, attach via `attachScript`, confirm the file appears in `EditorFileSystem` and that a failing transaction removes the created file

## 3. Handshake and tool-contract docs

- [x] 3.1 In `handle_initialize` (`src/mcp_service.cpp`), include the session token in the result body as `sessionId`; verify `initialize` still returns the `MCP-Session-Id` header and a client using only the body can `tools/list`
- [x] 3.2 Append the external-action isolation sentence to the `execute` tool description; verify `tools/list` output mentions that `pluginAction`s need their own `execute` call with no other writes

## 4. `api.helpers()` discovery

- [x] 4.1 Add a static helper-metadata table in `src/js_worker.cpp` exposed as `api.helpers()` in both `search` and `execute` contexts, one entry per helper (`build`, `scene.*`, `pluginRead/Call/Action`, `attachScript`, `createScript`, `values.*`, `batch`, `requestTier`, `unlockedTiers`, `permissions`, `checkpoint`) with `{name, signature, mode, tier, description, example}`
- [x] 4.2 Add the `api.helpers()` typing to `generate_dts` in `src/api_index.cpp`; verify `npm exec tsc --noEmit` on the generated surface
- [x] 4.3 Verify in `search` context that `api.helpers()` returns all entries without touching live editor state

## 5. Collection unwrapping + permissions encoding fix

- [x] 5.1 In `variant_to_js` (`src/js_worker.cpp`), unwrap `{$t:"Array",values}` to a JS array and `{$t:"Dictionary",entries}` with all-string keys to a JS object, preserving `$t` for typed/packed collections and non-string-keyed dictionaries; verify `get_children()` returns an iterable JS array and `NodePath`/`StringName` elements still decode
- [x] 5.2 Reproduce `godot.permissions()` returning `INVALID_VALUE_ENCODING`, fix the offending field in `permissions_summary()` (`src/api_index.cpp`); verify the summary returns tier names/policies/unlock state and denied lists per the Permission introspection spec scenarios
- [x] 5.3 Verify round-trip: an unwrapped array passed back into `godot.call` as an argument still encodes correctly

## 6. Compact `effects` result

- [x] 6.1 In the execute-result assembly (`src/js_worker.cpp`), emit `effects` as `{counts: {"op": n}, total}` when `!verbose` and keep the ordered log under `verbose: true`; verify a large `godot.build` returns the summary and `verbose` restores per-op entries

## 7. Build and regression verification

- [x] 7.1 Rebuild both native binaries: `cmake --build build -j 6` and `cmake --build build-windows -j 6`
- [x] 7.2 Run `python3 tools/test_generic_bridge.py` and the fast inventory check `python3 -B tools/test_generic_bridge.py --inventory-only`; fix regressions
- [x] 7.3 If `api-surface.d.ts` changed, regenerate via `--write-declarations` and typecheck with `tsc --noEmit --strict --target ES2020`; run `git diff --check`
