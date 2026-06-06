# Phase 5.0 (bridge core) test obligations — propagated 2026-06-06

Derived with `allium plan` from `specs/mcp-bridge.allium` (54 obligations).
The bridge is Rust (`rmcp`); obligations map to three test kinds, all
runnable by `/phase-gate` and CI:

- **proptest** — the theft analog: property tests over the pure mapping
  logic (`bridge/tests/prop_*.rs` or `#[cfg(test)] proptest!`).
- **integration** — spawn the built bridge binary, drive JSON-RPC over
  its stdin/stdout pipe against a **mock device** (a fake serial/TCP peer
  speaking the frozen wire protocol); `bridge/tests/*.rs`.
- **conformance** — the **MCP Inspector CLI**
  (`npx @modelcontextprotocol/inspector --cli <bin> --method …`) for the
  protocol-shape obligations.

Each obligation has one or more targets; every 5.0 test cites its IDs.

## Lifecycle + negotiation

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.SessionInitialized`, `rule-failure.SessionInitialized.1`, `rule-entity-creation.SessionInitialized.1`, `transition-edge.McpSession.initializing.{ready,failed}` | integration + conformance | `initialize` only succeeds once the mock device's ready handshake completed; otherwise the session fails |
| `rule-success.VersionNegotiated`, `config-default.bridge_protocol_version`, `when-presence.McpSession.negotiated_version`, `invariant.ReadySessionHasVersion` | proptest + conformance | client version == `2025-11-25` → echoed; any other → bridge offers `2025-11-25`; a ready session always carries a non-empty negotiated version (Inspector `initialize`) |
| `rule-success.SessionInitFailed`/`.failure.1`, `rule-success.SessionClosed`/`.failure.1`, `transition-edge.McpSession.ready.closed`, `transition-{rejected,terminal}.McpSession.status` | integration | init-fail and clean-close paths; no transition out of a terminal/closed session |

## Capability gating (the generic contract)

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.CapabilitiesResolved`, `rule-entity-creation.CapabilitiesResolved.1`, `entity-fields.DeviceCapabilities`, `enum-comparable.{MemTier,EncodingMode}` | integration | on ready, capabilities are read from the mock device's features; tiers (`mem`, `encoding`) parse to the right enum values |
| `rule-success.ToolAdvertised`, `rule-failure.ToolAdvertised.1`, `entity-optional.Tool.required_capability`, `invariant.AdvertisedToolsAreCapable`, `entity-fields.Tool` | proptest | `capability_satisfied`: a tool with no required capability is always advertised; one requiring an absent capability is **not** advertised; advertised+capability-requiring ⇒ capability present. (Pins the gate the 5.1–5.4 tools rely on — e.g. `win32_pty_exec` absent when `features.pty=false`.) |

## Tool-call mapping + the isError/protocol split

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.ToolCallDispatched`/`.failure.1`/`creation.1`, `transition-edge.ToolCall.dispatched.{ok,error}` | integration | an advertised tool with valid args is dispatched to the device |
| `rule-success.ToolCallRejected`/`.failure.1`/`creation.1`, `transition-rejected.ToolCall.status`, `invariant.ProtocolFailuresNeverDispatch`, `enum-comparable.FailureKind` | proptest + integration | an unadvertised/unknown tool **or** malformed args → a **JSON-RPC protocol error**, and the device is never touched (mock device records zero traffic) |
| `rule-success.ToolCallSucceeded`/`.failure.1`/`creation.1` | integration | device ok → `CallToolResult` with `content` text **+** `structuredContent` |
| `rule-success.ToolCallRecoverableError`/`.failure.1`/`creation.1`, `when-presence.ToolCall.failure_kind`, `invariant.DispatchedErrorsAreRecoverable` | proptest + integration | a recoverable device failure (busy/timeout/catalog-miss/…) → `isError: true` with the reason text; a dispatched failure is always `recoverable`, never a protocol error |
| `invariant.SuccessResultsAreStructured`, `entity-fields.{ToolCall,ToolResult}` | proptest | `has_structured ⇒ not is_error` (only successes carry a structuredContent mirror) |

## Surfaces

| Obligations | Kind | Test |
|---|---|---|
| `surface-{actor,provides}.McpClient` | conformance | the client boundary: `initialize`/`tools/call` only on a ready session (Inspector drives these) |
| `surface-{actor,provides}.DeviceRuntime` | integration | the device boundary: ok/error replies only for a dispatched call (mock device) |

## Floor

54 obligations → a 5.0 test floor of **≥20 tests** (proptest properties +
integration scenarios + the Inspector conformance run), each citing its
obligation IDs. The mock-device harness is itself a 5.0 deliverable
(reused by 5.1–5.5). Concrete-tool obligations (file-ops, build, memory,
encoding) are propagated with their work-items.
