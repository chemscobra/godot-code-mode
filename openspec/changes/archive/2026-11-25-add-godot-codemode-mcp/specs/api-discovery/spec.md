# Spec Delta

## Purpose

Provides progressive API discovery through the `search` tool: read-only JavaScript executed against an immutable index of authorized API metadata, returning bounded results and type signatures.

## ADDED Requirements

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
