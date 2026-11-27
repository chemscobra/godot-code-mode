# Design

## Context

`ApiIndex::load()` parses `allowlist.json` once at service start into `policy_document`; every later `refresh()` rebuilds the catalog from that in-memory copy, and `tier_policy`/`unlocked` are only seeded `if (!loaded)`. `editor.attach_script` already demonstrates the trusted-operation pattern (plugin GDScript op + capture/restore + JS facade). Returned collections are always wire-tagged (`{$t:"Array",values}`), which JS agents must unwrap by hand. The `initialize` response already sets the `MCP-Session-Id` header; `verbose` already summarizes `changes` but not `effects`.

## Goals / Non-Goals

**Goals:**
- Policy file becomes authoritative whenever it changes on disk, with sub-second propagation via the existing `BridgePump` refresh poll.
- Scene+script authoring completes entirely over MCP without weakening `deny_classes`.
- Helper discovery and result decoding no longer require reading addon source.

**Non-Goals:**
- Changing default tier policies (`filesystem` stays `ask`); removing `deny_classes` entries.
- GET/SSE on `/mcp`; making external actions transactional.
- Redesigning the wire format for input (agents still send `$t` values; only results are unwrapped).

## Decisions

### Policy hot-reload in `ApiIndex::refresh()`
Store `policy_path` and `policy_mtime` at `load()`. At the top of `refresh()` (before the `loaded && !dirty` early return), stat the file via `FileAccess::get_modified_time`. On change: re-parse; on success replace `policy_document`, reset `tier_policy` from the file, add the file's `unlocked_tiers`/`auto_grant_tiers` into `unlocked` (additive — session grants survive), and set `dirty`. On parse failure keep the old document and log. Alternative considered: watching via `EditorFileSystem` signals — rejected, `allowlist.json` under `res://addons` does fire `filesystem_changed`, but a direct mtime check in the already-running refresh poll is simpler and also covers edits that bypass the filesystem scan.

### `editor.create_script` mirrors `attach_script`
New entry in `editor_operations.gd` `register()` with `capture`/`restore`/`tier: filesystem`. `capture` records `{path, existed, prior_source}` after validating `_script_path`; the method writes via `FileAccess`, refuses `@tool` in source (same rationale as `attach_script`), calls `EditorFileSystem.update_file` so the class lands in discovery, and `restore` deletes a created file or restores prior bytes. `godot.createScript(path, source)` is a thin `plugin_call` facade in `js_worker.cpp`, plus a d.ts line. Alternative considered: relaxing `deny_classes` for `GDScript.new()` + `ResourceSaver` — rejected; it would expose the whole `Script` surface for a need that one audited op covers.

### Session token duplicated into `initialize` result body
Add the token to `result` in `handle_initialize` (field name `sessionId`, non-standard but harmless under MCP's tolerant result schema). Header stays authoritative for conforming clients. Alternative: JSON-RPC extension field — same idea, body field is simpler for curl/manual clients.

### `api.helpers()` as a static table
A C++ function returning a fixed `Array` of `{name, signature, mode, tier, description, example}` dictionaries, callable in both `search` and `execute` contexts (pure metadata, no live access). Chosen over publishing helpers as `operation:*` catalog entries: helpers are JS-prelude conveniences, not registered bridge ops, so catalog entries would fake an `execution` contract they don't have. The `api-surface.d.ts` gains `api.helpers()` typing.

### Unwrap collections in `variant_to_js`, not in `encode`
`variant_to_js` (JS-boundary) unwraps `{$t:"Array", values}` → JS array and `{$t:"Dictionary", entries}` with all-string keys → JS object; non-string-keyed dictionaries and typed/packed envelopes keep `$t`. Doing it here rather than in `ValueCodec::encode` keeps the canonical wire format unchanged for persisted/metadata paths (checkpoints, `values.read`, catalogs stay tagged and round-trip safe). Risk: code that round-trips a returned array back into `godot.call` still works — plain JS arrays decode as `Array` anyway.

### `effects` summary mirrors `changes`
Where the execute result is assembled (`js_worker.cpp`), when `!verbose` replace `effects` with `{counts: {"op": n}, total}`; `verbose` keeps the ordered log. Untracked effects are all `rollbackable:false`, so the summary loses no actionable info.

### MIXED_EFFECTS documentation only
One sentence appended to the `execute` tool description; semantics unchanged — alternatives (a "post-external" write mode) would weaken the transactional model for marginal gain.

## Risks / Trade-offs

- [Policy reload resets `tier_policy`, clobbering dock OptionButton overrides] → acceptable: the file is documented source of truth; dock remains for session-only tweaks. Reload event is printed to the editor log.
- [Hot-write of `allowlist.json` mid-save parses partially] → reload only applies on successful parse; mtime check re-tries next refresh.
- [Unwrapping collections could confuse code that forwards the raw `$t` envelope to another op] → plain arrays/dicts are valid input already, so round-trips still work.
- [`godot.permissions()` INVALID_VALUE_ENCODING root cause unknown] → task list starts with a repro step; the summary fix is verified against the existing spec scenarios.

## Migration Plan

None — all changes are additive or fix observable behavior to match existing specs. No file or config migrations; `allowlist.json` schema is unchanged.
