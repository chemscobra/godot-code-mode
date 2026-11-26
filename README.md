# godot-code-mode

MCP "code mode" for the Godot editor: two tools (`search`, `execute`) backed by a
bounded QuickJS-NG runtime inside a GDExtension addon. Agents write JavaScript
that awaits authorized editor operations; writes are grouped into one undoable
editor action per `execute` call.

- Godot floor: **4.7** (validated on 4.7.2, linux-x86_64)
- MCP spec: **2025-11-25** (Streamable HTTP on loopback + stdio adapter)
- License: **MIT** (see `LICENSE`; bundled components in
  `addons/godot_cm/THIRD_PARTY_LICENSES.txt`)

![Godot CM bottom panel: MCP endpoint, session token, and per-tier permission policies](docs/screenshots/godot_cm_panel.png)

## Install into a project

**From the Asset Library** — install, then enable **Godot Code Mode** in
Project → Project Settings → Plugins.

**Manual** — copy `addons/godot_cm/` into your project's `addons/` folder and
enable the plugin the same way (a symlink works for development).

```
your-project/
└── addons/godot_cm/
    ├── godot_cm.gdextension      # entry symbol + per-platform binaries
    ├── plugin.cfg / plugin.gd    # EditorPlugin that owns the service
    ├── bin/                    # native core (.so linux, .dll windows)
    ├── tools/godot_cm_stdio.py # stdio adapter for MCP clients
    ├── allowlist.json            # permission config: denylist, tiers, policies
    ├── api-surface.d.ts          # generated declarations for the exposed API
    ├── extension_api.json        # 4.7.2 dump, seed of the search index
    ├── LICENSE
    └── THIRD_PARTY_LICENSES.txt
```

When the plugin is active the editor serves MCP on `http://127.0.0.1:8765`
(loopback only, per-session bearer token written to
`user://godot_cm_session` — the adapter reads it, clients never see it on argv).
Set `GODOT_CM_PORT` before launching the editor to change the port; the
adapter reads the same variable.

Permissions are a **denylist**: every engine class is exposed except platform
escape paths (raw `OS`, `Script`, `Engine`, threading, capture devices, and
meta-methods like `call`/`set`/`emit_signal` that would bypass checks). Border
areas live in unlockable **tiers** — `network` (sockets, HTTP, multiplayer
peers) and `filesystem` (FileAccess, DirAccess, resource importers). Each tier
has a policy settable per session in the panel or in `allowlist.json`:
`allow` grants `godot.requestTier()` instantly, `ask` shows a confirmation
dialog, `deny` refuses silently. See `allowlist.json` comments for the full
schema.

## Client configuration

HTTP-capable clients can point at the loopback URL with the session token.
For stdio-only clients use the adapter shipped inside the addon at
`addons/godot_cm/tools/godot_cm_stdio.py`:

**Claude Code**

```
claude mcp add godot-cm -- python3 your-project/addons/godot_cm/tools/godot_cm_stdio.py
```

**Codex / other stdio clients** — register an equivalent stdio command:

```
python3 your-project/addons/godot_cm/tools/godot_cm_stdio.py
```

The adapter forwards stdin/stdout JSON-RPC to the HTTP endpoint, attaching
the token it reads from the session file.

## Tools

| Tool      | Purpose                                                    |
|-----------|------------------------------------------------------------|
| `search`  | Run JS against the immutable API index (`api.*` only)      |
| `execute` | Run JS with `api.*` + `godot.*` bridge (`mode`: read/write)|

Inside `execute`: `godot.editor.getEditedScene()`, `godot.create(cls)`,
`godot.get/set(handle, prop, v)`, `godot.call(handle, m, ...)`,
`godot.batch([{op, ...}])`, `godot.saveScene()`, `godot.unlockWrites()`.
All `godot.*` calls return promises resolved on the editor main thread;
write ops require `mode:"write"`, are journaled, and commit as a single
editor Undo step — or roll back on failure. Ops with no journaled inverse
are rejected with `NO_UNDO` rather than applied unrevertably.

`godot.batch` applies a list of ops in **one** main-thread dispatch and
resolves with ordered per-element results — the fast path for op-dense
scripts (see `docs/benchmarks.md`).

Budgets per call: 5 s wall (raise via `timeoutMs`), 32 MiB heap, 1,000 ops,
256 KiB output.

## Build

Linux (host build):

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows cross-compile (MinGW-w64 toolchain installed):

```
cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=toolchains/mingw-w64.cmake \
    -DGODOTCPP_TARGET=editor -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows -j
```

Output lands directly in `addons/godot_cm/bin/` (`.so` / `.dll` — the
Windows build is fully static, no MinGW runtime DLLs needed on the target
machine). `godot-cpp/` is a submodule compiled against the project's
`extension_api.json` (dumped from Godot 4.7.2).
