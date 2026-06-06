# Phase 4 test obligations (propagated 2026-06-06)

Derived with `allium plan` (CLI 3.2.4) from the four Phase 4 specs on
`claude/phase4-specs`: `process-ops.allium` (73), `catalog.allium` (31),
`wire-contract.allium` (43), `mcp-protocol.allium` (64) — 211 obligations.
The test tables in the host repo's `plan/PHASE4.md` are the floor; this
list may add tests, never remove them. Every Phase 4 test file documents
the obligation IDs it covers in a header comment; this file is the index.

Conventions:

- **target** names the test file that discharges the obligation.
- *(existing)* — already covered by a pre-Phase-4 test; referenced, not
  duplicated.
- *(PBT)* — property-based: theft host-native first (autoshrinking),
  mirrored in `prop.h` on the target binary where OS-independent.
- *(scenario)* — discharged by lifecycle walks rather than a dedicated
  unit test; the covering test names the IDs.

## specs/process-ops.allium (73)

| Obligations | Target | Test |
|---|---|---|
| `enum-comparable.{BinaryType,KilledBy,ExecMethod}` | test_exec_ops.c | response fields hold exactly the spec'd literals (`binary_type`, `killed_by`, `exec_method` vocabulary check) |
| `entity-fields.Capabilities` | test_feat.c | `Features` struct populates every capability flag; `has_*` iff `p*` non-NULL |
| `entity-fields.ExecRequest`, `when-presence.ExecRequest.effective_{timeout_ms,output_cap}` | test_exec_ops.c | accepted request resolves sentinels; rejected request never resolves them |
| `config-default.stdin_max` | test_exec_ops.c | stdin of 4096 accepted; 4097 → `"stdin too large"` |
| `config-default.default_timeout_ms` | test_exec_ops.c | `timeout_ms:0`/omitted resolves to 55000 |
| `config-default.max_timeout_ms` | test_exec_ops.c | `timeout_ms:9999999` clamps to 600000 |
| `config-default.output_cap` | test_exec_ops.c | `max_output:0` and `max_output:1000000` both cap at 65536 (silent clamp; truncation flags report) |
| `transition-edge.Process.running.exited` | test_exec_ops.c | PHASE4 #1 (`echo hello` exits 0) |
| `transition-edge.Process.running.timed_out` | test_exec_ops.c | PHASE4 #5 (timeout kill; `killed_by` ∈ {timeout, ctrl_break}) |
| `transition-edge.Process.running.still_active` | test_exec_ops.c | NEW: 16-bit child + timeout → not killed, `still_active`, in-band timed-out error (Wine: simulated via binfmt classification fixture) |
| `transition-edge.Process.still_active.exited` | test_exec_ops.c | NEW: orphan observed exited on next request → request proceeds (implicit reap) |
| `transition-rejected.Process.status`, `transition-terminal.Process.status` | test_exec_ops.c | *(scenario)* exited/timed_out are final; no rule path out |
| `entity-fields.Process` | test_serial.c (integration) | exec response carries `binary_type`, `exec_method`, `killed_by`, `duration_ms` |
| `transition-{rejected,terminal}.ExecResult.status`, `when-presence.ExecResult.error_reason`, `entity-fields.ExecResult` | test_serial.c (integration) | ok response has no `error`; error response has `error` and no output fields |
| `transition-{rejected,terminal}.PtyExecResult.status`, `when-presence.PtyExecResult.error_reason`, `entity-fields.PtyExecResult` | test_pty_exec.c | PHASE4 pty #1–#4 response-shape assertions |
| `rule-success.ExecRequestBusy`, `rule-failure.ExecRequestBusy.1` | test_exec_ops.c | NEW: request while still_active orphan runs → `"busy"` carrying orphan `cmd_line` + elapsed ms; no orphan → not busy (PHASE4 #17 generalised) |
| `rule-success.ExecStdinTooLarge`, `rule-failure.ExecStdinTooLarge.{1,2}` | test_exec_ops.c | oversize stdin rejected; ≤4096 not rejected; busy takes precedence |
| `rule-success.PtyUnavailable`, `rule-failure.PtyUnavailable.{1,2,3}` | test_pty_exec.c | PHASE4 pty #4 (`FeatForceFallback(FORCE_NO_PTY)` → `"pty not available on this Windows"`); pty-capable, non-pty, busy-precedence branches |
| `rule-success.ExecRequestAccepted`, `rule-failure.ExecRequestAccepted.{1,2,3}` | test_exec_ops.c | happy-path admission + the three rejection guards are mutually exclusive (exactly one outcome per request) |
| `rule-success.ProcessSpawned`, `rule-failure.ProcessSpawned.1`, `rule-entity-creation.ProcessSpawned.1` | test_exec_ops.c | PHASE4 #1 (spawn ok), #16 (`started` only after CreateProcessA succeeds) |
| `rule-success.ProcessSpawnFailed`, `rule-failure.ProcessSpawnFailed.1` | test_exec_ops.c | PHASE4 #3, #9, #12 (nonexistent exe / empty cmdline / bad cwd → `"spawn failed"`, no Process) |
| `rule-success.ProcessExits`, `rule-failure.ProcessExits.1` | test_exec_ops.c | PHASE4 #1, #2 (exit codes), #18 (final drain) |
| `rule-success.ProcessTimeoutKilled`, `rule-failure.ProcessTimeoutKilled.{1,2}` | test_exec_ops.c | PHASE4 #5, #21 (no-ctrl-events fallback → `killed_by:"timeout"`) |
| `rule-success.VdmTimeoutNotKilled`, `rule-failure.VdmTimeoutNotKilled.{1,2}`, `rule-entity-creation.VdmTimeoutNotKilled.1` | test_exec_ops.c | NEW: still_active path (above); 32-bit child NOT left running |
| `rule-success.OrphanReaped`, `rule-failure.OrphanReaped.1` | test_exec_ops.c | NEW: reap on re-poll; running (non-orphan) child is not "reaped" |
| `rule-success.JobLimitKills`, `rule-failure.JobLimitKills.{1,2}`, `rule-entity-creation.JobLimitKills.1` | test_exec_ops.c | PHASE4 #22 (memory cap, skip if `!has_create_job_object`), #20 (no-job-objects fallback) |
| `invariant.TimedOutMeansKilled` | test_exec_ops.c | *(PBT, prop.h)* every timed_out result has `killed_by != ""` |
| `invariant.StillActiveNeverKilled` | test_exec_ops.c | still_active orphan has `killed_by:""` |
| `invariant.OnlyVdmChildrenGoStillActive` | test_exec_ops.c | 32-bit timeout never yields still_active |
| `invariant.ShellRoutingReported` | test_catalog.c + test_serial.c | builtin exec reports `exec_method:"shell"` regardless of request `shell` flag |
| `invariant.CtrlBreakRequiresCapability` | test_exec_ops.c | PHASE4 #21 (forced-off capability never reports ctrl_break) |
| `invariant.JobKillRequiresCapability` | test_exec_ops.c | PHASE4 #20 (no job kill without capability) |
| `invariant.PtyResultRequiresCapability` | test_pty_exec.c | PHASE4 pty #4 |
| `invariant.Win32sIsBaseline` | test_feat.c | PHASE4 feat #6 (`FeatForceFallback` consistency; win32s probe implies all uplift flags 0) |
| `surface-actor.SpawnBoundary`, `surface-provides.SpawnBoundary` | test_exec_ops.c | *(scenario)* spawn outcomes only reported for accepted requests |
| `surface-actor.ProcessRuntime`, `surface-provides.ProcessRuntime` | test_exec_ops.c | *(scenario)* exit/timeout events only from running children; reap only from still_active |

## specs/catalog.allium (31)

| Obligations | Target | Test |
|---|---|---|
| `enum-comparable.EntryKind` | test_catalog.c | PHASE4 cat #4 (`dir` → `kind=shell_builtin`) |
| `entity-fields.Catalog` | test_catalog.c | PHASE4 cat #1 (load → entry_count ≥ 30) |
| `entity-fields.CatalogEntry` | test_catalog.c | PHASE4 cat #4 (entry fields populated from JSON) |
| `rule-success.CatalogLoadRecorded` | test_catalog.c | PHASE4 cat #1–#3 (load ok / missing file / malformed JSON) |
| `rule-success.WhitelistDisabledByOperator` | test_serial.c (integration) | `/UNSAFE` startup flag → uncatalogued command executes |
| `rule-success.GateBypassedByUnsafeRequest`, `rule-failure...1`, `rule-entity-creation...1` | test_serial.c (integration) | PHASE4 integration: `unsafe:true` bypasses catalog and response carries `"unsafe_used":true`; `unsafe:false` does not bypass |
| `rule-success.GateUnenforced`, `rule-failure...1`, `rule-entity-creation...1` | test_serial.c (integration) | `/UNSAFE` mode admits with `unsafe_used:false` |
| `rule-success.GateBuiltinHit`, `rule-failure.GateBuiltinHit.{1,2,3}`, `rule-entity-creation...1` | test_catalog.c + test_serial.c | PHASE4 cat #6 (`dir /B` ok) + NEW: builtin routes via shell with `shell:false` in the request (decision 3) |
| `rule-success.GateExternalHit`, `rule-failure.GateExternalHit.{1,2,3}`, `rule-entity-creation...1` | test_catalog.c | PHASE4 cat #8 (`cl /TC file.c` ok; external honours request `shell` flag) |
| `rule-success.GateMiss`, `rule-failure.GateMiss.{1,2}` | test_catalog.c + test_serial.c | PHASE4 cat #5 (`unknown_xyz` → NULL) + integration `"command not in catalog"` |
| `rule-success.GateArgsInvalid`, `rule-failure.GateArgsInvalid.{1,2}` | test_catalog.c | PHASE4 cat #7 (`dir /UNKNOWN` → `"argument not allowed"`) |
| `invariant.LoadedCatalogHasEntries` | test_catalog.c | PHASE4 cat #1 |
| `invariant.EntriesBelongToTheCatalog` | theft_catalog.c | *(PBT)* loaded entries always have non-empty names |
| `surface-actor.ServerStartup`, `surface-provides.ServerStartup` | test_serial.c (integration) | *(scenario)* `/CATALOG:path` override + missing-catalog ready warning |
| — gate exclusivity (cross-rule) | theft_catalog.c | *(PBT)* PHASE4 theft: `CatalogValidateArgs` never accepts unknown flags; glued `/A:v` and split `/A v` validate identically; for any (cmd, flags, unsafe, enforced) exactly one gate outcome |

## specs/wire-contract.allium (43)

| Obligations | Target | Test |
|---|---|---|
| `contract-signature.ReadyHandshake.read_ready` | wire_client.c | `--ready-only`: first line parses as JSON, `status:"ready"` |
| `contract-signature.RequestResponse.{send_request,read_response}` | wire_client.c | echo + exec round-trips over TCP and mock |
| `transition-edge.WireSession.connected.ready`, `rule-success.ReadyReceived`, `rule-failure.ReadyReceived.1` | wire_client.c | ready consumed exactly once, before any command |
| `transition-edge.WireSession.connected.failed`, `rule-success.ReadyMalformed`, `rule-failure.ReadyMalformed.1` | wire_client.c | malformed first line → harness fails the session (no guessing) |
| `transition-edge.WireSession.ready.{closed,failed}`, `rule-success.{SessionClosed,SessionFailed}`, `rule-failure...1` | wire_client.c | clean disconnect vs injected fault paths |
| `transition-{rejected,terminal}.WireSession.status`, `when-presence.WireSession.{codepage,server_version}`, `entity-fields.WireSession` | wire_client.c | ready fields (`codepage` int, `version` non-empty, `features` object with documented keys) recorded on the session |
| `transition-edge.WireRequest.sent.answered`, `transition-{rejected,terminal}.WireRequest.status`, `entity-fields.WireRequest`, `entity-fields.WireResponse` | wire_client.c | each request answered exactly once, in order |
| `rule-success.SessionOpened`, `rule-entity-creation.SessionOpened.1` | wire_client.c | TCP connect (and mock attach) opens a session in `connected` |
| `rule-success.RequestPutOnWire`, `rule-failure.RequestPutOnWire.1`, `rule-entity-creation...1` | wire_client.c | no request before ready (harness enforces ordering) |
| `rule-success.ResponseCorrelated`, `rule-failure.ResponseCorrelated.{1,2}`, `rule-entity-creation...1` | wire_client.c | *(PBT)* correlation ids round-trip for random id strings |
| `invariant.ResponsesMatchTheirRequest` | wire_client.c | *(PBT)* same property, asserted across every exchange |
| `invariant.RequestsOnlyOnReadySessions` | wire_client.c | *(scenario)* harness ordering assertion |
| `invariant.ReadySessionsHaveVersion` | wire_client.c | ready validation rejects empty `version` |
| `surface-actor.{HarnessControl,ServerWire,RequestWire}`, `surface-provides.{...}` | wire_client.c | *(scenario)* covered by the session lifecycle walks above |
| — client robustness (contract guidance) | wire_client.c | *(PBT)* unknown response keys ignored; malformed response lines surface as client errors, never crash the parser |

## specs/mcp-protocol.allium (64)

Pre-Phase-4 dispatch/file-op obligations are already covered; references
below point at the existing suites. New-in-Phase-4 obligations get new
tests.

| Obligations | Target | Test |
|---|---|---|
| `transition-edge.Command.pending.{processed,error}`, `transition-{rejected,terminal}.Command.status`, `rule-success.{CommandCreated,CommandDispatch}`, `rule-failure.CommandDispatch.1`, `rule-success.CommandUnknown`, `rule-failure...1`, `rule-entity-creation...1` | test_serial.c *(existing)* | dispatch + unknown-command tests; extend the known-set check to include `ptyExec` |
| `entity-fields.Command` | test_json.c | *(existing + NEW)* parse new exec fields: `argv` array, `cwd`, `shell`, `unsafe` booleans, `timeout_ms`/`max_output` ints, `stdin_b64` size |
| `entity-fields.Response`, `transition-{rejected,terminal}.Response.status`, `rule-success.ResponseCorrelation`, `invariant.ResponseHasContent` | test_serial.c *(existing)* | response-shape + id-correlation tests |
| `rule-success.{Echo,Read,Write,List,Delete}Command` + their `rule-failure`/`rule-entity-creation` + `rule-success.{Read,Write,List,Delete}{Success,Error}` + creations (28 IDs) | test_serial.c, test_file_ops.c *(existing)* | Phases 1–3 suites; unchanged by this branch |
| `rule-success.ExecCommand`, `rule-failure.ExecCommand.1` | test_serial.c (integration) | NEW: `cmd:"exec"` enters the catalog gate (catalogued cmd executes; uncatalogued rejected) |
| `rule-success.PtyExecCommand`, `rule-failure.PtyExecCommand.1` | test_serial.c (integration) | NEW: `cmd:"ptyExec"` dispatches; non-pty commands never hit the pty path |
| `rule-success.ExecRejectedResponse`, `rule-entity-creation...1` | test_serial.c (integration) | NEW: every rejection reason surfaces as `{"id":...,"status":"error","error":<reason>}`; busy detail rides in the body |
| `rule-success.ExecSuccess`, `rule-entity-creation...1` | test_serial.c (integration) | PHASE4 integration: full exec response with all new keys |
| `rule-success.ExecError`, `rule-entity-creation...1` | test_serial.c (integration) | timed-out exec → error response with same id |
| `rule-success.PtyExecSuccess`, `rule-entity-creation...1` | test_pty_exec.c + test_serial.c | PHASE4 pty #1 (`output_b64`, `output_kind:"ansi"`) |
| `rule-success.PtyExecError`, `rule-entity-creation...1` | test_pty_exec.c | PHASE4 pty #4 error envelope |
| `surface-actor.TransportConnection`, `surface-exposure...`, `surface-provides...` | test_transport.c *(existing)* | commands only on connected transports |

## Floor reconciliation

All PHASE4.md test-table entries remain; this propagation **adds** the
following beyond the floor (decisions 1–11 made them obligations):

1. test_exec_ops.c: still_active lifecycle (timeout-no-kill, busy-with-
   detail, implicit reap, reap-unblocks) — 4 tests.
2. test_exec_ops.c: config sentinel resolution (stdin_max boundary,
   default/clamp timeout, output clamp) — 4 tests (PHASE4 #13 reworded:
   `timeout_ms:0` now means *server default 55000*, not "no timeout").
3. test_exec_ops.c: admission exclusivity (busy > pty-capability > stdin
   precedence) — 1 test.
4. test_catalog.c / test_serial.c: builtin auto-route with `shell:false`
   reports `exec_method:"shell"` — 1 test.
5. test_serial.c: `unsafe_used:true` response field (never stderr) — in
   the existing unsafe integration test's assertions.
6. theft_catalog.c: gate-exclusivity property — 1 property.
7. wire_client.c: scope fixed by 4.0b; its PBT properties trace to the
   `RequestResponse` contract guidance above.

Floor: ≥154 tests (PHASE4.md criterion 13). With the additions: **≥163**.
