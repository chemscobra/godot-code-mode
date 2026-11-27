#!/usr/bin/env python3
"""Benchmark: agent builds a Godot demo via MCP vs plain file editing.

Runs the SAME spec (tools/fixtures/prompts/bench-spec.md) N times per
variant against fresh isolated Godot projects:

  mcp   — `claude -p` with --mcp-config pointing at the godot-cm bridge
          running inside a headless Godot editor.
  file  — `claude -p` with --strict-mcp-config and no MCP servers; the
          agent must author .tscn/.gd directly and verify headless.

Metrics come from Claude Code's `--output-format json` result object
(usage tokens, num_turns, duration_ms, total_cost_usd, is_error) and are
tabulated to CSV + Markdown under --out.

Usage:
    python3 tools/bench_agent_variants.py --godot /path/to/godot \
        --runs 3 --out bench-runs/

Requires `claude` (Claude Code) on PATH and a signed-in account.
"""

import argparse
import csv
import json
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROMPTS = REPO / "tools" / "fixtures" / "prompts"

SPEC = (PROMPTS / "bench-spec.md").read_text()
VARIANT_PROMPT = {
    "mcp": (PROMPTS / "bench-variant-mcp.md").read_text(),
    "file": (PROMPTS / "bench-variant-file.md").read_text(),
}

PROJECT_GODOT = """config_version=5

[application]
config/name="Bench Agent Variants"

[editor_plugins]
enabled=PackedStringArray("res://addons/godot_cm/plugin.cfg")

[rendering]
renderer/rendering_method="gl_compatibility"
"""


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def make_project(directory: Path):
    """Minimal isolated project with the godot_cm addon enabled."""
    addons = directory / "addons"
    addons.mkdir(parents=True)
    shutil.copytree(REPO / "addons" / "godot_cm", addons / "godot_cm")
    cache = directory / ".godot"
    cache.mkdir()
    (cache / "extension_list.cfg").write_text(
        "res://addons/godot_cm/godot_cm.gdextension\n")
    (directory / "project.godot").write_text(PROJECT_GODOT)


def launch_editor(godot: str, directory: Path, port: int):
    """Import pass (both variants), then keep a headless editor running
    so the godot-cm MCP bridge is reachable. Returns (proc, log); proc
    is None for the file variant (no editor — avoids a project lock when
    the agent runs `godot --headless` itself)."""
    subprocess.run([godot, "--headless", "--editor", "--path", str(directory),
                    "--import", "--quit"],
                   capture_output=True, text=True, timeout=180, check=True)
    if port is None:
        return None, None
    log = open(directory / "editor.log", "w+")
    env = dict(**__import__("os").environ, GODOT_CM_PORT=str(port))
    proc = subprocess.Popen(
        [godot, "--headless", "--editor", "--path", str(directory)],
        env=env, stdout=log, stderr=subprocess.STDOUT)
    return proc, log


def wait_mcp(port: int, proc: subprocess.Popen, deadline_s=90):
    url = f"http://127.0.0.1:{port}/mcp"
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                       "params": {"protocolVersion": "2025-11-25",
                                  "capabilities": {},
                                  "clientInfo": {"name": "bench", "version": "0"}}
                       }).encode()
    deadline = time.monotonic() + deadline_s
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"editor exited {proc.returncode} before MCP was up")
        try:
            req = urllib.request.Request(
                url, body, {"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=5) as resp:
                json.load(resp)
            return
        except Exception:
            time.sleep(0.5)
    raise TimeoutError("MCP server did not come up")


def run_agent(variant: str, project: Path, port: int, model: str | None,
              outdir: Path, timeout_s: int):
    prompt = SPEC + "\n\n" + VARIANT_PROMPT[variant]
    (project / "PROMPT.md").write_text(prompt)

    cmd = ["claude", "-p", prompt,
           "--output-format", "json",
           "--dangerously-skip-permissions",
           "--strict-mcp-config"]
    if variant == "mcp":
        cfg = project / "mcp-config.json"
        cfg.write_text(json.dumps({"mcpServers": {"godot-cm": {
            "type": "http", "url": f"http://127.0.0.1:{port}/mcp"}}}))
        cmd += ["--mcp-config", str(cfg)]
    if model:
        cmd += ["--model", model]

    t0 = time.monotonic()
    result = subprocess.run(cmd, cwd=project, capture_output=True,
                            text=True, timeout=timeout_s)
    wall_s = time.monotonic() - t0

    raw = outdir / "agent-stdout.json"
    raw.write_text(result.stdout)
    (outdir / "agent-stderr.txt").write_text(result.stderr)

    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError:
        return {"variant": variant, "error": "non-json claude output",
                "wall_s": round(wall_s, 1), "rc": result.returncode}

    usage = report.get("usage") or {}
    return {
        "variant": variant,
        "error": None if not report.get("is_error") else "is_error",
        "rc": result.returncode,
        "wall_s": round(wall_s, 1),
        "duration_ms": report.get("duration_ms"),
        "num_turns": report.get("num_turns"),
        "cost_usd": report.get("total_cost_usd"),
        "input_tokens": usage.get("input_tokens"),
        "output_tokens": usage.get("output_tokens"),
        "cache_create_tokens": usage.get("cache_creation_input_tokens"),
        "cache_read_tokens": usage.get("cache_read_input_tokens"),
        "session_id": report.get("session_id"),
        "result_excerpt": (report.get("result") or "")[-300:],
    }


def summarize(rows):
    out = []
    for variant in ("mcp", "file"):
        ok = [r for r in rows if r["variant"] == variant and not r.get("error")]
        if not ok:
            continue
        out.append({
            "variant": variant, "n": len(ok),
            "output_tokens_mean": round(statistics.mean(r["output_tokens"] for r in ok if r["output_tokens"] is not None)),
            "input_tokens_mean": round(statistics.mean(r["input_tokens"] for r in ok if r["input_tokens"] is not None)),
            "turns_mean": round(statistics.mean(r["num_turns"] for r in ok if r["num_turns"] is not None), 1),
            "wall_s_mean": round(statistics.mean(r["wall_s"] for r in ok), 1),
            "cost_usd_mean": round(statistics.mean(r["cost_usd"] for r in ok if r["cost_usd"] is not None), 4),
            "failures": len([r for r in rows if r["variant"] == variant]) - len(ok),
        })
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--godot", default="godot")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--variants", default="mcp,file")
    ap.add_argument("--model", default=None)
    ap.add_argument("--timeout", type=int, default=1200,
                    help="per-run agent timeout (s)")
    ap.add_argument("--out", default=None, help="results dir (default: bench-runs-<ts>)")
    ap.add_argument("--keep-projects", action="store_true",
                    help="keep project dirs (default keeps them anyway under --out)")
    args = ap.parse_args()

    variants = args.variants.split(",")
    outroot = Path(args.out or f"bench-runs-{time.strftime('%Y%m%d-%H%M%S')}").resolve()
    outroot.mkdir(parents=True, exist_ok=True)
    rows = []

    for run in range(args.runs):
        for variant in variants:
            name = f"run{run:02d}-{variant}"
            project = outroot / name / "project"
            agent_out = outroot / name
            agent_out.mkdir(parents=True, exist_ok=True)
            print(f"[{name}] preparing project in {project}", flush=True)
            make_project(project)

            port = free_port() if variant == "mcp" else None
            proc, log = launch_editor(args.godot, project, port)
            try:
                if proc is not None:
                    wait_mcp(port, proc)
                    print(f"[{name}] editor+MCP up on :{port}", flush=True)
                print(f"[{name}] running agent", flush=True)
                row = run_agent(variant, project, port, args.model,
                                agent_out, args.timeout)
            finally:
                if proc is not None:
                    proc.terminate()
                    try:
                        proc.wait(15)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                    log.close()
            row["run"] = run
            rows.append(row)
            print(f"[{name}] done: {row.get('num_turns')} turns, "
                  f"{row.get('output_tokens')} out tok, "
                  f"{row.get('wall_s')}s, err={row.get('error')}", flush=True)

    csv_path = outroot / "results.csv"
    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    summary = summarize(rows)
    md = outroot / "summary.md"
    with md.open("w") as f:
        f.write("| variant | n | out tok mean | in tok mean | turns | wall s | cost $ | fails |\n")
        f.write("|---|---|---|---|---|---|---|---|\n")
        for s in summary:
            f.write(f"| {s['variant']} | {s['n']} | {s['output_tokens_mean']} "
                    f"| {s['input_tokens_mean']} | {s['turns_mean']} "
                    f"| {s['wall_s_mean']} | {s['cost_usd_mean']} | {s['failures']} |\n")

    print(f"\nResults: {csv_path}\nSummary: {md}")
    for s in summary:
        print(s)


if __name__ == "__main__":
    main()
