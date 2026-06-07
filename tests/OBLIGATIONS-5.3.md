# 5.3 (memory peek/poke subsystem) test obligations — propagated 2026-06-07

Derived with `allium plan` from `specs/memory-ops.allium` (new),
`specs/mcp-bridge.allium` and `specs/wire-contract.allium`, each diffed against
the pre-5.3 baseline (`2d312e4`, the 5.2 merge): **40 new obligations** — 36 in
`memory-ops.allium` (the whole file is new) and 4 in `mcp-bridge.allium`, **0
structural in `wire-contract.allium`** (its `ReadyShape`/`CommandsAreAdditive`
changes are prose `@invariant`s, which yield no structural obligation). Nothing
was removed from coverage; the floor only grows.

Two relocations/content-changes, not new coverage:
- `enum-comparable.MemTier` **moved** from `mcp-bridge.allium` to
  `memory-ops.allium` (the tier is a device-side concept; the bridge now
  references `memops/MemTier`). It is one of the 36 memory-ops obligations.
- `entity-fields.DeviceCapabilities` changes **content** (gains
  `allow_memory_write`, the bridge-operator opt-in for memory writes) — same ID.

Two behaviours ride prose `@invariant`s and so carry no structural ID — traced
to concrete rules/tests instead:
- The 7th safety pin **`PokeRequiresBothArmingLayers`** is a surface
  `@guarantee` (a bare top-level `@invariant` is not legal). Its two halves are
  pinned concretely: the device wire arm by `MemoryPoked`'s `requires:
  memory_write_armed` (obligation `rule-failure.MemoryPoked.1`) and the bridge
  advertisement gate by `invariant.MemoryWriteToolRequiresOptIn`. Tested on both
  sides.
- The device ready message's `features.mem` tier (`wire-contract` `ReadyShape`)
  — traced to the device tier-detection + ready-envelope tests.

**No bridge-composed tool this time** (unlike 5.2's build tools): the five
memory tools are **1:1 relays** to five NEW device wire commands
(`spawnRetain`/`peek`/`poke`/`terminate`/`release`, `device_cmd` set), additive
to the frozen contract (like 5.1's copy/move/mkdir/rmdir).

## Testability boundary (CI parity — load-bearing)

The dev host (WSL interop) and CI (Wine) are **NT-family**, so the device's
`mem` tier is **`process`** there. Only the `process`-tier path
(`ReadProcessMemory`/`WriteProcessMemory` on a child we `spawnRetain`) is
**executable in CI**. The pre-NT `arena`/`shared_vm` raw-load/store branches are
never taken on NT, so their integration tests **skip-with-reason** on NT-family
hosts (the `test_pty_exec.c` SKIP convention); their live verification is
**Phase 6 hardware**. The **pure-logic** guards are unit/property-tested
everywhere by factoring them out of the live syscall: `MemParseU32`, the
overflow-safe range guard, the `region_accessible` decision given a synthetic
`MEMORY_BASIC_INFORMATION`, token-table validity, and the bridge gating.

## Test kinds

- **unit (C89)** — new `tests/test_mem_ops.c` (MemParseU32, the range/overflow
  guard, the token table, the `region_accessible` decision over a synthetic
  MBI, the audit-record formatting); `tests/test_serial.c` for the
  `features.mem` ready envelope + the process-tier peek/poke round-trip + the
  device-arm refusal; `tests/test_feat.c` for the OS-family→tier mapping;
  `tests/test_json.c` for the new `mem_addr`/`mem_len`/`mem_token` string fields.
- **prop (C89)** — `tests/prop.h` on-target + **theft** host-pbt for the
  OS-independent arithmetic: the range/wraparound guard and `MemParseU32`
  (`tests/host/`), the strongest pin on the off-by-overflow class.
- **integration (Rust)** — `bridge/tests/memory.rs` (new) over the duplex mock
  device; **proptest** — `bridge/tests/props.rs` (the gating composition);
  **conformance** — MCP Inspector CLI in CI.

---

## Device: the spawn-retain lifecycle + tier dispatch

| Obligation | kind | Test |
|---|---|---|
| `entity-fields.RetainedProcess`, `enum-comparable.RetainStatus` | unit | `test_mem_ops.c` `retained_process_lifecycle`: a `spawnRetain` of a catalogued child records `{token, pid, command, status:retained}`; the token round-trips |
| `transition-edge.RetainedProcess.retained.released` / `.terminated`, `transition-terminal.RetainedProcess.status`, `transition-rejected.RetainedProcess.status` | unit (state) | `token_lifecycle_transitions`: `release` moves retained→released, `terminate` retained→terminated; both are terminal (a second `release`/`terminate`/`peek` on the consumed token is rejected); no other transition is accepted |
| `rule-success.ProcessRetained`, `rule-entity-creation.ProcessRetained.1` | unit | `spawn_retain_creates_target`: process-tier `spawnRetain` of a catalogued exe → a live token whose handle RPM/WPM accepts (WSL/Wine) |
| `rule-failure.ProcessRetained.1` + `invariant.SpawnRetainCommandIsCatalogued` **(SAFETY PIN #5)** | unit | `spawn_retain_requires_catalogued`: `spawnRetain` of an **uncatalogued** command is refused — spawnRetain cannot launch outside the exec whitelist (the launch-anything-bypass guard; mirrors 5.2 `definition_commands_must_be_catalogued`) |
| `rule-success.ProcessReleased`, `rule-failure.ProcessReleased.1` | unit | `release_consumes_token`: `release` on a retained token closes the handle + frees the slot; `release` on an absent/released token errors |
| `rule-success.ProcessTerminated`, `rule-failure.ProcessTerminated.1` | unit | `terminate_kills_child`: `terminate` on a retained token `TerminateProcess`es the child + frees the slot; on an absent token errors |
| (table exhaustion + leak) | unit | `process_table_bounded`: an 9th `spawnRetain` past the 8-slot table errors (no silent eviction); a connection close releases all retained handles (`MemReleaseAll`) — **recorded non-obligation:** the specific slot count (8) is an implementation detail, not a pinned case |
| `surface-provides.MemoryRuntime`, `surface-actor.MemoryRuntime` | integration | the five commands are reachable only over the wire boundary (the bridge); covered by the round-trip tests below |

## Device: peek / poke (the access path + the guards)

| Obligation | kind | Test |
|---|---|---|
| `rule-success.MemoryPeeked`, `rule-entity-creation.MemoryPeeked.1` | unit | `peek_process_tier_round_trip`: `spawnRetain` a child, `poke` a known pattern, `peek` it back — bytes match (process tier, CI-executable) |
| `entity-fields.PeekResult`, `entity-optional.PeekResult.process` | unit | `peek_result_shape`: `{process?, address, length, bytes_read, truncated}`; `process` is null on a pre-NT (tier-forced) peek, non-null on a token peek |
| `invariant.AddressIsWellFormed` **(SAFETY PIN #1)** | unit + **theft** | `mem_parse_u32_*`: `MemParseU32` accepts hex(`0x…`)/decimal in `[0,0xFFFFFFFF]`, rejects overflow/garbage/empty/trailing — the reason addresses are wire **strings** (a 32-bit address overflows the signed-int JSON parser) |
| `invariant.MemoryAccessRangeBounded` **(SAFETY PIN #2)** | unit + **theft** | `range_guard_*`: an access is admitted only when `length ≤ max_access_length` AND `address+length` does not wrap past `0xFFFFFFFF` (overflow-safe form, e.g. `addr=0xFFFFFFF0,len=0x100` rejected) — the single most critical arithmetic guard; property-tested natively |
| `config-default.max_access_length` | unit | `range_cap_default`: the cap defaults to 65536; a `length` above it is rejected (peek) / refused (poke) |
| `invariant.PreNtAccessGuarded` **(SAFETY PIN #3)** | unit (synthetic) + **skip-on-NT** | `region_accessible_decision`: given a synthetic `MEMORY_BASIC_INFORMATION`, the guard rejects non-committed (peek) / non-writable (poke) regions and clamps a spanning peek (`truncated`); the **live** arena/shared_vm load/store is **skipped-with-reason on NT** (Phase 6 hardware) |
| `invariant.RetainedTokenValid` **(SAFETY PIN #4)** | unit | `token_validity`: peek/poke/terminate/release succeed only against an `in_use`, `retained`-status slot whose token string-matches; released/terminated/forged/never-issued tokens are rejected; tokens are never reused (the monotonic seq) — **the peek/poke-after-release rejection traces to the `mem_access_valid` rule guard, not the invariant** |
| `rule-success.MemoryPoked`, `rule-entity-creation.MemoryPoked.1` | unit | `poke_process_tier_writes`: an armed, catalogued, in-range poke writes the bytes; `partial` reflects a `WriteProcessMemory` `ERROR_PARTIAL_COPY` prefix (process tier only) |
| `entity-fields.PokeResult`, `entity-optional.PokeResult.process` | unit | `poke_result_shape`: `{process?, address, length, bytes_written, partial}`; pre-NT `partial` is always false (whole-or-nothing) |
| `rule-failure.MemoryPoked.1` **(SAFETY PIN #7, device half)** | unit | `poke_requires_device_arm`: a `poke` with the device `/ALLOWMEMWRITE` arm **absent** is refused regardless of the request — the wire arm binds **every** client, incl. a direct one a bridge-only gate could not reach |
| `rule-failure.MemoryPoked.2` | unit | `poke_requires_valid_access`: a poke failing the range/token/region floor is refused (no partial mutation) |
| `rule-failure.MemoryPoked.3` + `invariant.PokeIsAuditedFailClosed` **(SAFETY PIN #6)** | unit | `poke_fail_closed_audit`: when the audit sink is unwritable, the poke is refused and **no memory is written** — no unlogged mutation, ever; a successful poke writes exactly one `AuditRecord` before the `PokeResult` |
| `entity-fields.AuditRecord` | unit | `audit_record_shape`: the per-poke line carries `{result-link, command, address, length, bytes_written}`, integer-only, ANSI, append-only |

## Device: tier detection + ready envelope

| Obligation | kind | Test |
|---|---|---|
| `enum-comparable.MemTier` | unit | `test_feat.c` `os_family_maps_to_tier`: is_nt→`process`, is_win9x→`arena`, is_win32s→`shared_vm`, else `none` |
| `features.mem` (ReadyShape prose) | unit | `test_serial.c` `ready_carries_mem_tier`: the ready `features.mem` is the tier string (`process` on the NT dev host), present always |
| (new wire string fields) | unit | `test_json.c` `parses_mem_string_fields`: `mem_addr`/`mem_len`/`mem_token` parse as strings (NOT the int path); unknown-key tolerance preserved |

## Bridge: the five tools + the first real capability prune (G1)

| Obligation | kind | Test |
|---|---|---|
| `rule-success.MemoryToolsRegistered`, `rule-entity-creation.MemoryToolsRegistered.1` | integration | `memory.rs` `memory_tools_registered`: on a memory-capable device, `tools/list` shows `win32_spawn_retain`/`peek`/`poke`/`terminate`/`release` with the spec'd `device_cmd`/`destructive`/`read_only` |
| (the **first real G1 prune** — carried unexercised since 5.0) | integration | `peek_pruned_when_mem_none`: with `mem: none`, `win32_peek`/`spawn_retain`/`terminate`/`release` are **absent** from `tools/list` (the prune→absence path, finally exercised); with `mem: process` they appear |
| `invariant.MemoryWriteToolRequiresOptIn` **(SAFETY PIN #7, bridge half)** | integration | `poke_requires_opt_in`: `win32_poke` is advertised only when `mem≠none` **AND** `--allow-memory-write` (the two-factor `mem_write` gate); absent when the operator did not opt in, even on a memory-capable device |
| `invariant.MemoryToolHintsAreHonest` | integration | `memory_tool_hints_honest`: spawn_retain/poke/terminate carry `destructive`; peek carries `read_only`; read_only excludes destructive |
| `entity-fields.DeviceCapabilities` (content: `allow_memory_write`) | proptest | `props.rs` `capability_satisfied_mem`: `satisfies("mem") = mem≠none`; `satisfies("mem_write") = mem≠none && allow_memory_write` (the two-factor); `allow_memory_write` threads from the `--allow-memory-write` operator flag |
| (tool→device relay mapping) | integration | `memory_tools_relay`: each tool round-trips to its `device_cmd` with name-mapped args (token/addr-as-string/data); a device `status:error` (bad token, unarmed, guard reject, audit-fail) → recoverable `isError`; peek `data_b64` rides structuredContent |

---

## The seven safety pins (consolidated)

| # | Invariant | Device test | Bridge test |
|---|---|---|---|
| 1 | `AddressIsWellFormed` | `mem_parse_u32_*` (+ theft) | addr passed as String, never re-typed to int |
| 2 | `MemoryAccessRangeBounded` | `range_guard_*` (+ theft) | — (device-enforced) |
| 3 | `PreNtAccessGuarded` | `region_accessible_decision` (synthetic) + skip-on-NT live | — |
| 4 | `RetainedTokenValid` | `token_validity` | bad-token call → isError |
| 5 | `SpawnRetainCommandIsCatalogued` | `spawn_retain_requires_catalogued` | — |
| 6 | `PokeIsAuditedFailClosed` | `poke_fail_closed_audit` | audit-fail → isError |
| 7 | `PokeRequiresBothArmingLayers` | `poke_requires_device_arm` (device `/ALLOWMEMWRITE`) | `poke_requires_opt_in` (bridge `--allow-memory-write`) |

## Recorded non-obligations (no test asserts these)

- **No live pre-NT raw-memory verification.** The `arena`/`shared_vm` load/store
  paths are un-exercisable on the NT dev host/CI; their integration tests
  skip-with-reason. Live correctness is **Phase 6 hardware** — do not assert
  pre-NT memory behaviour against Wine.
- **The bounded-table size (8) is an implementation detail**, not a pinned case;
  the test pins the *exhaustion behaviour* (error, no eviction) and the
  release-on-disconnect, not the number.
- **The LCG token suffix is hardening, not a security boundary.** Tests pin
  never-reuse + table-membership (`RetainedTokenValid`), not the RNG quality —
  the boundary is the membership check, not guess-resistance.
- **No interpretation of memory contents** — peek/poke move raw bytes via
  base64; UTF-8/codepage decoding of memory text is **5.4**, not a 5.3 test.
- **No `OpenProcess`-by-PID / cross-process reach** — the spawn-retain table is
  the sole `process`-tier target boundary; there is deliberately no test for
  attaching to a process we did not launch (it stays out, permanently).

## Floor

New: **≥14 device** (`test_mem_ops.c` ≥10 incl. the 5 device-side safety pins +
the lifecycle/table; `test_serial.c`/`test_feat.c`/`test_json.c` ≥4 for tier +
ready + wire fields) + **≥6 bridge** (`memory.rs` ≥5 incl. the first-real prune +
the two-layer poke gate; `props.rs` ≥1 for the two-factor `satisfies`) + the
theft host-pbt for the range guard + `MemParseU32`. Existing floors only grow
(`OBLIGATIONS-PHASE4.md` ≥163 device, `OBLIGATIONS-5.0.md` ≥20 bridge,
`OBLIGATIONS-5.1.md` ≥24 device/≥5 bridge, `OBLIGATIONS-5.2.md` ≥4 device/≥18
bridge).
