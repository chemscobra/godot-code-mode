# Tasks

## 1. Policy schema: groups and write-only denies

- [x] 1.1 Audit the current `deny_classes` against `api/extension_api.json` (and `api.describe`) and produce the final grouping: which classes stay `deny` (protected), which become `write`, which become `ask` groups (`capture`, `crypto`), which drop. Record the rationale per group in `allowlist.json` group names/comments.
- [x] 1.2 In `ApiIndex`, parse the unified `groups` schema (`{classes, policy, protected?}`) with migration of legacy `tiers`/`tier_policy`/`unlocked_tiers`/`auto_grant_tiers`/`deny_classes`; enforce the deny/write/ask/allow/off state machine in authorization. Rewrite the shipped `allowlist.json` in the new shape.
- [x] 1.3 Update `permissions_summary()`/`godot.permissions()` to report groups (name, classes, policy, protected, unlock state); keep `godot.permissions({class, member})` accurate for reads vs writes.
- [x] 1.4 Hot-reload must pick up the new keys with the existing pump path (no extra plumbing beyond the parsed document).

## 2. Remediation hints on denials

- [x] 2.1 In `src/bridge.cpp`, attach a `remediation` string to PERMISSION/not-authorized results: `requestTier('<tier>')` when locked and policy allows/asks, `"denied permanently by policy"` for hard denies, trusted-op hints for Script-family denials (`godot.createScript`, `godot.attachScript`), and restrict/deny_member hints.
- [x] 2.2 Verify via the fixture that a `Script` call suggests `createScript`, a `FileAccess` write suggests `requestTier('filesystem')`, and a deny-policy tier reports permanent denial.

- [x] 2.3 Give `requestTier`/`request_tier` ops a dedicated longer deadline (e.g. 120s) independent of the execute mode so a human has time to read and answer the dialog; verify a request issued in `read` mode no longer hits the 5s default.

## 3. Dock pipeline panel

- [x] 3.1 Emit `policy_reloaded` (and parse-failure status) from `ApiIndex`/`McpService`; connect the dock to rebuild the permissions section on reload.
- [x] 3.2 Rebuild `_build_dock` permissions section into the unified "Permission groups" table: policy file row (path + "Open file" + reload status), one row per `groups` entry (combo with the group's legal states — `deny`/`write`/`ask`/`allow`/`off`, restricted to `deny`/`write`/`off` on `protected` groups — plus live unlock indicator for `ask`), a read-only "other restrictions" row for `deny_classes`/`deny_members`/`restrict` remainders, auto-grant checkbox.
- [x] 3.3 Persist dock edits to `allowlist.json` via a `save_group_policy(name, state)` binding writing `groups.<name>.policy` without touching class lists — relying on hot-reload to re-apply; surface malformed-file failures in the dock status line.

## 4. Verification

- [x] 4.1 Update `tools/test_generic_bridge.py`: grouped denies still block, write-only denies allow reads, new tiers gate until unlocked, remediation hints appear in diagnostics.
- [x] 4.2 Rebuild both native binaries; run the full integration suite and the inventory check.
- [x] 4.3 Regenerate `api-surface.d.ts` if the public surface changed; typecheck; `git diff --check`; update CHANGELOG for 0.4.0 candidate.
