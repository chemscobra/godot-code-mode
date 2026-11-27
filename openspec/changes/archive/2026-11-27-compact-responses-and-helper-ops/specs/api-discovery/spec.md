# Spec Delta

## ADDED Requirements

### Requirement: Tagged-value examples
`api.example(type)` SHALL return the exact wire literal accepted by
`godot.set`/`godot.call`/`godot.build` for tagged engine types, available in
both `search` and `execute` contexts.

#### Scenario: Transform3D example is executable
- **WHEN** code calls `api.example("Transform3D")` and passes the result to
  `godot.set(node, "transform", v)`
- **THEN** the engine receives a real `Transform3D` — no guessing at Basis
  layout

#### Scenario: Unknown type
- **WHEN** the type has no example
- **THEN** the result is `null`

### Requirement: Scene helper surface
`godot.build` SHALL construct a whole node tree in one call inside the
transaction, with automatic owner resolution; `godot.scene.new/reload`
SHALL wait for the real scene root instead of returning `"opening": true`;
`godot.scene.tree` SHALL inspect the hierarchy passively;
`godot.scene.run`/`stop` SHALL control playback via declared external
actions.

#### Scenario: Build with automatic owners
- **WHEN** code builds `{type,name,props,children}` with a parent in the
  edited scene
- **THEN** created nodes get the scene root as owner; a detached build owns
  its descendants under the built root

#### Scenario: Scene.new waits for completion
- **WHEN** code calls `godot.scene.new(path)` successfully
- **THEN** the promise resolves with the opened scene's root handle, or
  rejects with a typed timeout

#### Scenario: Console coverage stated honestly
- **WHEN** `scene.run` resolves
- **THEN** it reports `playing`, `scene`, and `console` — `unavailable`
  until a supported debugger channel exists, never implying errors were
  captured when they were not
