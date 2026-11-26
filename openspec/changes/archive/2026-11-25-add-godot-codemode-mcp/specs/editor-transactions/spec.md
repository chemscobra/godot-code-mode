# Spec Delta

## Purpose

Provides reversible editor writes: supported mutations inside one `execute` call are grouped into a single undoable action, rolled back on failure, with scene saving kept as a separate explicit operation.

## ADDED Requirements

### Requirement: Grouped reversible writes
All supported write operations within a single `execute` call SHALL be recorded into one undoable editor action, registering prior values, hierarchy, ownership, and references.

#### Scenario: Successful execution is undoable
- **WHEN** a write-mode `execute` call completes successfully
- **THEN** the editor exposes one Undo step that reverts all of that call's mutations, and Redo reapplies them

### Requirement: Rollback on failure
If an operation fails mid-execution or the call is cancelled, the system SHALL revert the supported changes already applied in that call, leaving no partial mutations.

#### Scenario: Partial work rolled back
- **WHEN** nodes were created and a subsequent operation in the same call fails
- **THEN** no partial nodes remain in the scene

#### Scenario: Rollback failure reported
- **WHEN** reverting applied changes itself fails
- **THEN** the system reports the partial state and blocks further writes pending review

### Requirement: Explicit save
Saving the scene SHALL occur only through a separate explicitly authorized action, outside the reversible transaction; `execute` SHALL NOT save implicitly.

#### Scenario: No implicit save
- **WHEN** a write-mode `execute` call completes
- **THEN** the scene file on disk is unchanged unless a separate authorized save action was invoked

### Requirement: External change awareness
The system SHALL detect scene changes made outside the execution (e.g. by the user in the editor) that conflict with an in-flight transaction.

#### Scenario: External modification detected
- **WHEN** the scene is modified externally while a write transaction is in flight
- **THEN** the conflict is detected and reported rather than silently overwriting
