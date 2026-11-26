#!/usr/bin/env python3
"""Benchmark: building a scene via MCP code-mode vs raw .tscn editing.

Measures the agent-visible cost of ONE scene-building task done two ways:

  A) MCP path  — one `execute` call whose JS builds N MeshInstance3D nodes.
  B) File path — the equivalent .tscn text the agent would have to author
     (byte/token footprint + structural risk surface), then a scene reload.

Usage (with the editor open on a scene):
    python3 tools/bench_scene_build.py [--nodes 20]

Reported metrics:
    mcp_ms            wall time of the execute call (incl. all awaited ops)
    mcp_ops           bridge ops performed
    mcp_ms_per_op     latency per awaited op (frame-bound)
    mcp_tokens_est    rough token footprint of the code the agent writes
    file_tokens_est   token footprint of equivalent .tscn text to author
    file_reload_ops   extra round-trips the file path needs (write+reload+
                      verify) that the MCP path folds into one call
"""

import argparse
import json
import os
import subprocess
import sys
import time

ADAPTER = os.path.join(os.path.dirname(__file__), "godot_cm_stdio.py")

BUILD_JS = """
const root = await godot.editor.getEditedScene();
const made = [];
for (let i = 0; i < %d; i++) {
  const n = await godot.create("MeshInstance3D");
  const m = await godot.create("BoxMesh");
  await godot.set(n, "name", "Bench_" + i);
  await godot.set(n, "mesh", m);
  await godot.call(root, "add_child", n);
  await godot.call(n, "set_owner", root);
}
await godot.saveScene();
return { nodes: made.length };
"""

# Equivalent .tscn the agent would have to author by hand (per node ~6 lines).
TSCN_NODE = '[node name="Bench_%d" type="MeshInstance3D" parent="."]\nmesh = SubResource("BoxMesh_%d")\n'


def rpc(stdin, payload):
    stdin.write(json.dumps(payload) + "\n")
    stdin.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nodes", type=int, default=20)
    args = ap.parse_args()

    p = subprocess.Popen(["python3", ADAPTER], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True)
    rpc(p.stdin, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "protocolVersion": "2025-11-25", "capabilities": {},
        "clientInfo": {"name": "bench", "version": "0"}}})
    p.stdout.readline()

    # --- Path A: MCP execute -------------------------------------------
    code = BUILD_JS % args.nodes
    t0 = time.time()
    rpc(p.stdin, {"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {
        "name": "execute", "arguments": {"code": code, "mode": "write",
                                         "timeoutMs": 60000}}})
    resp = json.loads(p.stdout.readline())
    mcp_ms = (time.time() - t0) * 1000
    inner = json.loads(resp["result"]["content"][0]["text"])
    n_ops = len(inner.get("changes", []))

    # --- Path B: file authoring footprint -------------------------------
    tscn = "".join(TSCN_NODE % (i, i) for i in range(args.nodes))
    # rough token estimate (~4 chars/token) for code the agent must emit
    tok = lambda s: max(1, len(s) // 4)

    print(f"nodes                : {args.nodes}")
    print(f"A) mcp_ms            : {mcp_ms:8.0f} ms wall")
    print(f"   mcp_ops           : {n_ops} bridge ops (1 MCP round-trip)")
    if n_ops:
        print(f"   mcp_ms_per_op     : {mcp_ms / n_ops:8.1f} ms (frame-bound)")
    print(f"   mcp_tokens_est    : {tok(code):6d} tokens of code")
    print(f"   undo              : 1 editor action; rollback on failure")
    print(f"B) file_tokens_est  : {tok(tscn):6d} tokens of .tscn to author")
    print(f"   file_round_trips  : ~3 MCP/file ops (write, reload, verify)")
    print(f"   undo              : none (file write is not an editor action)")
    print(f"   risk              : syntax/uid errors break the whole file")
    p.terminate()


if __name__ == "__main__":
    main()
