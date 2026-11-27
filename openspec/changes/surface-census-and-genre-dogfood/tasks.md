# Tasks

## 1. Surface census + snapshot

- [ ] 1.1 Census script: one `execute` iterates the catalog, classifies every member into `allowed-read|allowed-write|tier-locked|denied|no-undo`, returns bucket counts + violations
- [ ] 1.2 Emit census JSON; diff against `tools/expected_surface.json` (generate initial snapshot)
- [ ] 1.3 Verify: unexplained bucket empty; snapshot diff fails on unrefreshed bucket changes

## 2. Execution sampling

- [ ] 2.1 Deterministic const sample runs in read mode with zero `PERMISSION`
- [ ] 2.2 Every journaled inverse applied then force-failed; prior state restored; single undo step on success

## 3. Multi-genre dogfood

- [ ] 3.1 Fixtures under `tools/fixtures/`: platformer (TileMapLayer + CharacterBody2D), UI (Control + theme), signals
- [ ] 3.1a 3D baseline: replay `tools/fixtures/prompts/monument-valley-3d.md` (the v0.2.1 dogfood prompt) and compare calls/bytes vs the original ~17 KB effects invoice
- [ ] 3.2 Per-genre scenario driver producing friction invoice: calls, blocks+reasons, response bytes
- [ ] 3.3 Verify: expected denials explained; `NO_UNDO` gaps (set_cell, connect, …) reported; byte budgets hold

## 4. MCP contract audit

- [ ] 4.1 Assert `tools/list` schemas valid; report annotation presence (`readOnlyHint`/`destructiveHint`)
- [ ] 4.2 Verify `structuredContent` ↔ text parity, domain error codes on correct JSON-RPC errors, cross-session handle rejection

## 5. Independent MCP client

- [ ] 5.1 Add `mcp` Python SDK as test-only dep (pinned)
- [ ] 5.2 `tools/test_mcp_client.py`: own handshake + tools/list + write transaction; results match harness

## 6. Report + follow-ups

- [ ] 6.1 Dump `no-undo` inventory to report as backlog input
- [ ] 6.2 Document findings; file follow-up change(s) for new inverses / `commitIrreversible` / missing annotations
