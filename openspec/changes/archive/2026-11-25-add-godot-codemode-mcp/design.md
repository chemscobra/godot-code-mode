# Design

## Context

Greenfield: no existing addon code. Host project `godot-cm/` is an empty Godot 4.7.2 project that will serve as the demo/dev host. Godot 4.7.2 stable is installed on the development machine (linux-x86_64). See proposal.md for motivation and scope.

Target architecture:

```
MCP client (agent: Claude / Codex / Devin)
        |  stdio   (stdio-only clients)
        v
+-- stdio adapter (thin proxy, owns token) --+
        |  Streamable HTTP + session token
        v
+----------- GDExtension core -------------+
|  HTTP server  -->  search / execute      |
|                        |                 |
|                  QuickJS worker  <-->  API index (immutable)
|                        |  promises       |
|                        v                 |
|            command queue --> MAIN THREAD |
|            validate / allowlist / txns   |
+------------------------------------------+
        |
EditorPlugin + ClassDB + EditorInterface + EditorUndoRedoManager
```

## Goals / Non-Goals

**Goals:**
- Two-tool MCP surface (`search`, `execute`) reachable by stdio-only and HTTP-capable clients.
- Read-only by default; writes are authorized, bounded, and reversible.
- Editor stays responsive: JS runs off the main thread; native calls are dispatched with a per-frame budget.
- One undo action per `execute` write call.

**Non-Goals:**
- Strong process isolation (in-process sandbox only, documented honestly — see Risks).
- Universal API surface: only an explicit allowlist is exposed.
- GDScript execution, OS/filesystem/network access from JS, exported-game support.
- Multi-undo-step writes, script editing, viewport capture (post-MVP).

## Decisions

### D1. Native integration
Light `EditorPlugin` starts/stops the service; GDExtension core (godot-cpp, 4.7 branch) owns transport, runtime, and bridge. Classes registered at editor-appropriate init level. Per-platform binaries packaged under `addon/godot_cm/bin/`; server never starts in exported games (guard on `Engine::is_editor_hint` / build profile).

### D2. Runtime — QuickJS-NG in a worker thread
Dedicated worker, no native modules, no network/filesystem bindings exposed. Fresh JS context per execution. Worker processes promises/results; the main thread never blocks waiting on it.
Alternative considered: separate process + IPC — stronger isolation for untrusted code, but doubles the bridge complexity (Variant serialization over IPC) and the deployment surface. Deferred as a post-MVP evolution; specs must not assume in-process.

### D3. Async bridge — every `godot.*` call is a promise
Worker enqueues ops; main thread resolves them under a per-frame budget (~2 ms initial) and posts results back. Simplest correct model: no locks, no cross-thread Object access.
Cost acknowledged: N awaited calls = N dispatches. A `godot.batch([...])` op-list primitive is a possible fast path — decide with spike measurements, not now (see Open Questions). Sync bridge calls rejected: they would stall the worker↔main contract and complicate cancellation.

### D4. Controlled reflection
Combine `extension_api.json`, ClassDB, and runtime object metadata to build the API index. Generate TypeScript declarations for the *authorized* surface only — reflection discovers capabilities, it does not grant permissions. Third-party scripts/classes require explicit authorization.

### D5. Transport — HTTP is the single protocol implementation
Streamable HTTP on 127.0.0.1, configurable port, per-editor-session token, Origin validation, version negotiation per the chosen MCP spec version (`initialize`, `tools/list`, `tools/call`, cancellation).
A thin stdio adapter process ships in the MVP (not optional): it is the common denominator for Claude/Codex/Devin. The adapter is a dumb stdio↔HTTP proxy that owns the token (env var or file, not argv). No MCP logic is duplicated.

### D6. Tool contracts
- `search(code)`: runs read-only JS over the immutable metadata index. `api.classes()`, `api.describe(name)`, `api.find(query)`. Bounded results, type signatures, no live handles, no editor access.
- `execute(code, timeoutMs?, mode=read|write)`: runs an async function; host validates permissions per-op regardless of requested mode. Structured output: `ok`, `result`, `diagnostics`, `changes`, `truncated`. Errors carry stable code + message + phase; never native pointers or secrets.

### D7. Variant/JSValue bridge
Strict conversion: scalars, Array, Dictionary, Vector2/3, Color, tagged types. Opaque session handles bound to ObjectID, resolved and validated on the main thread. Strong refs for RefCounted; free unadopted temp nodes. Reject expired handles, incompatible types, unauthorized methods.

### D8. Transactions
Every supported write inside one `execute` call groups into a single `EditorUndoRedoManager` action. Record previous values, hierarchy, ownership, references. On error/cancel: roll back supported changes; if rollback fails, report partial state and block new writes pending review. Save scene only via explicit authorized action, outside the transaction.

### D9. Budgets (initial, tunable)
5 s total time, 32 MiB JS heap, 1,000 bridge ops, 256 KiB output, 2 ms dispatch/frame. Interrupt handler stops JS but cannot interrupt a blocking native call — unbounded-cost ops are excluded from MVP. Limits apply to queues and native objects too. Measure and adjust in the spike.

### D10. Repo layout (pattern B)
`addon/` = distributable tree (`.gdextension`, `plugin.cfg`, plugin.gd, `bin/`, generated `.d.ts`); `src/` + `godot-cpp/` submodule = native build (SCons/CMake at root, artifacts copied into `addon/.../bin/`); `godot-cm/` = demo host mounting the addon via `addons/godot_cm` symlink (Linux-dev fine). Alternative considered: addon inside the project — rejected to keep the distributable tree clean.

### D11. Versions
Godot minimum **4.7** (declared floor; forward-compat within 4.x expected via GDExtension), validated only against **4.7.2** for MVP. godot-cpp pinned to the 4.7 branch. First platform: linux-x86_64.

## Risks / Trade-offs

- [In-process sandbox does not contain native faults — a crash kills the editor] → Document honestly; keep isolation-by-process on the post-MVP roadmap; exclude unbounded native calls.
- [EditorInterface/EditorUndoRedoManager reachability and edge behavior from GDExtension on 4.7 is the load-bearing unknown] → Task 1 spike compiles a minimal addon and verifies access before anything else is built.
- [N-await dispatch latency makes scene building slow] → Measure in spike; `godot.batch` fast path held in reserve (D3).
- [HTTP server inside editor process is attack surface] → Loopback only + per-session token + Origin validation; adapter holds the token; no remote binding in MVP.
- [Reflected methods enable abuse even with read/write modes] → Allowlist per class/method/property enforced on every call including returned objects; block OS, process spawn, network, dynamic code loading, indirect dynamic calls, arbitrary file paths; verify symlink escapes if filesystem is ever enabled.
- [Not every `Object::callv` is reversible] → Only explicitly supported ops join transactions; failed rollback reports partial state and locks writes.

## Observability

Log execution ID, duration, op count, permissions, outcome. Never log tokens or full code by default. Editor UI shows server state and allows stopping it. Sessions and handles invalidate on project change or service stop.

## Open Questions

- ~~Is `godot.batch` warranted?~~ **Resolved by spike (1.4):** each awaited bridge op costs ~1 editor frame (measured 6.9 ms avg / 13.7 ms max round-trip on 4.7.2, ~145 fps editor). A 200-op script ≈ 1.4 s wall time. `godot.batch([ops])` fast path is therefore **warranted** — additive API, to be designed with the bridge in task group 4.
- Which write operations, if any, warrant individual user confirmation beyond session-level `mode=write` grant? — can tighten allowlist/UX later without changing specs.
- ~~Acceptable performance targets in large scenes (op latency, index build time)?~~ **Resolved (6.4):** per-op latency is frame-bound. Measured end-to-end through the full MCP path on 4.7.2: ~6.9 ms/op while the editor renders continuously, **~100 ms/op when the editor is idle** (Godot low-processor mode paces `process_frame`; each awaited op waits ≥1 frame). Consequences: (a) the default 5 s budget limits a script to ~50 sequential awaited ops in an idle editor — heavy scripts must pass a larger `timeoutMs`; (b) the `godot.batch` fast path is confirmed desirable and is queued as a post-MVP additive API; (c) index build and `.d.ts` generation are one-time plugin-startup costs, not per-request.

## Spike findings (task group 1)

- `EditorInterface` singleton exists at `MODULE_INITIALIZATION_LEVEL_EDITOR`, but `get_editor_undo_redo()` returns null until the editor finishes loading. Service start MUST be driven from `EditorPlugin._enter_tree` (confirms D1), not from library init.
- `SceneTree::process_frame` drain connections require the callable target to outlive registration — plugin must hold a strong ref to the service objects it wires (observed: freed callable silently dropped).
- MCP 2025-11-25 handshake verified end-to-end: a real client (claude mcp) completed `initialize` → `notifications/initialized` → `tools/list` against the stub in `tools/stub_mcp.py`.
