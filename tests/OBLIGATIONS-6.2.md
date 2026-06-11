# 6.2 (Win32s direct-UART serial route) test obligations — propagated 2026-06-11

Derived with `allium plan` from `specs/uart.allium` (new), diffed against the
pre-UART baseline (`9809aab`, the loadability merge): **37 new structural
obligations** — all in `uart.allium` (the whole file is new). `serial.allium`'s
only change is a clarifying comment (no structural obligation); `transport.allium`
is untouched (the route is invisible at the transport layer — kind/name stay
`serial`). Nothing was removed from coverage; the floor only grows.

**Three prose-discipline invariants carry NO structural ID** (they are prose, not
expression-bearing, so `allium plan` emits nothing for them) — they are the
mechanism/termination properties and are traced to concrete tests + static checks
+ the weed gate-bypass audit instead (see "The six security pins" below):
`BarePortIoNoEscalation`, `UartOwnedExclusively`, `EveryPollLoopBounded`.

## Testability boundary (CI parity — load-bearing)

The direct-UART path's atom is a bare ring-3 `IN`/`OUT` to 0x3F8–0x3FF. That atom
is **un-executable in CI / on the dev host**: the runners are x86_64 NT-family
(Wine / WSL interop), there is no 16550 at 0x3F8, and a bare `IN` would fault.
So coverage is split exactly like 5.3's mem_ops (pure guards host-tested; the live
syscall on-target):

- **Pure ladder + driving logic is host-tested** by factoring it behind an
  injected port-I/O seam (`UartPortIo`: `in`/`out`/`yield` + ctx) and driving it
  against a **simulated 16550** (a stateful register machine). The asm `inb`/`outb`
  are excluded from the host build by `-DUART_HOST_PURE` (mirrors
  `-DMEM_OPS_HOST_PURE`), so the host harness never compiles them.
- **The tier dispatch** (`is_win32s` ⇒ direct route) lives behind that same wall,
  so it is **not** host-testable. It is asserted **on-target** in `test_serial.c`
  by setting `g_features.is_win32s`/`is_nt` directly and reading which route was
  selected via a `UartLastRouteForTest()` probe — **with no real port I/O** (the
  probe records the decision, it does not drive the chip).
- **The live asm seam** is verified on the **pinned QEMU Win32s guest** — the
  throwaway spike (`tools/phase6-qemu/uart-probe.c`) already proved the physics
  full-duplex; the integrated backend re-runs it as the #35 acceptance
  (operator-driven human gate). The simulated 16550's register semantics are a
  **transcription of that spike's observed behaviour**, not the datasheet alone —
  the spike is the only thing that ran on real hardware.
- **Degraded real hardware** (8250/16450 no-FIFO, non-A 16550, dead-clone) is the
  **QEMU stretch — task #39** (feasibility TBC: QEMU models a 16550A always). The
  simulated 16550 is the **exhaustive** degraded-logic coverage; the QEMU run is
  the fidelity stretch. **Do not assert degraded-chip behaviour against Wine.**

## Test kinds

- **prop (theft, host)** — new `tests/host/theft_uart.c` + the simulated-16550
  fake: deep generative (50k trials, ASan, autoshrink) over the OS-independent
  ladder/driving — the strongest pin on the security invariants.
- **prop (C89, on-target)** — new `tests/test_uart.c`: `prop.h` mirrors of the
  theft properties, proving they hold on the actual C89/i386 build (Wine/native).
- **unit (C89)** — `tests/test_serial.c`: the dispatch-gate tests (route selection
  by tier) via `UartLastRouteForTest()`, no real port I/O.
- **static (CI)** — `build-and-test.yml`: extend the 486+/FPU objdump grep to
  `uart.dir/*.obj` and add a no-escalation opcode grep; the import-allowlist diff
  (now ordinal-fail-closed) must show the UART backend adds **zero** imports.
- **on-target (Phase 6 hardware, operator-driven)** — the live wire round-trip on
  the pinned Win32s guest = the #35 acceptance.

---

## Device: the detection-ladder lifecycle (rules + transitions)

| Obligation | kind | Test |
|---|---|---|
| `entity-fields.UartChip`, `enum-comparable.ChipKind` | prop (host+target) | `uart_chip_shape`: a driven chip records `{base_port, host_is_win32s, chip_kind, fifo_enabled, ier_enabled, out2_enabled, tx_chunk, divisor, status}`; `ChipKind` values are comparable |
| `rule-success.UartRouteSelected`, `rule-entity-creation.UartRouteSelected.1` | prop | `route_selected_creates_detecting`: a Win32s route request creates a `UartChip` in `detecting` with the single-byte defaults (`tx_chunk=1`, `fifo_enabled=false`, `divisor=0`) |
| `rule-failure.UartRouteSelected.1` **(SECURITY PIN #1, the tier gate)** | unit (target, `test_serial.c`) | `uart_route_only_on_win32s`: a route request with `host_is_win32s=false` creates **no** `UartChip` (off-Win32s ⇒ no direct route); paired with the dispatch-gate test that NT selects the `CreateFileA` path and forced-`is_win32s` selects the direct route — **no real port I/O** |
| `rule-success.UartPortAbsent`, `rule-failure.UartPortAbsent.1/.2` | prop | `presence_absent_fails_open`: the IER store-test reading a floating bus (`unknown`) → `open_failed`; the rule is rejected from a non-`detecting` state or with a present (`!=unknown`) chip |
| `rule-success.UartChipIdentified`, `rule-failure.UartChipIdentified.1/.2` **(SECURITY PIN #4)** | prop (host, the headline detection property) | `fifo_iff_16550a`: across random `chip_kind`, the `0xC0`(16550A) fake ⇒ `fifo_enabled=true, tx_chunk=16`; the `0x80`(non-A) and `0x00`(8250/16450) fakes ⇒ `fifo_enabled=false, tx_chunk=1`; rejected from a non-`detecting` state or with `unknown` |
| `rule-success.UartGoesLive`, `rule-failure.UartGoesLive.1/.2` | prop | `loopback_pass_goes_live`: loopback `passed ∧ divisor_verified` ⇒ `live` with `divisor` set and `ier_enabled/out2_enabled=false`; rejected from a non-`loopback_pending` state or when `passed`/`divisor_verified` is false |
| `rule-success.UartOpenFailsLoopback`, `rule-failure.UartOpenFailsLoopback.1/.2` **(fail-closed)** | prop (host) | `loopback_fail_closed`: a dead/clone fake that decodes registers but does not truly loop, or that ignored the divisor write (`divisor_verified=false`), ⇒ `open_failed`; rejected from a non-`loopback_pending` state or when both pass |
| `rule-success.UartCloses`, `rule-failure.UartCloses.1` | prop | `close_ends_live`: an orderly close moves `live → closed`; rejected from any non-`live` state |
| `rule-success.UartCommsErrorCloses`, `rule-failure.UartCommsErrorCloses.1` | prop | `comms_error_ends_live`: a comms fault moves `live → closed` (the only other live exit); rejected from any non-`live` state |
| `transition-edge.UartChip.*` (×5), `transition-rejected.UartChip.status`, `transition-terminal.UartChip.status` | prop (state machine) | `uart_lifecycle_transitions`: a full walk `detecting → loopback_pending → live → closed` and both `→ open_failed` fail paths; `open_failed`/`closed` are terminal (a second event on a consumed chip is rejected); no undeclared edge is accepted |
| `surface-provides.UartRouteOperator/UartHardwareProbe`, `surface-actor.*` | (internal) | the UART surfaces are **device-internal** boundaries (operator = the serial backend selecting the route; hardware = the port-I/O seam reporting ladder outcomes), **not** MCP wire commands — covered by the route-selection (dispatch) + seam-driven ladder tests above, not a wire round-trip |

## Device: the security/safety invariants

| Obligation | kind | Test |
|---|---|---|
| `invariant.ServingViaUartImpliesWin32s` **(PIN #1)** | unit (target) | `uart_route_only_on_win32s` (above) + weed gate-bypass: no path (esp. the `/AUTO` serial fallback re-entering `SerialBackendOpen`) constructs a `live` chip off Win32s |
| `invariant.NoInterruptPathArmed` **(PIN #3)** | prop (host, write-trace) | `interrupts_never_armed`: the fake records every `out` to IER(+1)/MCR(+4) across open/tx/rx/close; assert IER stays `0` and MCR OUT2 (bit 3) stays **clear** at all live points |
| `invariant.FifoEnabledImpliesDetected16550A` **(PIN #4)** | prop (host) | `fifo_iff_16550a` (above): `fifo_enabled ⇒ chip_kind=uart_16550a` **and** `tx_chunk>1 ⇒ chip_kind=uart_16550a` — the false-16550A-positive is the only dangerous direction |
| `config-default.fifo_tx_chunk` (16), `config-default.single_byte_tx_chunk` (1) | prop | `tx_chunk_defaults`: burst is `16` only on a detected 16550A, `1` otherwise; ties to PIN #4 |
| `BarePortIoNoEscalation` **(PIN #2, prose — no structural ID)** | static + weed | `no_escalation_opcodes`: CI objdump-greps `uart.dir/*.obj` for **only** `in`/`out` port ops — no `cli`/`sti`/`int`/`lgdt`/`lidt`/`call far`/segment-reg loads; the import-allowlist shows **zero** new imports (no VxD/DPMI entry); weed reads the seam and confirms no escalation primitive |
| `UartOwnedExclusively` **(PIN #5, prose — no structural ID)** | static + weed | `no_os_comm_open`: the `uart.c` TU contains **no** `CreateFileA("COM1"`/`OpenComm` reference (the route never opens COM1 via the OS); weed confirms COMM.DRV is never engaged on this route |
| `EveryPollLoopBounded` **(PIN #6, prose — no structural ID)** | prop (host) | `open_loops_bounded`: a never-ready fake (never asserts THRE/DR) ⇒ every **open-phase** loop (presence, detect, loopback, divisor read-back, TX THRE/TEMT) terminates within its hard bound and returns an error — the op/yield counter is bounded, no spin. **RX exception:** `rx_idle_is_live` — a `yield`-then-repoll RX poll on an idle line never returns 0/ends the session; it ends only on data or a comms error |

---

## The six security pins (consolidated)

| # | Invariant | Kind | Test / check |
|---|---|---|---|
| 1 | `ServingViaUartImpliesWin32s` | expr + dispatch | `uart_route_only_on_win32s` (target, `UartLastRouteForTest`) + weed gate-bypass on `/AUTO` |
| 2 | `BarePortIoNoEscalation` | prose | `no_escalation_opcodes` CI grep + zero-new-imports + weed seam read |
| 3 | `NoInterruptPathArmed` | expr | `interrupts_never_armed` (host write-trace: IER=0 ∧ OUT2 clear) |
| 4 | `FifoEnabledImpliesDetected16550A` | expr | `fifo_iff_16550a` (host, the one load-bearing detection rule) |
| 5 | `UartOwnedExclusively` | prose | `no_os_comm_open` static grep + weed (no CreateFile/OpenComm on the route) |
| 6 | `EveryPollLoopBounded` | prose | `open_loops_bounded` (host, never-ready fake) + `rx_idle_is_live` |

## Recorded non-obligations (no test asserts these)

- **No live UART verification on CI/Wine.** The asm `IN`/`OUT` is un-executable on
  the x86_64 NT runners; the integrated live wire round-trip is **Phase 6
  hardware** (operator-driven, the pinned guest). Do not assert real-chip
  behaviour against Wine.
- **No degraded-chip run on stock QEMU yet.** Smoke-testing the 8250/16450/non-A/
  dead-clone downgrade branches against emulated hardware is **task #39**
  (feasibility TBC — QEMU models a 16550A always). The simulated 16550 is the
  exhaustive degraded-*logic* coverage; the QEMU run is a fidelity stretch.
- **The 16-byte FIFO depth is an implementation/config detail**, not a pinned
  case; the test pins `tx_chunk>1 ⇒ 16550A` and burst ≤ detected depth, not the
  number 16 (it lives in `config.fifo_tx_chunk`).
- **The VC6 `__asm{}` port-I/O block is not CI-built** (VC6 is not in CI); its
  correctness is a manual Phase-6 build-grep, not an automated obligation.
- **The cooperative-yield primitive** (`Sleep(0)`/`Sleep(1)` vs a `PeekMessage`
  pump) is an open hardware question; the obligation pins *bounded yield*, not the
  specific primitive (abstracted behind `UartPortIo.yield`).

## Floor

New: **≥7 theft host properties** (`theft_uart.c`: `fifo_iff_16550a`,
`loopback_fail_closed`, `interrupts_never_armed`, `divisor_round_trip`,
`open_loops_bounded`, `rx_idle_is_live`, `tx_burst_le_depth`/`lsr_before_rbr`) +
**≥7 on-target `prop.h` mirrors** (`test_uart.c`) + **≥2 dispatch-gate units**
(`test_serial.c`: NT⇒CreateFile, forced-`is_win32s`⇒direct) + the static CI checks
(no-escalation opcode grep, no-OS-comm grep, zero-new-imports). Existing floors
only grow (`OBLIGATIONS-PHASE4.md` ≥163 device; 5.0–5.5 unchanged).
