# Spec Delta

## MODIFIED Requirements

### Requirement: Structured results and errors
`execute` SHALL return a structured result containing `ok` and `result`,
with `diagnostics`, `changes`, `effects`, and `truncated` present only when
non-empty or true. Changes SHALL be summarized as
`{counts: {"Class.op": n}, total, status}` and effects SHALL be summarized
as `{counts: {"op|name": n}, total}`, unless `verbose: true` restores the
ordered per-op logs. Errors SHALL carry a stable code, message, and
phase, and SHALL never expose native pointers or secrets.

#### Scenario: Successful execution shape
- **WHEN** an `execute` call completes
- **THEN** the response contains `ok: true`, the returned `result`, and the `changes` summary

#### Scenario: Error shape
- **WHEN** an `execute` call fails
- **THEN** the error includes a stable code, a message, and the phase in which it failed, with no pointers or secrets

#### Scenario: Compact success
- **WHEN** an execute call returns successfully with no writes or warnings
- **THEN** the result contains only `ok` and `result`

#### Scenario: Rollback is not misreported
- **WHEN** a transaction rolls back
- **THEN** `changes.status` is `rolled_back`, so attempted writes are never
  presented as persisted

#### Scenario: External effects stay visible
- **WHEN** an external action completed and later JavaScript fails
- **THEN** `effects` still reports the action and its terminal status

#### Scenario: Effects summarized by default
- **WHEN** an `execute` call applies many operations without `verbose`
- **THEN** `effects` is a per-op-kind count summary, not the ordered log

#### Scenario: Verbose detail
- **WHEN** `verbose: true` is passed
- **THEN** `changes` and `effects` carry the ordered per-operation logs
