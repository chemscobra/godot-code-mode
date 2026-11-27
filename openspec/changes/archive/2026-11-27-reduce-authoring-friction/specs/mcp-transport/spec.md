# Spec Delta

## ADDED Requirements

### Requirement: Session token in initialize body
The `initialize` response SHALL carry the session token in both the
`MCP-Session-Id` header and the result body, so clients that cannot read
response headers can still complete the handshake.

#### Scenario: Token readable without headers
- **WHEN** a client completes `initialize` but cannot access response
  headers
- **THEN** the session token is available inside the `initialize` result
  body

#### Scenario: Header remains authoritative
- **WHEN** a conforming client reads the `MCP-Session-Id` header
- **THEN** the token matches the one in the result body

### Requirement: External-action rule documented
The `execute` tool description SHALL state that external actions
(declared `pluginAction`s such as scene creation and playback) require
their own `execute` call with no other writes, so `MIXED_EFFECTS` is
predictable rather than discovered by failure.

#### Scenario: External-action rule documented
- **WHEN** a client reads the `execute` tool contract
- **THEN** the description states that external actions must run in their
  own `execute` call, isolated from writes
