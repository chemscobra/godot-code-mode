# Spec Delta

## ADDED Requirements

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

## MODIFIED Requirements

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
