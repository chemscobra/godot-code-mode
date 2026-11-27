# Spec Delta

## ADDED Requirements

### Requirement: Policy hot-reload
The permission policy file SHALL be re-parsed when it changes on disk,
without requiring a plugin or editor restart. A reload SHALL apply the
file's declared tier policies and configured grants; tier grants already
made during the session SHALL be preserved. An unparseable file SHALL keep
the previously loaded policy in force.

#### Scenario: Edit applies without restart
- **WHEN** `allowlist.json` is modified on disk while the service is running
- **THEN** subsequent authorization checks and the permission summary
  reflect the new policy

#### Scenario: Session grants survive reload
- **WHEN** a tier was unlocked during the session and the policy file is
  reloaded
- **THEN** the unlocked tier remains unlocked

#### Scenario: Broken file keeps prior policy
- **WHEN** the policy file is temporarily malformed (e.g. mid-save)
- **THEN** the previously loaded policy remains in force and no partial
  policy is applied

### Requirement: Helper surface discovery
`api.helpers()` SHALL return descriptive metadata for every JavaScript
convenience helper exposed in the `execute` context (e.g. `godot.build`,
`godot.scene.*`, `pluginRead/Call/Action`, `attachScript`, `createScript`,
`values.*`, `batch`, `requestTier`, `unlockedTiers`, `permissions`,
`checkpoint`), including name, signature, mode requirements, and a short
description, so agents can discover them without reading addon source.

#### Scenario: Helpers listed with signatures
- **WHEN** `search` or `execute` code calls `api.helpers()`
- **THEN** it receives one entry per helper with its signature and
  description

#### Scenario: Helper docs stay offline-safe
- **WHEN** `api.helpers()` runs inside `search`
- **THEN** it returns static metadata only — no live editor access occurs
