# Spec Delta

## ADDED Requirements

### Requirement: Stable metadata reuse
Repeated operations against unchanged metadata SHALL reuse the current catalog rather than perform project-wide rediscovery for each element. Reuse SHALL preserve passive discovery and the declared type descriptors, defaults and partial-metadata status available in 0.3.0.

#### Scenario: Stable native read batch
- **WHEN** a batch repeatedly reads known native properties without changing classes, scripts or policy
- **THEN** development counters show no project-wide rediscovery or reflection of every cached script per element
- **AND** all element results retain their normal authorization and type information

#### Scenario: Idle catalog maintenance
- **WHEN** the editor is idle with an unchanged project
- **THEN** catalog maintenance reuses valid normalized metadata instead of publishing identical full catalogs on every check
- **AND** maintenance cost is distinguishable from request-triggered work in development measurements

### Requirement: Cache freshness before reuse
Metadata reuse SHALL be invalidated when a relevant script identity, signature, extension or authorization generation changes. If freshness cannot be established, the system SHALL check the affected metadata before depending on it, without loading or instantiating scripts merely for discovery.

#### Scenario: A cached script becomes different
- **WHEN** a script is reloaded with a changed parameter type or an object receives a different script
- **THEN** the next operation depending on that metadata does not use the old signature to validate its arguments

#### Scenario: Partial metadata becomes available
- **WHEN** an authorized load makes an initially uncached script available
- **THEN** a later request can discover its cached signatures without a discovery-only load or instance construction

#### Scenario: Extension and script membership changes
- **WHEN** an extension reloads or a script class is added or removed
- **THEN** affected metadata is reconciled before it is relied on for execution
- **AND** subsequent catalog requests reflect the reconciled state

#### Scenario: Change inside a batch
- **WHEN** an authorized earlier element changes the script or class metadata required by a later element
- **THEN** the later element obtains current metadata instead of assuming the whole batch shares an unchanged catalog

### Requirement: Immutable publication across generations
A request SHALL retain one consistent metadata snapshot for its api queries. Publishing a newer generation SHALL NOT mutate that snapshot or grant execution through it. Obsolete cache generations SHALL remain retained only while actively needed, not accumulate as permanent history.

#### Scenario: Catalog changes during a query
- **WHEN** a new catalog is published while a request already holds an older snapshot
- **THEN** its api.classes, api.find and api.describe queries remain internally consistent
- **AND** a later request can use the new snapshot

#### Scenario: Repeated invalidation cycles
- **WHEN** metadata is changed repeatedly and requests holding old snapshots finish
- **THEN** measurements show obsolete cache generations becoming releasable rather than growing without bound
