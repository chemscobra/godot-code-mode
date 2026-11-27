# Spec Delta

## ADDED Requirements

### Requirement: Incremental bounded page construction
Value-page sizing SHALL avoid repeatedly encoding the entire accumulated prefix. Its encoding work SHALL scale with the encoded elements visited and final envelope. Pages SHALL preserve the 0.3.0 range, ordering, continuation, reference and exact UTF-8 output-limit contracts.

#### Scenario: Increasing page size
- **WHEN** pages of 16, 64 and 256 similarly sized elements are constructed
- **THEN** development counters show element encoding and final-envelope work rather than the sum of all encoded prefixes
- **AND** the returned page remains within its 16 KiB limit

#### Scenario: Escaped or multibyte data near a boundary
- **WHEN** a page contains quotes, backslashes, control characters or multibyte Unicode near its byte limit
- **THEN** the actual encoded JSON fits the limit without splitting an encoding or silently losing data
- **AND** count and next_offset describe only the included complete elements

#### Scenario: An attempted element does not fit
- **WHEN** encoding the next element tentatively retains references but the element cannot be included
- **THEN** only those tentative new retentions are rolled back
- **AND** existing references and already included elements remain valid

#### Scenario: Deep dictionary offset
- **WHEN** a dictionary page starts at a large offset
- **THEN** performance reporting distinguishes the cost of reaching that offset from page encoding
- **AND** the system does not claim offset-independent total cost or introduce an undocumented cursor

### Requirement: Reusable descriptors do not retain authority
Each operation SHALL validate current object existence, reference generation, class compatibility and authorization even when its descriptor is reused. Cached metadata SHALL NOT turn an earlier grant or successful conversion into authorization for a later operation.

#### Scenario: Permission revoked after metadata was cached
- **WHEN** a tier or policy is changed after a descriptor was used successfully
- **THEN** the next affected operation follows the current permission state rather than the cached grant

#### Scenario: Deleted object or expired value reference
- **WHEN** an object is deleted, a value reference is released, or the service generation changes
- **THEN** later access fails normal handle or value-reference validation even if a matching descriptor remains cached

#### Scenario: Mutable input changes
- **WHEN** a second operation reuses a type descriptor but supplies a different or mutated input value
- **THEN** that input is validated independently and an invalid value is rejected before dispatch

### Requirement: Optimization preserves dispatch and transaction semantics
Optimization SHALL preserve the single-dispatch ordered batch behavior, per-element errors and sibling continuation of 0.3.0. Engine-object access SHALL remain on the main thread. Write journaling, rollback, untracked effects and operation budgets SHALL NOT be weakened to obtain a faster benchmark.

#### Scenario: Mixed batch with an invalid element
- **WHEN** valid and invalid elements are submitted together
- **THEN** their order, independent validation and per-element results are preserved in one main-thread dispatch

#### Scenario: Failure after a reversible mutation
- **WHEN** a write operation succeeds and a later conversion fails
- **THEN** supported changes roll back and any retained checkpoint remains an independent historical observation

#### Scenario: Non-reversible or long-running native call
- **WHEN** an authorized untracked native call has effects or runs beyond a cooperative dispatch budget
- **THEN** its effects and duration are reported honestly without pretending it was preempted, rolled back or silently moved to another thread
