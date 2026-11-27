# Spec Delta

## ADDED Requirements

### Requirement: One permission table
The dock SHALL present every `groups` entry as a row in a single
"Permission groups" table: name, example classes, a state combo, and a
live locked/unlocked indicator for `ask` groups. The concepts "tier" and
"deny group" SHALL NOT appear separately in the UI.

#### Scenario: States offered per group
- **WHEN** the user opens a group's combo
- **THEN** unprotected groups offer `deny`/`write`/`ask`/`allow`/`off`
  while `protected` groups offer only `deny`/`write`/`off`

#### Scenario: Ask group shows live state
- **WHEN** an `ask` group was unlocked this session
- **THEN** its row shows unlocked status next to the `ask` policy

### Requirement: Panel edits state, file owns lists
Combo edits SHALL write only `groups.<name>.policy` to `allowlist.json`;
class lists SHALL remain editable only in the file.

#### Scenario: State change persists
- **WHEN** the user flips a group to `ask` in the dock
- **THEN** `groups.<name>.policy` becomes `ask` in the file while its
  class list is untouched, and the change applies on the next reload tick

#### Scenario: Flat leftovers read-only
- **WHEN** `deny_classes`, `deny_members`, or `restrict` entries exist
  outside any group
- **THEN** a collapsed "other restrictions" row lists them read-only

### Requirement: File-first refresh
On `policy_reloaded` the dock SHALL rebuild the permissions section so file
edits appear immediately; a parse failure SHALL surface as a status line,
not a dialog.

#### Scenario: External edit visible
- **WHEN** `allowlist.json` changes on disk while the editor runs
- **THEN** the group's combo reflects the new file policy without a plugin
  restart

### Requirement: Policy file affordance
The dock SHALL show the policy file path, an "Open file" action, and the
last reload time/status so the file's existence and authority are
discoverable.

#### Scenario: Open file
- **WHEN** the user clicks "Open file"
- **THEN** `allowlist.json` opens for editing in the editor or the OS
