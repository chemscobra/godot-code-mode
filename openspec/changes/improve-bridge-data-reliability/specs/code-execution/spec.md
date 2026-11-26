# Spec Delta

## MODIFIED Requirements

### Requirement: Structured results and errors
execute SHALL return ok, result, diagnostics, changes and truncated, plus mode, rollback_supported, effects, partial and checkpoints. Errors SHALL preserve a stable original code, message and phase without exposing native pointers, credentials or automatic argument dumps. Available operation/type/data-path/source context SHALL accompany errors; absence of such context SHALL NOT be replaced by invented values.

#### Scenario: Successful execution shape
- **WHEN** an execute call completes
- **THEN** the response contains ok:true, its returned result and changes summary, together with the mode, effect and checkpoint fields

#### Scenario: Error shape
- **WHEN** an execute call fails
- **THEN** the error includes its stable original code, message and phase without pointers or credentials, while result is null if no final result was produced

#### Scenario: Bridge error survives a rejected promise
- **WHEN** a bridge operation rejects with TYPE_MISMATCH, PERMISSION or INVALID_HANDLE
- **THEN** diagnostics retains that code instead of replacing it with JS_REJECTED

#### Scenario: Type mismatch context
- **WHEN** a nested component or argument fails conversion
- **THEN** diagnostics identifies the available class, member, argument index, data path, expected and received types, and batch index if applicable
- **AND** it does not include full argument values by default

#### Scenario: Void remains a valid result
- **WHEN** an otherwise successful execution returns undefined because its final operation is void
- **THEN** the response has ok:true and result:null rather than a serialization failure

### Requirement: Read/write modes
execute SHALL accept read, write and untracked, defaulting to read. read SHALL reject mutations; write SHALL require journaled supported effects; untracked SHALL require explicit authorization and SHALL NOT promise undo or rollback. Every mode SHALL retain class/member/tier validation and the same data conversion contract. Choosing a mode SHALL NOT weaken value validation or authorize discovered metadata.

#### Scenario: Write attempted in read mode
- **WHEN** code attempts a mutation while running in read mode
- **THEN** the operation is rejected with a permission error and no side effects of that rejected operation occur

#### Scenario: Unknown inverse in write
- **WHEN** write code calls a mutating method without a supported inverse
- **THEN** it is rejected before dispatch rather than silently changed into untracked

#### Scenario: Authorized generic execution
- **WHEN** a caller explicitly grants the untracked tier and invokes an otherwise authorized plugin method in untracked
- **THEN** the method can run without a plugin-specific adapter, with rollback_supported:false and honest reporting of dispatched effects

## ADDED Requirements

### Requirement: Source locations refer to submitted code
Diagnostics SHALL include the JavaScript source line, column and bounded stack when the runtime makes them available. Locations SHALL refer to user-submitted code rather than the internal async wrapper. A location unavailable from the runtime SHALL be omitted or marked unavailable. Error extraction SHALL NOT execute user-defined accessors or formatting code.

#### Scenario: Wrapper-adjusted exception
- **WHEN** a JavaScript exception occurs on line 4 of submitted code
- **THEN** the diagnostic reports that user line when available rather than the wrapper-shifted line

#### Scenario: Missing native source location
- **WHEN** a native invocation error provides no source column
- **THEN** the diagnostic preserves its native code/context without inventing a column or native stack

#### Scenario: Thrown object with active formatting
- **WHEN** a thrown object defines accessors or a custom toString
- **THEN** collecting diagnostics does not execute them or dispatch additional bridge operations

#### Scenario: Intrinsic stack hooks are reserved by the worker
- **WHEN** submitted code attempts to replace intrinsic Error.prepareStackTrace or Error.stackTraceLimit
- **THEN** these non-writable, non-configurable properties remain undefined and 32 respectively
- **AND** automatic stack construction and diagnostic extraction do not invoke user-provided formatting or limit-coercion functions
- **AND** this worker-local restriction does not change bridge authorization or project files

### Requirement: Explicit execution checkpoints
execute SHALL provide synchronous checkpoint(name,value), capturing an ordered bounded snapshot of explicitly supplied data. Checkpoints SHALL survive later execution failure, remain separate from result and effects, and SHALL NOT persist into another execution or files. They SHALL NOT capture locals, resolve promises, invoke accessors or inspect live objects automatically. Reference snapshots SHALL NOT imply copied object state.

#### Scenario: Intermediate result survives failure
- **WHEN** code checkpoints a computed height and a later assignment throws
- **THEN** checkpoints contains that recorded height, diagnostics contains the error and result is null if no final return occurred

#### Scenario: No checkpoint does not recover locals
- **WHEN** code computes a local value and then throws without recording it
- **THEN** the response does not pretend to recover that variable as a partial result

#### Scenario: Snapshot and repeated names
- **WHEN** a plain object is checkpointed, later changed and checkpointed under the same name
- **THEN** two ordered entries preserve their respective data snapshots without replacing the earlier entry

#### Scenario: Checkpoint state is execution-local
- **WHEN** a later execute starts after a previous call recorded checkpoints
- **THEN** its checkpoint list starts empty even though explicitly retained bridge references can still be valid

### Requirement: Checkpoint limits are explicit
Checkpoint capture SHALL enforce documented entry-count, UTF-8 name-length, total encoded-byte and nesting-depth limits. A rejected checkpoint SHALL leave previous complete entries intact and SHALL NOT append an incomplete value. Exceeding capture limits SHALL produce CHECKPOINT_LIMIT rather than silently discarding earlier checkpoints.

#### Scenario: Limit exceeded
- **WHEN** another checkpoint would exceed a documented capture quota
- **THEN** capture fails with CHECKPOINT_LIMIT and the response retains the earlier complete checkpoints

#### Scenario: Non-data checkpoint
- **WHEN** checkpoint receives a cyclic object, accessor, function or other unsupported data
- **THEN** it reports an explicit serialization error with the available path and does not execute user code to serialize it

### Requirement: Promises are not serialized as empty objects
Result and checkpoint encoding SHALL reject nested promises with UNAWAITED_VALUE and their data path instead of serializing them as empty objects. The outer async result SHALL retain normal promise adoption, so returning a bridge promise directly is valid. Detection SHALL NOT imply that native effects already dispatched can be undone or preempted.

#### Scenario: Missing await inside a result object
- **WHEN** code returns an object containing a promise from godot.call instead of its awaited value
- **THEN** the result reports UNAWAITED_VALUE with the nested path rather than ok:true with an empty object

#### Scenario: Direct promise return
- **WHEN** code returns godot.call directly as the outer async result
- **THEN** the call can settle normally without requiring an extra syntactic await before return

### Requirement: Bounded valid diagnostic envelopes
Response encoding SHALL respect documented bounds while preserving valid structured JSON and the primary error. Oversized results, stacks, checkpoints, pages or effect summaries SHALL be rejected, bounded or explicitly marked as omitted/truncated. Omitted effects SHALL include an omission count or equivalent incomplete-list indicator; serialization failure SHALL NOT erase already known effects.

#### Scenario: Large data and an error
- **WHEN** a failure response also has large recorded data
- **THEN** the client receives valid JSON with the primary diagnostic and explicit truncation information, not an unmarked partial payload

#### Scenario: Effects cannot all fit
- **WHEN** the response cannot include every known effect under its output budget
- **THEN** it reports that its effect list is incomplete and does not imply that the shown effects are the complete history
