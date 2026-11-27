# Proposal

## Why

Benchmark runs against v0.3.2 showed that MCP authoring flows cost more tokens and more failed calls than file editing, and that the permission policy file is not re-read at runtime. Two holes are near-blocking: there is no sanctioned way to create a `.gd` script (every `Script`/`GDScript` path is denied, and `editor.attach_script` requires an existing file), and `allowlist.json` edits never reach the in-memory policy without a plugin restart. The remaining items are discovery, serialization, and response-size friction that inflate benchmark token cost.

## What Changes

- Hot-reload the permission policy: re-parse `allowlist.json` when the file changes on disk and re-apply declared tier policies and grants, instead of requiring a plugin restart.
- New trusted plugin operation `editor.create_script` (plus `godot.createScript(path, source)` facade) that writes a non-`@tool` `.gd` file undoably — capture/restore like `editor.attach_script` — so scene+script authoring is possible entirely over MCP while `Script`/`GDScript` stay denied.
- Include the session token in the `initialize` response body (in addition to the `MCP-Session-Id` header) so HTTP clients that cannot read headers can complete the handshake.
- Document the external-action isolation rule in the `execute` tool description so `MIXED_EFFECTS` is predictable instead of discovered by failure.
- Expose the JS helper surface (`godot.build`, `godot.scene.*`, `pluginRead/Call/Action`, `attachScript`, `createScript`, `values.*`, `batch`, `requestTier`, `unlockedTiers`, `permissions`, `checkpoint`) through `api.helpers()` so agents can discover signatures without reading addon source on disk.
- Unwrap returned `Array`/`Dictionary` wire values into native JS arrays/objects in `godot.call` results (keeping `$t` forms only where they carry type information), and fix `godot.permissions()` returning `INVALID_VALUE_ENCODING` instead of the summary its spec already requires.
- Collapse the per-op `effects` log into a summary by default (mirroring the existing `changes` summary and `verbose` escape), since untracked effects are never rollbackable and the verbose log is mostly output tokens.

## Capabilities

### New Capabilities

(none — all changes map onto existing capabilities)

### Modified Capabilities

- `api-discovery`: add requirements for policy hot-reload and for the discoverable helper surface (`api.helpers()`).
- `editor-transactions`: add a requirement for the trusted `editor.create_script` operation alongside `editor.attach_script`.
- `godot-editor-bridge`: require native JS array/object unwrapping for returned collections; the `godot.permissions()` summary fix is a bug fix against the existing Permission introspection requirement.
- `mcp-transport`: require the session token in the `initialize` response body and the external-action rule in the `execute` tool contract.
- `code-execution`: extend the structured-results requirement so `effects` is summarized by default with `verbose` restoring the log.

## Impact

- `src/api_index.cpp` / `api_index.hpp`: policy path + mtime tracking, reload inside `refresh()`.
- `addons/godot_cm/editor_operations.gd`: new `editor.create_script` operation and JS facade in `src/js_worker.cpp`; no changes to `deny_classes`.
- `src/mcp_service.cpp`: `initialize` result gains a session field; `execute` tool description updated.
- `src/js_worker.cpp`: `api.helpers()` table, collection unwrapping in `variant_to_js`, compact `effects` in the execution result.
- `src/bridge.cpp` / `src/value_codec.cpp`: reproduce and fix the `permissions_summary` encoding failure.
- Out of scope: removing entries from `deny_classes`, changing the default `filesystem` tier policy (`ask`), and GET/SSE support on `/mcp`.
- Docs/specs: five capability specs get requirement deltas.
