# Tasks

## 1. Spike and compatibility

- [x] 1.1 Pin versions (Godot 4.7.2, godot-cpp 4.7 branch, QuickJS-NG release ≥7 days old, MCP spec version) and record them in `src/README.md`; verify the matrix: linux-x86_64 first
- [x] 1.2 Scaffold repo layout per design D10 (`addon/godot_cm/`, `src/`, `godot-cpp/` submodule, `godot-cm/addons/godot_cm` symlink) and verify the demo project opens in the editor without errors
- [x] 1.3 Compile a minimal GDExtension addon exposing an EditorPlugin and verify from the editor that `EditorInterface`, ClassDB queries, and `EditorUndoRedoManager` are reachable on 4.7.2 (build succeeds, plugin toggles, API calls return)
- [x] 1.4 Measure per-await dispatch latency (worker→main→worker round-trip) with a prototype queue and record whether `godot.batch` is warranted (resolves design Open Question 1); verify a real MCP client handshake against a stub server

## 2. Core and transport

- [x] 2.1 Implement service lifecycle (plugin enter/exit starts/stops server, session invalidation on disable/project change) and verify spec `mcp-transport` Lifecycle shutdown scenario
- [x] 2.2 Implement Streamable HTTP server on 127.0.0.1 with configurable port, per-session token auth, and Origin validation; verify `mcp-transport` Loopback and Invalid-token scenarios
- [x] 2.3 Implement JSON-RPC protocol handling: `initialize` + version negotiation, `tools/list`, `tools/call`, cancellation; verify protocol scenarios with a test harness client
- [x] 2.4 Implement the stdio adapter (stdio↔HTTP proxy, token via env/file not argv) and verify a stdio-only client can `initialize` → `tools/list` → `tools/call` end to end

## 3. Runtime and discovery

- [x] 3.1 Integrate QuickJS-NG on a dedicated worker with fresh context per execution, promise draining, and no native/net/fs bindings; verify `code-execution` Fresh context scenario
- [x] 3.2 Implement budgets (5 s, 32 MiB, 1,000 ops, 256 KiB out) and the interrupt handler; verify `code-execution` Infinite-loop and Budget-exceeded scenarios
- [x] 3.3 Implement structured results/errors (`ok`, `result`, `diagnostics`, `changes`, `truncated`; stable code+message+phase, no pointers/secrets); verify `code-execution` result/error shape scenarios
- [x] 3.4 Build the immutable API metadata index from `extension_api.json` + ClassDB filtered by the allowlist; implement `search` with `api.classes()`/`api.describe()`/`api.find()` and bounded output; verify all `api-discovery` scenarios
- [x] 3.5 Generate TypeScript declarations of the authorized surface into `addon/godot_cm/`; verify declarations cover exactly the allowlist

## 4. Editor bridge

- [x] 4.1 Implement JSValue↔Variant conversion (scalars, Array, Dictionary, Vector2/3, Color, tagged types) with strict rejection; verify `godot-editor-bridge` conversion scenario
- [x] 4.2 Implement opaque session handles bound to ObjectID with main-thread resolution and lifetime rules (strong RefCounted refs, free unadopted temp nodes); verify stale/cross-session handle scenarios
- [x] 4.3 Implement the main-thread dispatch queue with per-frame budget and cancellation of pending ops; verify `godot-editor-bridge` main-thread scenario under a burst of awaited calls
- [x] 4.4 Implement the allowlist (class/method/property) enforced on every call including returned objects; verify `godot-editor-bridge` authorization scenarios and `code-execution` read-mode scenario

## 5. Transactions

- [x] 5.1 Implement grouped undoable actions via `EditorUndoRedoManager` (prior values, hierarchy, ownership, references) covering supported write ops; verify `editor-transactions` Undo/Redo scenario
- [x] 5.2 Implement rollback on failure/cancel and the partial-state lock on rollback failure; verify both `editor-transactions` Rollback scenarios
- [x] 5.3 Implement explicit authorized scene-save action separate from transactions and external-change conflict detection; verify `editor-transactions` Explicit save and External change scenarios

## 6. Validation and distribution

- [x] 6.1 Run the full spec-scenario suite as integration tests in the real editor on linux-x86_64 (access denied, timeout, invalid handles, rollback, shutdown); verify all spec scenarios pass
- [x] 6.2 Run adversarial tests (malicious code strings, oversized payloads, concurrent calls) and verify no crash, no policy bypass, editor stays responsive
- [x] 6.3 Package the addon (binaries + `.gdextension` + plugin files + `.d.ts`) and document install + client config for Claude/Codex/Devin; verify a clean install into a fresh project works per the docs
- [x] 6.4 Measure latency and responsiveness in a large scene and record results against budgets (resolves design Open Question 3); verify findings documented

## Workflow follow-up

- Archive the change after review requirements are satisfied and verify the archived result.
