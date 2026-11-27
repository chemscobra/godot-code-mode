# Spec Delta

## ADDED Requirements

### Requirement: Unified permission groups
The policy file SHALL accept `groups`, a map of names to
`{"classes": [...], "policy", "protected"?}`. Policy `deny` blocks reads
and writes; `write` blocks writes only; `ask` blocks until unlocked via
`requestTier` or the dialog; `allow` grants silently; `off` allows the
group entirely. Legacy `tiers`/`tier_policy`/`unlocked_tiers`/
`auto_grant_tiers`/`deny_classes` keys SHALL migrate into groups on load.

#### Scenario: Ask group behaves like the old tier
- **WHEN** a group carries `"policy": "ask"` (e.g. `filesystem`)
- **THEN** its classes stay inert until `requestTier` succeeds or a dialog
  grant unlocks the session

#### Scenario: Deny group behaves like the old deny_classes
- **WHEN** a group carries `"policy": "deny"` (e.g. `execution` with
  `Script`/`GDScript`)
- **THEN** calls on its classes are denied identically to `deny_classes`

#### Scenario: Write policy allows reads only
- **WHEN** a group carries `"policy": "write"` (e.g. `config` with
  `ProjectSettings`)
- **THEN** read-mode calls succeed while write-mode calls are denied

#### Scenario: Legacy file migrates
- **WHEN** a policy file uses only `tiers`/`deny_classes`/`tier_policy`
- **THEN** the effective groups produce identical authorization results as
  before

### Requirement: Protected groups
Groups marked `"protected": true` SHALL never be prompt-unlockable: their
offered states are `deny`/`write`/`off` only.

#### Scenario: Protected group rejects ask
- **WHEN** `groups.execution.protected` is true and a request tries to set
  or request `ask`/`allow` on it
- **THEN** it is refused; `Script`/`GDScript` execution stays unreachable
