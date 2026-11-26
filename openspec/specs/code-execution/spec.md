# code-execution Specification

## Purpose

Provides bounded JavaScript execution through the `execute` tool: an async function runs in an isolated context with time, memory, operation, and output budgets, and returns structured results or structured errors.

## Requirements

### Requirement: Bounded execution
Each `execute` call SHALL enforce limits on total time, JS heap, bridge operation count, and output size. Initial budgets are tunable design constants.

#### Scenario: Infinite loop interrupted
- **WHEN** `execute` runs `while(true) {}`
- **THEN** the JS execution is interrupted and the editor remains responsive

#### Scenario: Budget exceeded reported
- **WHEN** an execution exceeds its heap, operation, or output budget
- **THEN** it terminates with a structured error identifying which budget was hit

### Requirement: Structured results and errors
`execute` SHALL return a structured result containing `ok`, `result`, `diagnostics`, `changes`, and `truncated`. Errors SHALL carry a stable code, message, and phase, and SHALL never expose native pointers or secrets.

#### Scenario: Successful execution shape
- **WHEN** an `execute` call completes
- **THEN** the response contains `ok: true`, the returned `result`, and the `changes` summary

#### Scenario: Error shape
- **WHEN** an `execute` call fails
- **THEN** the error includes a stable code, a message, and the phase in which it failed, with no pointers or secrets

### Requirement: Fresh context per execution
Each `execute` call SHALL run in a new JS context with no shared mutable state carried over from prior executions and no network, filesystem, or native-module bindings.

#### Scenario: State does not leak
- **WHEN** a second `execute` call reads a global set by a previous call
- **THEN** the value is absent because the context is fresh

### Requirement: Read/write modes
`execute` SHALL accept `mode: read | write` (default `read`). Write mode gates mutation capability but SHALL NOT bypass per-operation permission validation.

#### Scenario: Write attempted in read mode
- **WHEN** code attempts a mutation while running in `read` mode
- **THEN** the operation is rejected with a permission error and no side effects occur
