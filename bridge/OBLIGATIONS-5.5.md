# 5.5 (exec discovery: exec / ptyExec / listCommands tools) bridge test obligations — propagated 2026-06-08

Derived with `allium plan` from `specs/mcp-bridge.allium`, diffed against the
pre-5.5 baseline (`470ded9`, the 5.4 merge): **20 new bridge obligations**
(all in `mcp-bridge.allium`). The implementation does **not** exist yet — these
are the forward checklist the implement stage traces each test to.

Three existing obligation IDs change **content, not identity** (no new ID, the
existing test grows an assertion):

- `entity-fields.ToolCall` — gains `unsafe_requested` and `outcome` (the
  per-call unsafe flag and which `RecoverableOutcome` path the call took).
- `entity-fields.DeviceCapabilities` — gains `allow_unsafe_exec` (the
  bridge-operator opt-in for the unsafe catalog bypass; modelled exactly like
  `allow_memory_write`).
- `enum-comparable.RecoverableOutcome` is genuinely new (the 4-way
  `relayed | device_circuit_open | rate_limited | unsafe_opt_in_absent`
  partition driving the local-refusal vs relayed split).

The three exec tools are **1:1 relays** to three device wire commands
(`exec` / `ptyExec` / `listCommands`), additive to the frozen contract — like
5.1's copy/move/mkdir/rmdir and 5.3's memory commands, **unlike** 5.2's composed
build tools (`device_cmd` non-null for all three; no bridge-composed pipeline).

Test kinds (reuse the existing harnesses — do not rebuild):

- **integration (Rust)** — new `bridge/tests/exec_tools.rs` over the duplex
  `serve()` + `RecordingDevice` mock (`bridge/tests/common/mod.rs`); the
  `caps(...)` / `list_tools(...)` / `find(...)` helpers from `memory.rs` are the
  template (capability prune, relay-mapping, honest-hints conventions).
- **proptest** — `bridge/tests/props.rs` (extend the existing `satisfies_*` /
  capability properties; the unsafe-opt-in partition property).
- **conformance** — MCP Inspector CLI in CI (the existing `tools/list` /
  `tools/call` conformance run, extended to the three new tools).

A recoverable device error → `isError` and the ok structuredContent round-trip
are the **5.0 generic mapping** (`ToolCallSucceeded` / `ToolCallRecoverableError`),
already covered by `integration.rs`; the 5.5 tests assert these only at the
exec-tool seam (one representative per tool), not re-derive the generic mapping.

## The three exec tools: registration + capability prune

| Obligation | kind | Test |
|---|---|---|
| `rule-success.ExecToolsRegistered`, `rule-entity-creation.ExecToolsRegistered.1` | integration | `exec_tools_registered`: on a ready, pty-capable device, `tools/list` shows `win32_exec` (`device_cmd:"exec"`, destructive), `win32_pty_exec` (`device_cmd:"ptyExec"`, `required_capability:"pty"`, destructive), `win32_list_commands` (`device_cmd:"listCommands"`, read-only), each with a 2020-12 object inputSchema |
| `entity-fields.ToolCall` (content: `unsafe_requested`/`outcome`) | integration | covered transitively by the dispatch/refusal tests below — every created `ToolCall` carries the two new fields (`unsafe_requested` false for non-exec / plain exec; `outcome` = which path) |
| `invariant.PtyToolGatedOnCapability` + the G1 prune | integration | `pty_exec_pruned_when_no_pty`: with `has_pty:false`, `win32_pty_exec` is **absent** from `tools/list`; with `has_pty:true` it is present (the prune→absence path, the memory `peek_pruned_when_mem_none` precedent). `win32_exec` and `win32_list_commands` are present in **both** cases (no required capability) |

## The unsafe-exec safety pin (`UnsafeExecRequiresOperatorOptIn`)

| Obligation | kind | Test |
|---|---|---|
| `rule-success.ExecUnsafeRejected`, `rule-entity-creation.ExecUnsafeRejected.1`, `rule-failure.ExecUnsafeRejected.{1,2,3}` **(SAFETY PIN)** | integration | `unsafe_exec_refused_without_opt_in`: `win32_exec` with `unsafe:true` on a session whose `allow_unsafe_exec:false` → a **recoverable** `isError` ToolResult with text exactly `"unsafe exec not permitted: operator opt-in required"`, `has_structured:false`; the call is created already in `error` with `outcome:unsafe_opt_in_absent` and **is NOT relayed** — assert the `RecordingDevice` received **nothing** (`received` log empty). The three `rule-failure` arms are the guard's negative space: not refused when (1) not unsafe, (2) opt-in present, (3) not advertised/invalid args (those go to dispatch/reject, not this rule) |
| `rule-failure.ToolCallDispatched.{2,3}` (the new opt-in/unsafe arms) **(SAFETY PIN, dispatch half)** | integration | `unsafe_exec_relayed_with_opt_in`: `win32_exec` with `unsafe:true` on a session whose `allow_unsafe_exec:true` → **relayed** to the device with `unsafe:true` on the wire (the `RecordingDevice` records exactly one `exec` carrying `unsafe`), `outcome:relayed`; a plain `win32_exec` (`unsafe:false`) is always relayed regardless of opt-in. These two new failure arms of `ToolCallDispatched` are the `not unsafe_requested or caps.allow_unsafe_exec` precondition |
| `invariant.UnsafeExecRequiresOperatorOptIn` **(SAFETY PIN — PBT)** | proptest | `props.rs` `unsafe_reaches_device_only_under_opt_in`: over random `(unsafe_requested, allow_unsafe_exec)` and tool, the implication `dispatched ∧ unsafe_requested ⟹ allow_unsafe_exec` holds, **and** `outcome = unsafe_opt_in_absent ⟹ (status = error ∧ unsafe_requested)`. The bridge analogue of the device's `ShellTailNeutralised` pin — the property is named and tested, never abstracted |

## Runtime resilience: circuit-breaker + rate-limit (`BridgeResilience`)

| Obligation | kind | Test |
|---|---|---|
| `rule-success.ToolCallCircuitOpen`, `rule-entity-creation.ToolCallCircuitOpen.1` **(resilience outcome)** | integration | `circuit_open_refuses_locally`: a tripped device breaker → `win32_exec` (or any tool) returns a **recoverable** `isError` with text exactly `"device circuit open"`, `has_structured:false`, `outcome:device_circuit_open`; the `RecordingDevice` is **never touched** (`received` empty). A breaker-tripped fixture drives `DeviceCircuitOpen` |
| `rule-success.ToolCallRateLimited`, `rule-entity-creation.ToolCallRateLimited.1` **(resilience outcome)** | integration | `rate_limit_sheds_locally`: a rate-limit trip → a **recoverable** `isError` with text exactly `"rate limited"`, `has_structured:false`, `outcome:rate_limited`; the device is **never touched** (`received` empty). A rate-cap-exceeded fixture drives `ToolRateLimited` |
| `enum-comparable.RecoverableOutcome` | proptest (or unit) | `outcome_variants_compare`: the four variants (`relayed`/`device_circuit_open`/`rate_limited`/`unsafe_opt_in_absent`) are comparable as the `ToolCall.outcome` field; the three non-`relayed` kinds are exactly the local-refusal set asserted above (each maps to one fixed isError text) |
| `surface-actor.BridgeResilience`, `surface-provides.BridgeResilience` | integration | covered by the two resilience tests above: `DeviceCircuitOpen` / `ToolRateLimited` are provided only on a `ready` session, and only the bridge raises them (no device round-trip). No separate exposure test — the provides `when status = ready` is satisfied by the ready session the tests already build |

## Honesty + relay invariants (assertion tests on the tool table)

| Obligation | kind | Test |
|---|---|---|
| `invariant.ExecToolHintsAreHonest` | integration | `exec_tool_hints_honest`: `win32_exec`/`win32_pty_exec` carry `destructive` and **not** `read_only`; `win32_list_commands` carries `read_only` and **not** `destructive`; no tool is both (mirrors `memory_tool_hints_honest`) |
| `invariant.ExecToolsAreDirectRelays` | integration | `exec_tools_are_relays`: each of the three exec tools has `device_cmd != null` (`exec`/`ptyExec`/`listCommands`); calling each round-trips to exactly its `device_cmd` with name-mapped args, one call per tool (the `RecordingDevice` records exactly the relayed verb) — unlike the composed build steps whose `device_cmd` is null |
| `invariant.PtyToolGatedOnCapability` | integration | asserted by `pty_exec_pruned_when_no_pty` above (the tool always names `required_capability:"pty"`, so `AdvertisedToolsAreCapable` prunes it on a non-ConPTY device) |

## The audit guarantee (`@guarantee PowerToolsAreAudited`)

| Obligation | kind | Test |
|---|---|---|
| `@guarantee PowerToolsAreAudited` (a surface guarantee — no structural ID; the bridge counterpart to the device's poke audit) | integration | `power_tool_calls_are_audited`: a `win32_exec` / `win32_pty_exec` call produces a durable audit record carrying the tool name, the args, the outcome, and (for exec) whether `unsafe` was used — written whether the call was **relayed**, **refused locally** (circuit open / rate limited / unsafe opt-in absent), **or** errored at the device. Assert the record exists for each of those four outcome paths; `win32_list_commands` (read-only, non-power) produces **no** audit record |

## The seven-pin / outcome map (consolidated)

| Pin / outcome | Structural ID | Test | Side |
|---|---|---|---|
| `UnsafeExecRequiresOperatorOptIn` (refuse) | `rule-success.ExecUnsafeRejected` (+`.1`/`.failure.{1,2,3}`/`creation.1`) | `unsafe_exec_refused_without_opt_in` | bridge |
| `UnsafeExecRequiresOperatorOptIn` (relay) | `rule-failure.ToolCallDispatched.{2,3}` | `unsafe_exec_relayed_with_opt_in` | bridge |
| `UnsafeExecRequiresOperatorOptIn` (property) | `invariant.UnsafeExecRequiresOperatorOptIn` | `unsafe_reaches_device_only_under_opt_in` (proptest) | bridge |
| circuit-breaker open | `rule-success.ToolCallCircuitOpen` (+`creation.1`) | `circuit_open_refuses_locally` | bridge |
| rate-limit shed | `rule-success.ToolCallRateLimited` (+`creation.1`) | `rate_limit_sheds_locally` | bridge |
| power-tool audit | `@guarantee PowerToolsAreAudited` (no ID) | `power_tool_calls_are_audited` | bridge |

The device half of the unsafe-exec defence (the catalog `GateBypassedByUnsafeRequest`
+ `ShellTailNeutralised`) is **independent** and already pinned on the device side
(catalog.allium, Phase 4 / earlier). The bridge half asserted here is the
operator-opt-in relay gate only.

## Intentional gaps / non-obligations (no bridge test asserts these)

- **The circuit-breaker / rate-limiter trip *policy* (thresholds, half-open
  recovery timing) is implementation tuning, not a pinned case.** The tests pin
  the **refusal behaviour** (local isError, fixed text, device untouched) given a
  tripped breaker / exceeded cap via a fixture, not the threshold values or the
  recovery state machine. The breaker has no spec-modelled state field, so there
  is no transition obligation.
- **The audit sink format / durability mechanism is not spec-modelled on the
  bridge side** (`PowerToolsAreAudited` is a prose guarantee, no entity). The test
  asserts a record is produced per power-tool call and outcome, not its
  serialisation (contrast the device's `AuditRecord` entity, which *is* shape-pinned).
- **`win32_exec`'s lack of a `required_capability`** is asserted positively
  (present on every ready device) — there is no "exec pruned" case, by design
  (exec is uniform across every OS tier).
- **The device-side `exec` / `ptyExec` / `listCommands` semantics** (spawn,
  capture, timeout, catalog gate, the listing payload) are device obligations in
  `tests/OBLIGATIONS-5.5.md` and the Phase 4 floor — the bridge tests treat the
  device as a relay target (the `RecordingDevice` mock), not re-verify it.

## Floor

New: **≥8 bridge integration** (`exec_tools.rs`: `exec_tools_registered`,
`pty_exec_pruned_when_no_pty`, `unsafe_exec_refused_without_opt_in`,
`unsafe_exec_relayed_with_opt_in`, `circuit_open_refuses_locally`,
`rate_limit_sheds_locally`, `exec_tool_hints_honest` + `exec_tools_are_relays`,
`power_tool_calls_are_audited`) + **≥1 proptest**
(`unsafe_reaches_device_only_under_opt_in`) + the extended **Inspector
conformance** run (the three new tools in `tools/list` / `tools/call`). Existing
floors only grow — every pre-5.5 bridge test keeps passing
(`OBLIGATIONS-5.0.md` ≥20, `OBLIGATIONS-5.1.md` ≥5, `OBLIGATIONS-5.2.md` ≥18,
`OBLIGATIONS-5.3.md` ≥6, `OBLIGATIONS-5.4.md` ≥3 bridge).
