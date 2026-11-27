# Spec Delta

## ADDED Requirements

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
