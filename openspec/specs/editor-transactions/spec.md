# editor-transactions Specification

## Purpose

Provides reversible editor writes: supported mutations inside one `execute` call are grouped into a single undoable action, rolled back on failure, with scene saving kept as a separate explicit operation.

## Requirements

### Requirement: Grouped reversible writes
All supported write operations within a single `execute` call SHALL be recorded into one undoable editor action, registering prior values, hierarchy, ownership, and references. Every write op the bridge exposes SHALL record a journaled inverse; an op that cannot be undone SHALL be rejected at dispatch time rather than applied unjournaled.

#### Scenario: Successful execution is undoable
- **WHEN** a write-mode `execute` call completes successfully
- **THEN** the editor exposes one Undo step that reverts all of that call's mutations, and Redo reapplies them

#### Scenario: Unjournaled op rejected
- **WHEN** code requests a write op for which no inverse is implemented
- **THEN** the op is rejected before mutating, instead of being applied without a rollback path

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

### Requirement: Write-lock recovery
After a partial rollback locks writes, the system SHALL provide an explicit, user-visible path to clear the lock without restarting the editor.

#### Scenario: Operator clears write lock
- **WHEN** writes are locked after a partial rollback and the operator invokes the unlock action
- **THEN** subsequent write-mode `execute` calls are permitted again and the unlock event is reported in diagnostics

### Requirement: Trusted script attachment
A trusted plugin operation `editor.attach_script` SHALL assign a non-@tool `.gd` script to a node undoably, gated by the filesystem tier. The op SHALL validate a canonical `res://` path, file existence, `Script` type, and base-type compatibility before mutating, and SHALL capture the prior script for restore. `Script` classes and meta-methods SHALL remain denied.

#### Scenario: Attach and roll back
- **WHEN** `godot.attachScript(node, path)` applies and the transaction fails
- **THEN** the node's prior script (or none) is restored

#### Scenario: Unsafe scripts refused
- **WHEN** the script is `@tool`, missing, non-canonical, or has an incompatible base type
- **THEN** the op fails before any mutation

### Requirement: Extended journaled inverse set
The journaled set SHALL include transform mutators (inverted by restoring
`global_transform`), `show`/`hide` (via the `visible` property),
`add_to_group`/`remove_from_group` (inverse pair), `move_child` (prior
index), `add_sibling` (`remove_child` on the captured parent), and
`grab_focus`/`release_focus` (inverse pair). `set_x` calls SHALL route to
the journaled `set` path when the property is authorized.

#### Scenario: Transform mutator rolls back
- **WHEN** code calls `translate`/`rotate`/`scale_object_local` and the transaction later fails
- **THEN** `global_transform` is restored to the captured prior value

#### Scenario: Group and sibling calls invert
- **WHEN** `add_to_group` or `add_sibling` applies and the transaction fails
- **THEN** `remove_from_group` or `remove_child` on the captured parent restores the prior structure

### Requirement: Trusted script creation
A trusted plugin operation `editor.create_script` SHALL write a `.gd`
script file undoably, gated by the filesystem tier, so scene+script
authoring works entirely over the bridge while `Script`/`GDScript` classes
stay denied. The op SHALL validate a canonical `res://` `.gd` path and
SHALL refuse `@tool` sources and overwriting existing files unless
explicitly confirmed. Rollback SHALL restore the prior file contents or
delete a file the op created.

#### Scenario: Create and attach in one flow
- **WHEN** code calls `godot.createScript(path, source)` with a valid
  non-`@tool` source and then attaches it to a node
- **THEN** the file exists on disk, is visible to the editor filesystem,
  and attaches successfully

#### Scenario: Created file rolls back
- **WHEN** the transaction fails after `editor.create_script` created the
  file
- **THEN** the created file is removed and any prior file at the path is
  restored byte-for-byte

#### Scenario: Unsafe creations refused
- **WHEN** the path is non-canonical or outside `res://`, the source
  declares `@tool`, or the file exists and no overwrite was confirmed
- **THEN** the op fails without writing anything
