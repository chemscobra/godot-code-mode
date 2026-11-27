# Spec Delta

## MODIFIED Requirements

### Requirement: Grouped reversible writes
All supported write operations within a single `execute` call SHALL be recorded into one undoable editor action, registering prior values, hierarchy, ownership, and references. Every write op the bridge exposes SHALL record a journaled inverse; an op that cannot be undone SHALL be rejected at dispatch time rather than applied unjournaled.

#### Scenario: Successful execution is undoable
- **WHEN** a write-mode `execute` call completes successfully
- **THEN** the editor exposes one Undo step that reverts all of that call's mutations, and Redo reapplies them

#### Scenario: Unjournaled op rejected
- **WHEN** code requests a write op for which no inverse is implemented
- **THEN** the op is rejected before mutating, instead of being applied without a rollback path

## ADDED Requirements

### Requirement: Trusted script attachment
A trusted plugin operation `editor.attach_script` SHALL assign a non-@tool `.gd` script to a node undoably, gated by the filesystem tier. The op SHALL validate a canonical `res://` path, file existence, `Script` type, and base-type compatibility before mutating, and SHALL capture the prior script for restore. `Script` classes and meta-methods SHALL remain denied.

#### Scenario: Attach and roll back
- **WHEN** `godot.attachScript(node, path)` applies and the transaction fails
- **THEN** the node's prior script (or none) is restored

#### Scenario: Unsafe scripts refused
- **WHEN** the script is `@tool`, missing, non-canonical, or has an incompatible base type
- **THEN** the op fails before any mutation
