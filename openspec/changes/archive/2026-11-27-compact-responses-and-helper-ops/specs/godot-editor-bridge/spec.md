# Spec Delta

## ADDED Requirements

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

## MODIFIED Requirements

### Requirement: Strict value conversion
JS↔Variant conversion SHALL support scalars, Array, Dictionary, Vector2/3,
Color, and tagged types, and SHALL reject incompatible or ambiguous values
rather than coercing silently. Tagged types SHALL include Basis,
Transform3D, Quaternion, Vector4/i, Vector2i/3i, Rect2, AABB, Plane,
NodePath, and StringName.

#### Scenario: Incompatible type rejected
- **WHEN** code passes a value with no defined Variant mapping
- **THEN** the call fails with a typed conversion error

#### Scenario: Tagged Transform3D
- **WHEN** code passes `{$t:"Transform3D", basis, origin}` to a write
- **THEN** the engine receives a real `Transform3D` and a subsequent read
  returns the applied origin
