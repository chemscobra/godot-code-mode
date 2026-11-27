# Design: permission-ux-consistency

## Context

Three layers gate every bridged call today — `deny_classes` (flat, hard,
read+write), `deny_members`/`restrict` (flat lists), and `tiers` (prompt or
policy unlock). The dock surfaces only tiers and auto-grant, so users cannot
see or reason about the model, and the flat per-class deny list has grown
entries that block harmless reads.

## Goals

- One visible pipeline: file → deny → tiers → restrict → allowed, with each
  stage inspectable in the dock.
- Policy expressed as named groups, not unexplained class lists.
- Read-safe classes denied for writes only, eliminating "surprise" denies on
  harmless lookups.
- Every denial tells the agent (and the user) the way forward.

## Decisions

### Unified `groups` schema

Tiers and deny lists collapse into one concept: `groups`, a map of names to
`{"classes": [...], "policy": "deny"|"write"|"ask"|"allow"|"off",
"protected"?: bool}`. One state machine for every sensitive surface:

- `deny` — blocked reads and writes (was `deny_classes`)
- `write` — reads allowed, writes denied (was `deny_write_classes`)
- `ask` — blocked until `requestTier` plus the confirmation dialog
- `allow` — granted silently (was `tier_policy: allow`/`unlocked_tiers`)
- `off` — fully allowed, classes kept in the file for re-enablement

`protected: true` groups (execution, process, interop — script/OS escapes)
limit their offered states to `deny`/`write`/`off` so the dock can never
prompt-unlock arbitrary code execution. Legacy keys (`tiers`,
`tier_policy`, `unlocked_tiers`, `auto_grant_tiers`, `deny_classes`,
`deny_groups` from the earlier iteration of this change) migrate into
`groups` on load — old files keep working; the shipped file is rewritten in
the new shape. `godot.requestTier(name)` stays as the agent-facing verb for
`ask` groups.

### `deny_write_classes` for read-safe classes

New key `deny_write_classes` denies only write-mode and untracked calls;
`read`-mode calls remain authorized. Applied to `ProjectSettings`,
`EditorSettings`, `TextServer*` (after audit — see tasks). `Crypto*`,
`AESContext`, `HMACContext`, `X509Certificate` move to a new `crypto` tier,
and `CameraServer`/`CameraFeed`/audio capture classes to a `capture` tier —
prompt-gated instead of impossible. `Mutex`/`Semaphore` drop out of deny
entirely (inert without `Thread`). Each removal is gated on an audit of what
an instantiated object of that class can do, using `api/extension_api.json`.

### Dock: one table, one control

The permissions section becomes the evaluation order, top to bottom:

1. **Policy file row**: `res://addons/godot_cm/allowlist.json`, "Open file"
   button (`EditorInterface.edit_resource`/`shell_open`), last-reload label
   fed by a `policy_reloaded` signal on `McpService` (emitted by `ApiIndex`).
2. **Group table**: one row per `groups` entry — name, example classes in
   tooltip, a combo with the group's state, and a live locked/unlocked
   indicator for `ask` groups. `protected` groups show only their safe
   subset of states. `deny_classes`/`deny_members`/`restrict` leftovers
   appear as a read-only "other restrictions" row expanding to a list.
3. **Auto-grant checkbox** (auto-unlocks `ask` groups for the session).

The dock combo only ever writes `groups.<name>.policy` — class lists stay
file-authoritative, so the panel can never corrupt a group definition.

### Panel ↔ file consistency

Dock edits call a `save_group_policy(name, state)` binding persisting
`groups.<name>.policy` into `allowlist.json`. Because the file is
hot-reloaded, each write triggers a reload that re-applies the same value
idempotently (ask→unlocks, deny/write/off→relocks per the existing
file-wins semantics). On `policy_reloaded`, the dock rebuilds its
permissions section so file edits appear immediately in the panel.
Malformed-file failures surface as a status line, not a dialog.

### Remediation hints

PERMISSION/not-authorized results gain a `remediation` string:

- Tier-locked: `"call godot.requestTier('<tier>')"` (or `"tier policy is
  deny"` when the policy refuses).
- Class in `deny_classes`: `"denied permanently by policy"`, plus a
  specialized hint where a trusted op exists (Script-family →
  `"use godot.createScript / godot.attachScript"`).
- Denied member/restrict: `"member denied; see allowlist.json restrict/
  deny_members"`.
- `deny_write_classes` in read mode: not applicable (allowed); in write mode
  the same permanent-denial hint.

## Risks / Trade-offs

- **Loosening reads on config classes**: `ProjectSettings` reads can expose
  paths/metadata — acceptable; the sensitive surface is writes, which stay
  denied.
- **Panel writes to file**: concurrent manual edits + dock writes could race;
  mitigated because dock writes go through `FileAccess` replace and the
  reload picks up the last consistent JSON.
- **Audit may keep some "borderline" classes denied**: the tasks require
  per-class justification before moving anything to a tier or write-only.

## Migration

Existing `allowlist.json` schemas keep working (`deny_groups` is additive).
The shipped file is regrouped; custom user files without the new keys behave
exactly as before.
