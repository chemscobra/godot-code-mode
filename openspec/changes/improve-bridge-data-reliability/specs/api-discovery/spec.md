# Spec Delta

## MODIFIED Requirements

### Requirement: Authorized-only metadata
search SHALL return class/member metadata permitted by the configured discovery policy and SHALL hide hard-denied entries. Discoverable native, extension and project-script metadata SHALL identify execution requirements without granting execution permission. Script metadata availability SHALL NOT require a per-plugin adapter, script loading or instantiation. Locked execution tiers SHALL NOT be mistaken for absence of discoverable metadata.

#### Scenario: Query a permitted class
- **WHEN** search queries api.describe("MeshInstance3D")
- **THEN** it returns permitted signatures and their declared type information

#### Scenario: Blocked members invisible
- **WHEN** search enumerates a class that has hard-blocked methods or properties
- **THEN** those members do not appear in the results

#### Scenario: Discovering a script is not permission to call it
- **WHEN** an installed project-script class is discoverable but untracked execution has not been authorized
- **THEN** its available metadata can be described while execution still requires its mode, class/member and tier checks

#### Scenario: Unloaded script remains passive
- **WHEN** an unloaded script has only class-level metadata
- **THEN** search reports the partial metadata status without loading, instantiating or executing it to fill in signatures

### Requirement: Type declarations
The system SHALL provide generated type declarations describing the permitted execute/search surface and codec representations, including named math structures, explicit tags, opaque object/value references, value inspection and checkpoints. Declarations SHALL distinguish synchronous checkpoint capture from asynchronous godot operations and SHALL NOT advertise opaque-only capability types as constructible from JSON.

#### Scenario: Declarations reflect allowlist
- **WHEN** type declarations are generated
- **THEN** they cover exactly the configured surface, include its value formats and exclude blocked members

#### Scenario: Opaque-only types and async contracts are clear
- **WHEN** a client reads declarations for RID, Callable, Signal, checkpoint and godot.values
- **THEN** references are distinguished from constructors and synchronous capture is distinguished from asynchronous bridge queries

## ADDED Requirements

### Requirement: Passive destination type descriptors
Discovery SHALL expose available Variant identifiers, declared object class, property/parameter/return types, collection element or key/value types, argument positions, defaults and varargs needed for conversion. Unavailable details SHALL be explicitly unknown rather than inferred by executing code. Type information SHALL remain separate from execution permissions and preserve the existing immutable snapshot contract.

#### Scenario: A typed third-party parameter
- **WHEN** an extension or cached script declares a Transform3D parameter
- **THEN** its signature exposes that type so the same contextual conversion can be used without plugin-specific code

#### Scenario: Typed dictionary metadata
- **WHEN** a declared collection provides key/value or element constraints
- **THEN** discovery preserves those constraints rather than publishing only an undifferentiated Dictionary or Array name

#### Scenario: Metadata is insufficient
- **WHEN** a parameter has type Variant, no annotation or unavailable collection details
- **THEN** discovery identifies the absence of a concrete context and does not claim that vector/element inference is available

### Requirement: Codec help is discoverable through existing tools
The search context SHALL provide api.types() and api.describeType(name) exposing supported types, accepted contextual/explicit shapes, output forms, validation rules, reference-only restrictions and documented limits. Help SHALL remain available through search/execute tool descriptions without adding another MCP tool or requiring access to README files, live objects or editor state.

#### Scenario: Agent asks how to pass a rotation
- **WHEN** the agent consults type help for Vector3, Quaternion, Basis and Transform3D
- **THEN** it can distinguish their component schemas and learns that vector conversion does not choose degrees/radians or normalize rotations

#### Scenario: Agent asks about a packed result
- **WHEN** the agent consults PackedStringArray help
- **THEN** it sees the tagged inline and paginable-reference forms, their limits and how they differ from an object handle

#### Scenario: Agent needs intermediate diagnostics
- **WHEN** the agent reads execution help
- **THEN** it learns the await contract, named-object conversion, explicit $t fallback, checkpoint capture limits and the meaning of effects versus checkpoints

#### Scenario: Unsupported or opaque-only type
- **WHEN** a type is not constructible from structured JSON
- **THEN** type help identifies its supported reference operations or explicit unsupported status instead of supplying a fictitious constructor
