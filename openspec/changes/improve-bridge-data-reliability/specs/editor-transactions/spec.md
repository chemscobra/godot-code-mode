# Spec Delta

## MODIFIED Requirements

### Requirement: Grouped reversible writes
All supported operations in write mode SHALL be grouped into one undoable action with journaled prior values, hierarchy, ownership and references. A write-mode operation without a supported inverse SHALL be rejected before dispatch. Contextual conversion SHALL complete before recording a successful mutation. Explicitly authorized untracked operations SHALL remain outside this reversible contract and SHALL NOT be presented as undoable.

#### Scenario: Successful execution is undoable
- **WHEN** a write-mode execute call completes successfully
- **THEN** the editor exposes one Undo step that reverts that call's supported mutations, and Redo reapplies them

#### Scenario: Unjournaled op rejected
- **WHEN** write mode requests a mutation for which no inverse is implemented
- **THEN** the operation is rejected before mutating rather than applied without a rollback path

#### Scenario: Contextual value is journaled after validation
- **WHEN** a supported Vector3 property edit receives a valid named object in write mode
- **THEN** its validated engine value is assigned and its prior value is retained for Undo/Redo

#### Scenario: Invalid value is not a completed change
- **WHEN** a write-mode assignment fails conversion
- **THEN** no successful change entry or journaled forward mutation is recorded for that assignment

### Requirement: Rollback on failure
If write-mode execution fails or is cancelled, supported changes already applied in its journal SHALL be reverted, with partial rollback failures reported and writes locked pending review. An untracked failure SHALL NOT promise rollback: previously dispatched effects SHALL remain reported and partial SHALL identify potentially persistent effects. Failed conversion SHALL NOT be counted as a completed native effect.

#### Scenario: Partial work rolled back
- **WHEN** nodes were created in write mode and a subsequent operation in the same call fails
- **THEN** the supported mutations are reverted and no partial created nodes remain in the scene

#### Scenario: Rollback failure reported
- **WHEN** reverting applied changes itself fails
- **THEN** the system reports the partial state and blocks further writes pending review

#### Scenario: Untracked effects survive a later type error
- **WHEN** untracked code successfully mutates an object and a later assignment fails conversion
- **THEN** rollback_supported is false, the prior dispatched effect remains reported and partial is true
- **AND** the rejected assignment is not reported as a completed effect

## ADDED Requirements

### Requirement: Checkpoints are observations rather than scene state
Checkpoints SHALL remain explicit historical observations, separate from changes, effects and rollback results. Retaining checkpoints alone SHALL NOT set partial:true or create an Undo action. Rollback SHALL NOT rewrite a recorded checkpoint to pretend it described post-rollback state, and checkpoint references SHALL NOT guarantee that a recorded object remains alive.

#### Scenario: Observation retained after rollback
- **WHEN** write-mode code checkpoints a temporary position and later fails, causing successful rollback
- **THEN** the checkpoint retains its recorded value while transaction results report rollback of the scene state

#### Scenario: Read-only observation followed by error
- **WHEN** read code records a checkpoint and later throws without native mutations
- **THEN** the checkpoint is returned without claiming persistent partial scene effects or an Undo action

#### Scenario: Object deleted after checkpoint
- **WHEN** a checkpoint records an object handle and that object is subsequently deleted
- **THEN** the checkpoint remains a record of the reference, but later object access still fails normal handle validation
