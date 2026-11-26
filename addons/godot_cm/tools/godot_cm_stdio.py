#!/usr/bin/env python3
"""godot-cm stdio adapter: proxies MCP stdio (line-delimited JSON-RPC)
to the editor's Streamable HTTP endpoint.

Token ownership (design D5): the adapter reads the session token from
$GODOT_CM_TOKEN, $GODOT_CM_TOKEN_FILE, or the newest
~/.local/share/godot/app_userdata/*/godot_cm_session file the editor wrote.
Never takes the token from argv.

Usage as MCP server config:
  {"command": "python3", "args": ["/path/to/godot_cm_stdio.py"]}
"""
import json
import os
import sys
import urllib.request
import urllib.error
import glob

PORT = int(os.environ.get("GODOT_CM_PORT", "8765"))
URL = f"http://127.0.0.1:{PORT}/mcp"


def appdata_roots():
    """Godot user data dirs per platform."""
    if os.name == "nt":
        yield os.path.join(os.environ.get("APPDATA", ""), "Godot")
    elif sys.platform == "darwin":
        yield os.path.expanduser("~/Library/Application Support/Godot")
    else:
        yield os.path.expanduser("~/.local/share/godot")


def find_token() -> str:
    if os.environ.get("GODOT_CM_TOKEN"):
        return os.environ["GODOT_CM_TOKEN"].strip()
    token_file = os.environ.get("GODOT_CM_TOKEN_FILE")
    if token_file and os.path.exists(token_file):
        return open(token_file).read().strip()
    candidates = []
    for root in appdata_roots():
        candidates += glob.glob(
            os.path.join(root, "app_userdata", "*", "godot_cm_session"))
    if candidates:
        newest = max(candidates, key=os.path.getmtime)
        return open(newest).read().strip()
    return ""


def post(payload: dict, session_id: str):
    body = json.dumps(payload).encode()
    req = urllib.request.Request(URL, data=body,
                                 headers={"Content-Type": "application/json"})
    if session_id:
        req.add_header("MCP-Session-Id", session_id)
    try:
        with urllib.request.urlopen(req, timeout=30) as res:
            new_sid = res.headers.get("MCP-Session-Id")
            data = res.read()
            return res.status, data, new_sid or session_id
    except urllib.error.HTTPError as e:
        return e.code, e.read(), session_id
    except OSError as e:
        return 0, json.dumps({
            "jsonrpc": "2.0", "id": payload.get("id"),
            "error": {"code": -32000,
                      "message": f"godot-cm unreachable at {URL}: {e}"},
        }).encode(), session_id


def main():
    session_id = find_token()
    out = sys.stdout
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue
        status, data, session_id = post(payload, session_id)
        if status in (202, 204) or not data:
            continue  # notifications have no response body
        out.write(data.decode(errors="replace") + "\n")
        out.flush()


if __name__ == "__main__":
    main()
