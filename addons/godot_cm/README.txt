Godot Code Mode (godot-cm) 0.3.3
================================

MCP "code mode" for the Godot editor: two tools (search, execute) backed by a
sandboxed JS worker, a journaled undo transaction, and a live catalog of the
editor API.

Install
-------
1. Copy the addons/godot_cm/ folder into your project's addons/ directory.
2. Enable "Godot Code Mode" in Project > Project Settings > Plugins.
3. While the plugin is active the editor serves MCP on
   http://127.0.0.1:8765 (loopback only). Set GODOT_CM_PORT before launching
   the editor to change the port.

Connect an agent
----------------
HTTP-capable MCP clients can register the endpoint directly — no extra
dependencies, no token handling (the session id travels in response headers):

    { "mcpServers": { "godot-cm": { "url": "http://127.0.0.1:8765/mcp" } } }

Works for Cursor, VS Code, Windsurf, Devin and similar remote-MCP clients.

Stdio-only clients need the adapter shipped in tools/ — two equivalent
implementations, use whichever runtime the machine already has:

    tools/godot_cm_stdio.py   (Python 3)
    tools/godot_cm_stdio.js   (Node.js)

Claude Code:

    claude mcp add godot-cm -- python3 <project>/addons/godot_cm/tools/godot_cm_stdio.py
    claude mcp add godot-cm -- node    <project>/addons/godot_cm/tools/godot_cm_stdio.js

Claude Desktop (claude_desktop_config.json; %APPDATA%\Claude\ on Windows,
~/Library/Application Support/Claude/ on macOS):

    {
      "mcpServers": {
        "godot-cm": {
          "command": "python3",
          "args": ["<project>/addons/godot_cm/tools/godot_cm_stdio.py"]
        }
      }
    }

    (use "command": "node" with godot_cm_stdio.js, or "py" on Windows, if
    python3 is not on PATH; restart Claude Desktop after saving)

Codex / other stdio clients — register the same command.

The adapter reads the session token from the file the editor writes
(user://godot_cm_session) — it is never passed on argv. It polls for the
file while the editor starts and retries stale tokens from other projects.

Useful env vars for the adapter:
  GODOT_CM_PORT        same port you set for the editor (default 8765)
  GODOT_CM_TOKEN_WAIT  seconds to poll for the session file (default 20)
  GODOT_CM_PROJECT     app_userdata folder name (the project's config/name)
                       to pin one project when several editors are open

Permissions
-----------
Denylist model: all engine classes except escape paths (raw OS, Script,
Engine, threading, capture devices, meta-methods). Border areas live in
unlockable tiers — network and filesystem — with per-session policies
(allow / ask / deny) editable in the panel or allowlist.json.

execute modes: read | write | untracked (default read). Responses are
compact by default; pass "verbose": true for the full per-operation log.

See the repository README.md / CHANGELOG.md for the full contract.
