# api-discovery Specification

## Purpose

Provides progressive API discovery through the `search` tool: read-only JavaScript executed against an immutable index of authorized API metadata, returning bounded results and type signatures.

## Requirements

### Requirement: Authorized-only metadata
`search` SHALL return only metadata for classes, methods, and properties on the allowlist. Blocked members and third-party script classes without explicit authorization SHALL NOT appear.

#### Scenario: Query a permitted class
- **WHEN** `search` code queries `api.describe("MeshInstance3D")`
- **THEN** it returns the permitted signatures for that class

#### Scenario: Blocked members invisible
- **WHEN** `search` code enumerates a class that has blocked methods or properties
- **THEN** those members do not appear in the results

### Requirement: Read-only index
The metadata index SHALL be immutable during a search, and search code SHALL have no access to live editor objects, handles, or the editor state.

#### Scenario: Search cannot reach live objects
- **WHEN** search code attempts to obtain an object handle or editor state
- **THEN** the attempt fails because no such binding exists in the search context

### Requirement: Bounded output
`search` results SHALL be subject to an output size limit and SHALL report truncation.

#### Scenario: Large result truncated
- **WHEN** a query's serialized result exceeds the output budget
- **THEN** the response returns a bounded prefix and marks `truncated` as true

### Requirement: Type declarations
The system SHALL provide generated type declarations describing the authorized API surface exposed to `execute` and `search` code.

#### Scenario: Declarations reflect allowlist
- **WHEN** type declarations are generated
- **THEN** they cover exactly the authorized surface and no blocked members

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
