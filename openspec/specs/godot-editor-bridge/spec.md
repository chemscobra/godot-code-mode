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
JS↔Variant conversion SHALL support scalars, Array, Dictionary, Vector2/3,
Color, tagged types, and every packed array type (flattened to plain
arrays), and SHALL reject incompatible or ambiguous values rather than
coercing silently. Tagged types SHALL include Basis, Transform3D,
Quaternion, Vector4/i, Vector2i/3i, Rect2, AABB, Plane, NodePath, and
StringName.

#### Scenario: Incompatible type rejected
- **WHEN** code passes a value with no defined Variant mapping
- **THEN** the call fails with a typed conversion error

#### Scenario: Tagged Transform3D
- **WHEN** code passes `{$t:"Transform3D", basis, origin}` to a write
- **THEN** the engine receives a real `Transform3D` and a subsequent read
  returns the applied origin

#### Scenario: Packed array result
- **WHEN** a call returns `PackedStringArray` (e.g. `get_open_scenes`)
- **THEN** JS receives a real array, not a stringified value

### Requirement: Per-operation authorization
Every bridged operation SHALL be validated against the allowlist — class,
method, and property — including operations invoked on objects returned by
prior calls. Member authorization SHALL be inheritance-aware: a member
allowlisted on a class is permitted on its subclasses, and `"*"` on a class
covers members it inherits from base classes.

#### Scenario: Blocked API denied
- **WHEN** code attempts to reach OS, process, network, dynamic code loading, or arbitrary file access through any object
- **THEN** the operation is rejected and produces no side effects

#### Scenario: Authorization applies to returned objects
- **WHEN** code obtains a returned object and calls a non-allowlisted method on it
- **THEN** that call is rejected even though the object itself was legitimately obtained

#### Scenario: Inherited member permitted
- **WHEN** code sets a member declared on a base class (e.g. `albedo_color` on `StandardMaterial3D` via `BaseMaterial3D`) and that member is allowlisted on the base class
- **THEN** the operation is permitted without requiring a separate allowlist entry on the subclass

#### Scenario: Setter spelling equivalence
- **WHEN** code calls `set_material_override(m)` or sets `material_override`
  directly with a valid `StandardMaterial3D`
- **THEN** both spellings are authorized through the journaled `set` path

#### Scenario: Widened editor inspection
- **WHEN** code calls `get_open_scenes` or `get_resource_filesystem` on the
  `EditorInterface` singleton
- **THEN** the call is authorized; the returned object's own tier still gates
  its members

### Requirement: Batched operation dispatch
The bridge SHALL expose a `godot.batch` binding that applies a list of bridge operations in a single main-thread dispatch and resolves one promise with the ordered results. A failing element SHALL mark only that element's result; sibling elements SHALL still be applied unless the caller requests atomic mode.

#### Scenario: Batch applies in one dispatch
- **WHEN** code awaits `godot.batch` with N operations
- **THEN** all N are executed within one main-thread dispatch and one promise resolves with N ordered results

#### Scenario: Per-element error reporting
- **WHEN** one element of a batch fails authorization or execution
- **THEN** that element's result carries the error and the remaining elements still apply

### Requirement: Passive scene tree inspection
The bridge SHALL expose a read op returning the scene hierarchy with node
names, native classes, and script resource paths — bounded by `depth` and
`maxNodes`, reporting truncation explicitly, and never loading scripts.

#### Scenario: Tree with script paths
- **WHEN** code requests the tree of an open scene
- **THEN** each entry carries `name`, `class`, optional `script` path, and
  `children`, within the requested bounds

#### Scenario: Bounded walk
- **WHEN** the tree exceeds depth or node limits
- **THEN** the result reports `truncated: true` instead of silently cutting

### Requirement: Const-correct read/write classification
Method calls SHALL be classified by the `const` flag in the API index
(`is_const` from the dump or `flags & METHOD_FLAG_CONST` at runtime), not by
name prefixes. An indexed-const call SHALL execute in read mode; non-const
calls SHALL still require write mode.

#### Scenario: Const call in read mode
- **WHEN** code calls an indexed-const method without a `get_/is_` prefix
  (e.g. `can_process`) in read mode
- **THEN** the call executes normally

#### Scenario: Non-const call still gated
- **WHEN** code calls a non-const method in read mode
- **THEN** the op is rejected with `PERMISSION`

#### Scenario: Const calls are not reported as changes
- **WHEN** a const call is downgraded
- **THEN** it does not appear in `changes` counts or the verbose log

### Requirement: Permission introspection
The bridge SHALL expose `godot.permissions(query?)`: with no query a
tier/policy/denial summary; with `{class, member}` an explanation
`{allowed, reason, tier?, policy?, unlock?}`. It SHALL be passive — it never
grants permissions or executes code.

#### Scenario: Summary without query
- **WHEN** code calls `godot.permissions()`
- **THEN** it receives tier names, policies, unlock state, class examples,
  and the denied class/member lists

#### Scenario: Member explanation
- **WHEN** code queries a denied member (e.g. `EditorInterface` outside the
  restrict list, `Node.rpc`, `Object.call`)
- **THEN** the response names the cause: `restricted_class`,
  `member_denied`, `meta_method_denied`, `class_denied`, `tier_locked`, or
  `member_not_indexed`

### Requirement: Native collection results
Inside `execute`, returned `Array` values SHALL arrive as real JS arrays
and returned `Dictionary` values with all-string keys SHALL arrive as
plain JS objects. Packed collections, typed dictionaries, dictionaries
with non-string or reserved keys, and opaque value references SHALL keep
their tagged `{$t:...}` form.

#### Scenario: get_children is iterable
- **WHEN** a bridged call returns `Array` (typed or untyped, e.g.
  `get_children()`)
- **THEN** the awaited result is a JS array usable with `.map`/`.length`
- **AND** it can be passed back as a call argument without manual tagging

#### Scenario: Typed dictionary or non-string keys keep tag
- **WHEN** a bridged call returns a dictionary whose keys are not plain
  strings (or collide with reserved envelopes)
- **THEN** the `{$t:"Dictionary",entries:[...]}` form is preserved so key
  type information is not lost
