# Spec Delta

## ADDED Requirements

### Requirement: Write-lock recovery
After a partial rollback locks writes, the system SHALL provide an explicit,
user-visible path to clear the lock without restarting the editor.

#### Scenario: Operator clears write lock
- **WHEN** writes are locked after a partial rollback and the operator invokes
  the unlock action
- **THEN** subsequent write-mode `execute` calls are permitted again and the
  unlock event is reported in diagnostics

## MODIFIED Requirements

### Requirement: Grouped reversible writes
All supported write operations within a single `execute` call SHALL be recorded
into one undoable editor action, registering prior values, hierarchy, ownership,
and references. Every write op the bridge exposes SHALL record a journaled
inverse; an op that cannot be undone SHALL be rejected at dispatch time rather
than applied unjournaled.

#### Scenario: Successful execution is undoable
- **WHEN** a write-mode `execute` call completes successfully
- **THEN** the editor exposes one Undo step that reverts all of that call's mutations, and Redo reapplies them

#### Scenario: Unjournaled op rejected
- **WHEN** code requests a write op for which no inverse is implemented
- **THEN** the op is rejected before mutating, instead of being applied without
  a rollback path
