# Spec Delta

## ADDED Requirements

### Requirement: Permission-surface census
A single `execute` call SHALL classify every indexed class member into a
declared bucket — `allowed-read`, `allowed-write`, `tier-locked`, `denied`
(with a `reason`), or `no-undo` — by consulting permission introspection.
Every member SHALL land in exactly one bucket; the unexplained bucket SHALL
be empty.

#### Scenario: Full census is explainable
- **WHEN** the census sweeps all indexed members
- **THEN** each member reports one declared bucket and no member is
  unexplained

#### Scenario: Denials carry reasons
- **WHEN** the census classifies a member as `denied` or `tier-locked`
- **THEN** the entry includes the reason (`class_denied`, `member_denied`,
  `meta_method_denied`, `restricted_class`, `tier_locked`) and, for tiers,
  the unlock path

### Requirement: Census snapshot regression
The census result SHALL be emitted as JSON and compared against a versioned
expectation file; any member changing bucket without an updated snapshot
SHALL fail verification.

#### Scenario: Unexpected bucket change fails
- **WHEN** an allowlist or engine update moves a member between buckets and
  the snapshot was not refreshed
- **THEN** verification fails and reports the differing members

#### Scenario: Intentional change passes after refresh
- **WHEN** the snapshot is regenerated to reflect an intended change
- **THEN** verification passes

### Requirement: Execution sampling matches classification
A deterministic sample of const-declared members SHALL execute in read
mode, and every journaled inverse SHALL be exercised in write mode
followed by a forced rollback that restores prior state.

#### Scenario: Const sample runs in read mode
- **WHEN** the sampled const methods are invoked in read mode
- **THEN** none return `PERMISSION`

#### Scenario: Journaled inverses restore state
- **WHEN** each journaled op applies and the transaction is then failed
- **THEN** the object's prior state is restored and undo history gains one
  step on success

### Requirement: Multi-genre dogfood scenarios
Verification SHALL include per-genre scenarios (at minimum: 2D platformer
with TileMap, UI/Control with theme resources, signal wiring) that drive
the full workflow and report a per-scenario friction invoice: calls
required, blocks encountered with their `reason`, and response bytes.

#### Scenario: Expected denials are explained
- **WHEN** a scenario hits a denylisted or tier-gated member
- **THEN** the invoice records the block with a correct `reason` and the
  test treats it as expected coverage, not a failure

#### Scenario: Unjournaled mutators surface as gaps
- **WHEN** a scenario hits `NO_UNDO` on a common mutator (e.g.
  `set_cell`, `connect`)
- **THEN** the invoice reports it as a gap for follow-up prioritization

#### Scenario: Compact-response budget holds
- **WHEN** a scenario performs many writes without `verbose`
- **THEN** the summed response bytes stay under the scenario's declared
  budget

### Requirement: MCP contract audit
Verification SHALL audit the tool contract: valid `tools/list` input
schemas, declared tool annotations (`readOnlyHint`, `destructiveHint`),
`structuredContent` parity with text `content`, stable domain error codes
mapped to correct JSON-RPC errors, and rejection of cross-session handles.

#### Scenario: Tool list is well-formed
- **WHEN** a client calls `tools/list`
- **THEN** every tool has a valid input schema and annotation presence is
  reported

#### Scenario: Errors keep domain codes
- **WHEN** a call is denied (`PERMISSION`, `NO_UNDO`, `INVALID_HANDLE`)
- **THEN** the JSON-RPC error carries the stable domain code, not a generic
  internal error

#### Scenario: Cross-session handle rejected
- **WHEN** a handle minted in another session is used
- **THEN** it is rejected as invalid

### Requirement: Independent MCP client check
Verification SHALL exercise the server through an official MCP SDK client
that performs its own handshake, `tools/list`, and `tools/call`, covering
transport behavior the in-repo harness may share with the server.

#### Scenario: External client completes a workflow
- **WHEN** the SDK client initializes, lists tools, and executes a write
  transaction
- **THEN** results match the in-repo harness for the same calls
