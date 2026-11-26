# mcp-transport Specification

## Purpose

Provides the local MCP server inside the Godot editor: Streamable HTTP transport on loopback, session-token authorization, protocol version negotiation, and a stdio adapter for stdio-only clients.

## Requirements

### Requirement: Protocol negotiation and tool contracts
The server SHALL negotiate the MCP protocol version and expose only the `search` and `execute` tools to authorized clients.

#### Scenario: Valid client lists tools
- **WHEN** a client with a valid session token completes `initialize` and requests `tools/list`
- **THEN** it receives the contracts for both `search` and `execute`

#### Scenario: Invalid token rejected
- **WHEN** a request arrives with an invalid or missing token
- **THEN** the request is rejected and no tool contract is disclosed

### Requirement: Loopback-only transport
The server SHALL bind only to 127.0.0.1 on a configurable port and SHALL validate the request Origin.

#### Scenario: Remote origin refused
- **WHEN** a request presents a disallowed or absent Origin
- **THEN** the server refuses it before any protocol processing

### Requirement: Stdio adapter
A stdio adapter process SHALL proxy MCP stdio traffic to the HTTP transport so that stdio-only clients can connect without a second protocol implementation.

#### Scenario: Stdio-only client connects
- **WHEN** a stdio-only client launches the adapter and completes `initialize`
- **THEN** the client can call `tools/list` and `tools/call` through the adapter transparently

### Requirement: Lifecycle shutdown
The addon SHALL stop the service, invalidate all sessions and handles, and cancel pending work when the plugin is disabled or the project changes.

#### Scenario: Plugin disabled during execution
- **WHEN** the plugin is disabled while an `execute` call is in flight
- **THEN** pending work is cancelled and no late mutations are applied

### Requirement: Cancellation
The server SHALL honor client cancellation requests for in-flight tool calls.

#### Scenario: Client cancels an execution
- **WHEN** a client sends a cancellation for a running `execute` call
- **THEN** the JS execution is interrupted and its pending queued operations are dropped
