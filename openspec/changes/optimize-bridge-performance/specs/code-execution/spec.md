# Spec Delta

## ADDED Requirements

### Requirement: Opt-in execution cost observability
Development profiling SHALL provide bounded aggregate timings and counters for queue wait, catalog work, validation, native invocation, encoding and final response construction. It SHALL distinguish end-to-end latency from main-thread occupancy, including maintenance outside request dispatch, without adding MCP tools or public response fields.

#### Scenario: Profile a batch
- **WHEN** development profiling is explicitly enabled for a batch
- **THEN** the report identifies phase costs, catalog/reflection counts and continuous main-thread work separately from client-observed latency
- **AND** overlapping intervals are not represented as additive exclusive CPU time

#### Scenario: Profiling disabled
- **WHEN** the service runs without development profiling enabled
- **THEN** requests keep their normal 0.3.0 result shape without profiling output or additional data traversal solely for profiling

#### Scenario: Private data in a request
- **WHEN** submitted code, arguments or results contain private content
- **THEN** profiling does not record that content, credentials, project paths or native/object identifiers
- **AND** metrics remain bounded aggregates rather than an unbounded per-value trace

### Requirement: Comparable performance evidence
Performance comparisons SHALL use equivalent isolated workloads and record the baseline revision, environment, warmup, raw samples, failures, medians, p95 and profiling state. Primary acceptance SHALL compare against 0.3.0 with equivalent APIs. Cold startup, queue wait, execution and encoding SHALL remain distinguishable.

#### Scenario: Compare candidate and baseline
- **WHEN** an optimization is evaluated against 0.3.0
- **THEN** both variants use the same workload and permissions, with repeated warmed rounds and documented conditions
- **AND** principal latency comparisons are repeated with profiling disabled to expose instrumentation overhead separately

#### Scenario: Historical incompatible baseline
- **WHEN** 0.2.1 results are shown
- **THEN** its legacy batch-target workaround and lack of passive pagination are disclosed instead of presented as an equivalent API comparison

#### Scenario: Timeout or inconclusive variation
- **WHEN** a sample fails, reaches its execution budget or has variation that prevents a reliable conclusion
- **THEN** the report retains that outcome rather than omitting it or claiming a demonstrated speedup

#### Scenario: Frame responsiveness measurement
- **WHEN** editor responsiveness is evaluated
- **THEN** continuous bridge work on the main thread and rendered frame intervals are measured rather than inferred from HTTP duration alone

### Requirement: Serialization optimization preserves execution guarantees
Optimized result construction SHALL preserve the passive serialization, fresh-context isolation, precision, diagnostics, checkpoints, effects and bounded valid JSON behavior of 0.3.0. It SHALL NOT execute active formatters, skip validation or increase product limits as a performance shortcut.

#### Scenario: Removing an intermediate representation
- **WHEN** a result takes an optimized path without an intermediate JSON round trip
- **THEN** int64, special floats, Unicode, reserved keys, void and references retain their documented transport meaning

#### Scenario: Invalid or oversized result
- **WHEN** a result contains an accessor, nested promise or data exceeding its budget
- **THEN** the documented encoding error or omission behavior is preserved without executing user formatting code
- **AND** the primary diagnostic, complete prior checkpoints and known effects are not silently lost

#### Scenario: Consecutive executions
- **WHEN** one execution ends and another starts after any performance caches were populated
- **THEN** JavaScript globals and checkpoint state remain execution-local
