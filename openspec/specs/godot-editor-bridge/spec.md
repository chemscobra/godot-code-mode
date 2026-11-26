# godot-editor-bridge Specification

## Purpose

Provides the controlled bridge between JavaScript and the Godot editor: typed value conversion, opaque session-bound handles, main-thread dispatch of editor operations, and per-call permission enforcement.

## Requirements

### Requirement: Main-thread resolution
All object resolution and editor mutations requested from JS SHALL execute on the engine main thread via a queued dispatch with a per-frame budget; JS SHALL never access engine objects off-thread.

#### Scenario: Mutation dispatched to main thread
- **WHEN** JS code awaits a mutation such as `addChild`
- **THEN** the operation is performed on the main thread and its result returned to the awaiting promise

### Requirement: Opaque session handles
Editor objects SHALL cross the bridge only as opaque handles bound to the session and the object's ObjectID. Handles SHALL be validated on resolution.

#### Scenario: Stale handle rejected
- **WHEN** code uses a handle whose node has been deleted
- **THEN** the call returns `INVALID_HANDLE` without touching freed memory

#### Scenario: Cross-session handle rejected
- **WHEN** a handle minted in a different or expired session is used
- **THEN** the call is rejected as invalid

### Requirement: Strict value conversion
JS↔Variant conversion SHALL support scalars, Array, Dictionary, Vector2/3, Color, and tagged types, and SHALL reject incompatible or ambiguous values rather than coercing silently.

#### Scenario: Incompatible type rejected
- **WHEN** code passes a value with no defined Variant mapping
- **THEN** the call fails with a typed conversion error

### Requirement: Per-operation authorization
Every bridged operation SHALL be validated against the allowlist — class, method, and property — including operations invoked on objects returned by prior calls. Member authorization SHALL be inheritance-aware: a member allowlisted on a class is permitted on its subclasses, and `"*"` on a class covers members it inherits from base classes.

#### Scenario: Blocked API denied
- **WHEN** code attempts to reach OS, process, network, dynamic code loading, or arbitrary file access through any object
- **THEN** the operation is rejected and produces no side effects

#### Scenario: Authorization applies to returned objects
- **WHEN** code obtains a returned object and calls a non-allowlisted method on it
- **THEN** that call is rejected even though the object itself was legitimately obtained

#### Scenario: Inherited member permitted
- **WHEN** code sets a member declared on a base class (e.g. `albedo_color` on `StandardMaterial3D` via `BaseMaterial3D`) and that member is allowlisted on the base class
- **THEN** the operation is permitted without requiring a separate allowlist entry on the subclass

### Requirement: Batched operation dispatch
The bridge SHALL expose a `godot.batch` binding that applies a list of bridge operations in a single main-thread dispatch and resolves one promise with the ordered results. A failing element SHALL mark only that element's result; sibling elements SHALL still be applied unless the caller requests atomic mode.

#### Scenario: Batch applies in one dispatch
- **WHEN** code awaits `godot.batch` with N operations
- **THEN** all N are executed within one main-thread dispatch and one promise resolves with N ordered results

#### Scenario: Per-element error reporting
- **WHEN** one element of a batch fails authorization or execution
- **THEN** that element's result carries the error and the remaining elements still apply
