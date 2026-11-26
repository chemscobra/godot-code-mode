# godot-code-mode

MCP "code mode" for the Godot editor: two tools (`search`, `execute`) backed by a
bounded QuickJS-NG runtime inside a GDExtension addon. Agents write JavaScript
that awaits authorized editor operations. Supported edits can be grouped into
one undoable editor action; an explicit `untracked` mode also permits calls
whose effects cannot be rolled back.

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
| `execute` | Run JS with `api.*` + `godot.*` (`read`, `write`, `untracked`) |

The catalog reflects the running editor: native engine/GDExtension classes,
singleton aliases, registered project-script classes, and loaded anonymous
scripts encountered through the bridge or edited scene trees. `api.classes()`,
`api.find()` and `api.describe()` share a permission-filtered snapshot per call.
Discovery never loads or instantiates an uncached script: these entries carry
`metadata_status:"class_only"` until explicitly loaded. Cached script members,
paths, source, constructor mode and member execution modes are described too.
Godot's own rules still apply, including tool-script and signature requirements.

Inside `execute`: `godot.editor.getEditedScene()`, `godot.create(cls, ...args)`,
`godot.getSingleton(name)`, `godot.get/set(handle, prop, v)`,
`godot.call(handleOrStaticClassName, method, ...args)`,
`godot.batch([{op, ...}])`, `godot.saveScene()`, `godot.unlockWrites()`.
All `godot.*` calls return promises resolved on the editor main thread; await
all operations before returning.

| Mode | Contract |
|------|----------|
| `read` | Queries and methods/accessors declared const by the native API. |
| `write` | Supported engine-property edits and calls with known inverses, journaled as one editor Undo action; errors trigger rollback. Unknown inverses still return `NO_UNDO`. |
| `untracked` | Generic native/project-script execution after granting the separate `untracked` tier. No transaction, Undo action or rollback is promised. |

For example, first use `execute` in `read` mode to request authorization:

```js
return await godot.requestTier("untracked");
```

After approval, use another `execute` with `mode:"untracked"`:

```js
const editor = await godot.getSingleton("EditorInterface");
await godot.call(editor, "play_current_scene");
```

No plugin-specific adapter or registration is required. Unknown native/plugin
mutations, script calls/constructors/accessors and callbacks with unknown effects
use `untracked`; they are not classified as safe just because their names start
with `get_` or `is_`. Direct class/member restrictions, filesystem/network tiers
and the write lock still apply, including to batches. Invoked plugin/project
code is trusted code, not a filesystem/network sandbox: the `untracked` approval
explicitly covers its arbitrary side effects.

Responses include `mode`, `rollback_supported`, `effects` and `partial`.
On an `untracked` failure, effects may already have occurred; inspect state
before retrying. `effects` records attempted/dispatched non-reversible operations,
not a guarantee that a plugin completed its domain operation successfully.
JavaScript cancellation cannot preempt a running native method. The bridge waits
for in-flight native calls to settle before reporting their effects; if the outer
worker deadline expires first, `outcome:"unknown"` is returned instead.

Vectors/colors use tagged values such as `{$t:"Vector3",x:1,y:2,z:3}`.
Packed inputs use `{$t:"PackedByteArray",values:[0,1,255]}` (also numeric,
string, vector and color packed arrays). Other non-JSON values are opaque
`__godotValue` references that can be passed back as arguments/properties.
Object references include their native `class_name` and, when applicable,
`script_class`; use that symbol with `api.describe()` or `godot.create()`.
Unknown allocator methods may return caller-owned orphan nodes; adopt/free
them explicitly rather than expecting the bridge to delete borrowed objects.
Runtime declarations are written to `user://godot_cm-api-surface.d.ts`.

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
