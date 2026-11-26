# Proposal

## Why

Agents operating Godot through granular MCP tools consume large amounts of context and require many sequential calls for compound changes (e.g. create a node, attach a mesh, set properties, re-parent). A programmable interface with progressive discovery and bounded code execution — two tools, `search` and `execute` — collapses those workflows into single calls, following the "code mode" pattern proven by Cloudflare for MCP servers.

## What Changes

- Add a Godot 4 addon with a C++ GDExtension core (godot-cpp), a QuickJS-NG JavaScript runtime, a local MCP server, and a controlled reflection bridge to the editor.
- Expose exactly two MCP tools: `search` (read-only JS over an immutable API metadata index) and `execute` (async JS with host-side permission validation, `read`/`write` modes).
- Allow authorized inspection of scenes, and creation/modification of nodes and resources with reversible editor transactions.
- Transport: Streamable HTTP on 127.0.0.1 with per-editor-session token and Origin validation, plus a stdio adapter process so stdio-only clients (Codex, Claude Desktop) can connect. First dogfooding clients: Claude, Codex, Devin.
- Target Godot minimum 4.7 (validated against 4.7.2 stable), first platform linux-x86_64.
- Repo layout follows GDExtension convention: `addon/` (distributable), `src/` (C++), `godot-cpp/` (submodule), `godot-cm/` (existing project serves as demo/host project, mounting the addon via symlink during development).
- Explicit scene saving stays a separate authorized operation, outside the reversible transaction.

## Capabilities

### New Capabilities

- `mcp-transport`: Local MCP server lifecycle, Streamable HTTP transport on loopback, session-token auth, version negotiation, tool contracts (`search`/`execute`), and the stdio adapter for stdio-only clients.
- `api-discovery`: The `search` tool and the immutable, filtered API metadata index it runs against; authorized-only metadata, bounded output, TypeScript declarations of the authorized surface.
- `code-execution`: The `execute` tool and QuickJS-NG runtime — bounded execution (time/memory/op/output budgets), structured results and errors, read/write modes, cancellation.
- `godot-editor-bridge`: JS↔Variant conversion, opaque session handles bound to ObjectID, main-thread dispatch queue, allowlist-enforced calls into ClassDB/EditorInterface.
- `editor-transactions`: Reversible write operations via EditorUndoRedoManager — grouped actions, rollback on failure/cancel, ownership recording, explicit save.

### Modified Capabilities

(none — no existing specs)

## Impact

- New native build: godot-cpp (4.7 branch) + QuickJS-NG vendored, SCons/CMake producing per-platform binaries in `addon/godot_cm/bin/`.
- New directories: `addon/`, `src/`, `godot-cpp/` (submodule); `godot-cm/` gains `addons/godot_cm` symlink.
- Editor lifecycle: plugin activation starts/stops the service; no server in exported games.
- No engine modifications required for MVP.
- Out of scope for MVP: universal internal API access, arbitrary GDScript execution, OS access, remote support, production game control, multi-version Godot support, viewport captures, advanced debugging, script editing.
- Open decisions deferred to spike evidence: in-process vs separate-process isolation, per-write confirmation granularity, acceptable performance metrics in large scenes.
