# Spec Delta

## Purpose

Provides the controlled bridge between JavaScript and the Godot editor: typed value conversion, opaque session-bound handles, main-thread dispatch of editor operations, and per-call permission enforcement.

## ADDED Requirements

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
Every bridged operation SHALL be validated against the allowlist — class, method, and property — including operations invoked on objects returned by prior calls.

#### Scenario: Blocked API denied
- **WHEN** code attempts to reach OS, process, network, dynamic code loading, or arbitrary file access through any object
- **THEN** the operation is rejected and produces no side effects

#### Scenario: Authorization applies to returned objects
- **WHEN** code obtains a returned object and calls a non-allowlisted method on it
- **THEN** that call is rejected even though the object itself was legitimately obtained
