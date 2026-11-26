#!/usr/bin/env python3
"""Minimal Streamable HTTP MCP stub server for handshake verification (task 1.4).

Implements enough of MCP spec 2025-11-25 for a real client to:
initialize -> notifications/initialized -> tools/list
"""
import json
import secrets
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTOCOL_VERSION = "2025-11-25"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765

TOOLS = [
    {
        "name": "search",
        "description": "Run read-only JS over the API metadata index.",
        "inputSchema": {
            "type": "object",
            "properties": {"code": {"type": "string"}},
            "required": ["code"],
        },
    },
    {
        "name": "execute",
        "description": "Run an async JS function against the editor bridge.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "code": {"type": "string"},
                "timeoutMs": {"type": "integer"},
                "mode": {"type": "string", "enum": ["read", "write"], "default": "read"},
            },
            "required": ["code"],
        },
    },
]


def result(req_id, res):
    return {"jsonrpc": "2.0", "id": req_id, "result": res}


class Handler(BaseHTTPRequestHandler):
    session_id = secrets.token_hex(16)

    def _send_json(self, payload, status=200, session=True):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        if session:
            self.send_header("MCP-Session-Id", self.session_id)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != "/mcp":
            self._send_json({"error": "not found"}, 404)
            return
        length = int(self.headers.get("Content-Length", 0))
        try:
            msg = json.loads(self.rfile.read(length))
        except Exception:
            self._send_json(result(None, None), 400)
            return
        print(f"[stub] <- {msg.get('method', msg)}", flush=True)

        method = msg.get("method")
        req_id = msg.get("id")
        if method == "initialize":
            self._send_json(result(req_id, {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "godot-cm-stub", "version": "0.1.0"},
            }))
        elif method == "tools/list":
            self._send_json(result(req_id, {"tools": TOOLS}))
        elif method == "notifications/initialized" or method == "notifications/cancelled":
            self._send_json(None and {} or {}, 202)  # no body semantics for notifications
        elif method == "ping":
            self._send_json(result(req_id, {}))
        elif method == "tools/call":
            self._send_json(result(req_id, {
                "content": [{"type": "text", "text": "{\"ok\":true,\"stub\":true}"}],
                "isError": False,
            }))
        elif method is None:
            self._send_json({}, 202)
        else:
            self._send_json({"jsonrpc": "2.0", "id": req_id,
                             "error": {"code": -32601, "message": "method not found"}})

    def do_GET(self):
        # SSE stream for server->client messages: not needed for the handshake.
        self.send_response(405)
        self.send_header("Allow", "POST")
        self.end_headers()

    def do_DELETE(self):
        self._send_json({}, 200, session=False)

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    print(f"[stub] MCP stub on http://127.0.0.1:{PORT}/mcp", flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
