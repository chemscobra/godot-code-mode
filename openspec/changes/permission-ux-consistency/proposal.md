# Change: Unify permission visibility, grouping and remediation

## Why

The permission model works correctly but is invisible and inconsistent:

- The dock shows only tier policies and the auto-grant checkbox. Users never
  learn that `allowlist.json` exists, that ~47 classes are hard-denied, or why
  a call failed.
- `deny_classes` blocks reads and writes alike, so harmless lookups
  (`ProjectSettings.get_setting`, `TextServer`) fail identically to real
  escapes — "something denied that nobody predicted" keeps surfacing in tests.
- Hard deny and unlockable tier are per-class flat lists; there is no grouping,
  so borderline classes end up permanently denied instead of prompt-gated.
- `PERMISSION`/`denied` diagnostics name the blocked member but not the
  sanctioned path forward (`requestTier`, a trusted op, or "denied
  permanently").

## What Changes

- **Unified `groups` schema**: tiers and deny lists collapse into one map —
  each group is `{"classes": [...], "policy": deny|write|ask|allow|off,
  protected?}`. Legacy keys (`tiers`, `tier_policy`, `deny_classes`,
  `unlocked_tiers`, `auto_grant_tiers`) migrate on load; `requestTier` stays
  as the agent verb for `ask` groups. `protected` groups (script/OS escapes)
  can never be prompt-unlocked.
- **Dock shows one "Permission groups" table**: every group gets a row with
  a single state combo — no tier-vs-group distinction for the user. Policy
  file path + "Open file" + last-reload indicator; dock edits write only
  `groups.<name>.policy` back to the file (lists stay file-authoritative);
  panel rebuilds on hot-reload.
- **Actionable diagnostics**: permission failures include a `remediation`
  hint — `requestTier('<name>')`, the trusted op to use instead, or
  "denied permanently by policy".

## Capabilities

### New Capabilities
- `permission-console`: the dock panel surfaces the full permission pipeline —
  policy file, deny groups, tier state, restrict — read-only where the file
  owns the data, editable where the panel owns the tier.

### Modified Capabilities
- `api-discovery`: policy schema gains `deny_groups` and `deny_write_classes`;
  tier set extends with `capture` and `crypto`; `godot.permissions()` reports
  the new fields.
- `godot-editor-bridge`: permission-failure diagnostics carry a remediation
  hint so agents can plan without trial-and-error.

## Impact

- `addons/godot_cm/plugin.gd`: dock permissions section rebuilt around the
  pipeline layout; policy edit write-back; reload refresh hook.
- `addons/godot_cm/allowlist.json`: regrouped into `deny_groups` /
  `deny_write_classes`; new tiers `capture`, `crypto`.
- `src/api_index.cpp/hpp`: parse new schema keys; read-vs-write deny split;
  new tiers; expose deny groups to `permissions_summary()`.
- `src/bridge.cpp`: remediation hints on PERMISSION/not-authorized results.
- `tools/test_generic_bridge.py`: coverage for new schema, read-safe denies,
  hints, and panel-side behavior where fixture-reachable.

## Out of Scope

- Removing `deny_classes` entirely or unlocking `Script`/`GDScript` execution.
- A second TCP channel to the running game (runtime introspection).
- Per-member write/ask policies beyond class granularity.
