# Spec Delta

## MODIFIED Requirements

### Requirement: Opaque session handles
Editor objects SHALL cross the bridge only as opaque handles bound to the service generation and the object's ObjectID. Handles SHALL be validated on resolution and against the declared destination class. Object handles and opaque value references SHALL be distinguishable and non-interchangeable; client-supplied class/type labels SHALL NOT establish identity or authorization.

#### Scenario: Stale handle rejected
- **WHEN** code uses a handle whose node has been deleted
- **THEN** the call returns INVALID_HANDLE without touching freed memory

#### Scenario: Cross-session handle rejected
- **WHEN** a handle minted in a different or expired service generation is used
- **THEN** the call is rejected as invalid even if its local numeric identifier has been reused

#### Scenario: Value used as an object
- **WHEN** godot.call or godot.get receives a PackedStringArray value reference as its object target
- **THEN** it fails with REFERENCE_KIND_MISMATCH rather than claiming that the object expired

#### Scenario: Incompatible object class
- **WHEN** a live object handle is supplied to an argument requiring an incompatible class
- **THEN** it fails with TYPE_MISMATCH before invoking the target method

### Requirement: Strict value conversion
JS/Variant conversion SHALL cover every concrete type in the declared codec inventory through a defined data representation or a validated opaque reference. It SHALL reject incompatible, ambiguous, malformed or unsupported encodings instead of coercing silently. Validation of all known argument types SHALL complete before assigning a property or invoking the current operation, equally for standalone and batch elements.

#### Scenario: Incompatible type rejected
- **WHEN** code passes a value with no defined Variant mapping
- **THEN** the call fails with a typed conversion error and does not report successful assignment

#### Scenario: Invalid property value has no effect
- **WHEN** a Vector3 property receives an object whose y component is a string
- **THEN** it fails with TYPE_MISMATCH identifying that component, leaves the property unchanged and records no completed effect for the rejected assignment

#### Scenario: All arguments checked before dispatch
- **WHEN** an operation has several arguments and a later one has a known incompatible type
- **THEN** the method is not invoked with the earlier arguments

#### Scenario: Batch uses the same validation
- **WHEN** a batch has a valid element, an invalid conversion and another valid sibling
- **THEN** the invalid element returns its own typed error without being dispatched, and the siblings follow the existing per-element batch contract

## ADDED Requirements

### Requirement: Complete Variant inventory
The codec SHALL classify all 39 concrete Variant types in the Godot 4.7 API base, excluding VARIANT_MAX. Each type SHALL declare its contextual input, explicit input, output, validation and opaque-only restrictions. A future unrecognized type SHALL be reported as unsupported or safely opaque, never silently treated as another known type.

#### Scenario: Every current Variant type is classified
- **WHEN** a client consults the codec inventory
- **THEN** it includes Nil, bool, int, float, String, Vector2, Vector2i, Rect2, Rect2i, Vector3, Vector3i, Transform2D, Vector4, Vector4i, Plane, Quaternion, AABB, Basis, Transform3D, Projection, Color, StringName, NodePath, RID, Object, Callable, Signal, Dictionary, Array, PackedByteArray, PackedInt32Array, PackedInt64Array, PackedFloat32Array, PackedFloat64Array, PackedStringArray, PackedVector2Array, PackedVector3Array, PackedColorArray and PackedVector4Array
- **AND** VARIANT_MAX is not advertised as a constructible value

### Requirement: Contextual conversion follows declared types
The bridge SHALL convert named data objects only using the declared destination type. Dictionary, Variant and untyped positions SHALL retain ordinary object/array semantics without inferring mathematical types. Explicit supported $t tags SHALL remain available, and contradictory explicit types SHALL be rejected. No contextual mathematical conversion SHALL accept positional component arrays.

#### Scenario: Known vector destination
- **WHEN** a Vector3 property or parameter receives {x:1,y:2,z:3}
- **THEN** it receives a Vector3 with those components

#### Scenario: Unknown or dictionary destination
- **WHEN** a Dictionary, Variant or untyped argument receives an untagged {x:1,y:2,z:3}
- **THEN** it remains a Dictionary rather than being inferred to be Vector3

#### Scenario: Explicit type without context
- **WHEN** a Variant argument receives {$t:"Vector3",x:1,y:2,z:3}
- **THEN** it receives a Vector3

#### Scenario: Positional vector rejected
- **WHEN** a Vector3 destination receives [1,2,3]
- **THEN** the bridge returns TYPE_MISMATCH with the accepted named shape

### Requirement: Named mathematical schemas
Mathematical inputs SHALL use exact named components, with required fields and no unknown fields. Supported explicit outputs SHALL include their $t type and the same component structure. Fields nested inside a known structure SHALL receive their declared context recursively. No missing component SHALL be invented except the documented Color alpha default.

#### Scenario: Vector shapes
- **WHEN** a destination expects Vector2/Vector2i, Vector3/Vector3i or Vector4/Vector4i
- **THEN** the accepted component names are respectively x/y, x/y/z or x/y/z/w, all required and numeric, with exact signed-32 components for integer vectors

#### Scenario: Rectangle and bounding box shapes
- **WHEN** the destination expects Rect2, Rect2i or AABB
- **THEN** it accepts position and size of type Vector2, Vector2i or Vector3 respectively, without normalizing signs

#### Scenario: Transform and matrix shapes
- **WHEN** the destination expects Transform2D, Basis, Transform3D or Projection
- **THEN** it accepts respectively {x:Vector2,y:Vector2,origin:Vector2}, {x:Vector3,y:Vector3,z:Vector3}, {basis:Basis,origin:Vector3} or {x:Vector4,y:Vector4,z:Vector4,w:Vector4}
- **AND** x/y/z/w identify columns, with no transposition, implicit identity, implicit origin or orthonormalization

#### Scenario: Plane and quaternion shapes
- **WHEN** the destination expects Plane or Quaternion
- **THEN** it accepts respectively {normal:Vector3,d:number} or {x:number,y:number,z:number,w:number}, without normalization or interpreting Euler angles

#### Scenario: Missing or misspelled component
- **WHEN** a Vector3 input omits z or supplies an additional zz field
- **THEN** it fails with a component path rather than defaulting or discarding the field

### Requirement: Color alpha and channel semantics
Color conversion SHALL require numeric finite r, g and b and accept optional numeric finite a defaulting to 1. It SHALL reject other shapes and SHALL NOT clamp channels or change color space. Output SHALL include all four channels. Representability limits of the destination component precision SHALL still apply.

#### Scenario: RGB shorthand
- **WHEN** a Color destination receives {r:1,g:0,b:0}
- **THEN** it receives alpha 1

#### Scenario: HDR and explicit alpha
- **WHEN** a Color destination receives {r:2,g:0.5,b:0,a:0.25}
- **THEN** the codec preserves those values without clamping r to 1

### Requirement: Rotation semantics are not inferred
The codec SHALL NOT infer units, rotation order, normalization, coordinate space or scale from vector component values. Context SHALL distinguish Vector4, Quaternion, Basis and Transform3D even when representations overlap. Native property semantics SHALL remain the responsibility of the selected Godot property or method.

#### Scenario: Degrees versus radians
- **WHEN** the same {x:0,y:90,z:0} is assigned to Node3D.rotation_degrees and Node3D.rotation
- **THEN** the codec constructs Vector3 for each without converting units; the properties interpret it as degrees and radians respectively

#### Scenario: Quaternion is not a Vector4 cast
- **WHEN** a Quaternion destination receives an explicit Vector4 tag
- **THEN** it rejects the incompatible explicit type rather than reinterpreting the four components

### Requirement: Numeric values preserve explicit precision contracts
Numeric conversion SHALL reject implicit strings, booleans, fractional integers and out-of-range values. int64 SHALL support safe integral Numbers and signed-64 BigInt or explicit decimal int tags. Packed floats SHALL honor destination precision without silent overflow. Non-finite scalar floats and negative zero SHALL have explicit lossless tags rather than being serialized as null.

#### Scenario: Int64 outside the safe Number range
- **WHEN** int64 receives an unsafe Number, or a valid {$t:"int",value:"9223372036854775807"}
- **THEN** the unsafe Number is rejected and the explicit maximum signed-64 value is preserved exactly

#### Scenario: Integer and float32 boundaries
- **WHEN** Vector3i receives 1.5, PackedByteArray receives 256 or a finite float64 overflows float32
- **THEN** conversion fails without truncation, wraparound, clamping or silent infinity

#### Scenario: Exceptional float round trip
- **WHEN** a scalar NaN, positive infinity, negative infinity or negative zero crosses the data wire
- **THEN** its explicit float tag preserves the exceptional value rather than losing it through JSON

#### Scenario: Non-finite mathematical output
- **WHEN** an engine mathematical value contains non-finite components that its normal structured format cannot carry
- **THEN** the bridge retains a correctly typed opaque value reference rather than substituting null components

### Requirement: Literal dictionaries and typed collections
Collections SHALL validate each element and known key/value type recursively before dispatch. Ordinary JSON objects SHALL represent ordinary string-key dictionaries; an explicit Dictionary entries envelope SHALL preserve non-string/reserved keys literally. Explicit collection descriptors SHALL NOT contradict the destination or trigger loading of scripts. Duplicate keys caused by conversion SHALL be rejected.

#### Scenario: Reserved dictionary keys are data
- **WHEN** a Dictionary entries envelope contains keys named $t, __godotHandle or __godotValue
- **THEN** the resulting Dictionary contains those literal keys and is not reinterpreted as a type or reference

#### Scenario: Numeric dictionary key
- **WHEN** a typed Dictionary requires integer keys
- **THEN** a JSON object key "1" is not silently converted, while an entries envelope with integer key 1 can be validated

#### Scenario: Every packed family has checked elements
- **WHEN** numeric, string, Vector2, Vector3, Vector4 or Color packed arrays are supplied as arrays or explicit Packed tags
- **THEN** each element follows the corresponding scalar/structure rules, including alpha defaults, int64 precision and integer ranges
- **AND** a failing element identifies its index and prevents dispatch of the operation

#### Scenario: Typed and empty arrays
- **WHEN** an empty Array has a known destination element type or an explicit compatible element_type descriptor
- **THEN** conversion preserves that restriction without inferring types from nonexistent elements

### Requirement: Passive conversion and reference-only capabilities
Conversion SHALL NOT execute user accessors, toJSON, constructors or methods to discover or decode a type. StringName and NodePath SHALL accept contextual strings and explicit tags without resolving paths. RID, Object, Callable and Signal SHALL require validated references or admissible null rather than fabricated JSON capabilities; existing class, member and callback authorizations SHALL still apply.

#### Scenario: No instance created by data conversion
- **WHEN** a value contains a class name, path or callback-shaped dictionary
- **THEN** the codec does not instantiate a node/resource/script, look up a node, create a RID or construct a callback

#### Scenario: No active metadata probing
- **WHEN** argument metadata is incomplete or a JavaScript object has accessor properties
- **THEN** the bridge does not load a script or run the accessors to deduce a conversion and instead preserves valid untyped data or reports an explicit encoding/type error

#### Scenario: String-like type identity
- **WHEN** StringName or NodePath is returned and later passed to a Variant position
- **THEN** its explicit value tag preserves its type without interpreting its content as an instruction to resolve an object

### Requirement: Opaque value reference lifecycle
Opaque value references SHALL identify retained values in the current service generation, remain usable between executions until release or service shutdown, and validate their actual server-side type. Wrong-kind, stale and unknown references SHALL have distinct diagnostics. Retention SHALL have declared count/memory quotas and SHALL NOT silently evict live references.

#### Scenario: Packed value survives another execution
- **WHEN** a PackedStringArray reference is returned and reused in a later execute within the same generation
- **THEN** it resolves to the original retained value unless explicitly released

#### Scenario: Forged type or raw RID
- **WHEN** a client changes a reference's type label or submits a raw numeric RID
- **THEN** it cannot gain a differently typed value or native capability

#### Scenario: Quota reached
- **WHEN** a new retained value would exceed the documented quota
- **THEN** the new retention fails with VALUE_REFERENCE_LIMIT while existing references remain valid

### Requirement: Bounded value inspection and release
The bridge SHALL provide asynchronous godot.values.inspect and godot.values.read for passive value metadata and bounded collection pages/structural fields in read mode. Small packed collections SHALL expose tagged inline values; large ones SHALL remain paginable references. Explicit release SHALL invalidate only the selected value reference and require authorized untracked mode because final reference cleanup can have effects.

#### Scenario: Small packed array is readable data
- **WHEN** a PackedStringArray fits the documented inline element and byte limits
- **THEN** the response contains its type and string values without requiring object-method calls

#### Scenario: Large collection page
- **WHEN** a collection exceeds inline limits and its value reference is read with valid offset and limit
- **THEN** a bounded ordered page is returned with length and continuation information, without expanding the whole collection

#### Scenario: Unsupported or active inspection
- **WHEN** read is requested for an opaque capability without safe structural expansion
- **THEN** it returns UNSUPPORTED_VALUE_OPERATION without invoking a getter, callback or signal, and the reference remains transferable

#### Scenario: Explicit reference release
- **WHEN** authorized untracked code releases a value reference
- **THEN** later uses fail with INVALID_VALUE_REFERENCE, no borrowed object is explicitly freed, and no rollback of final-reference cleanup is promised
