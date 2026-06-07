# 5.2 (build-toolchain subsystem) test obligations — propagated 2026-06-07

Derived with `allium plan` from `specs/toolchains.allium` (new),
`specs/mcp-bridge.allium` and `specs/wire-contract.allium`, each diffed
against the pre-5.2 baseline (`fce5ce8`): **33 new obligations** — 8 in
`toolchains.allium` (the whole file is new) and 25 in `mcp-bridge.allium`,
0 in `wire-contract.allium` (its `ReadyShape` change is a prose `@invariant`,
which yields no structural obligation). Nothing was removed; the floor only
grows.

One existing obligation changes content, not identity:
`entity-fields.DeviceCapabilities` gains `toolchain_registration` (the operator
opt-in for runtime registration).

One behavioural obligation has no structural ID because it rides a prose
`@invariant`: the device's ready message carries a `toolchains` array (one
`{vendor, command, version}` per detected build command, empty when none
installed — `wire-contract.allium` `ReadyShape`). It is traced here to
`ToolchainDetected` and tested on the device side.

The build tools are **composed**, not relayed: each runs the existing
catalogued `exec` command and parses its output — so `device_cmd` is null and
**no new device wire command is added** (unlike 5.1's copy/move/mkdir/rmdir).
The only device-side additions are the startup toolchain probe + ready array
and the Open Watcom catalog entries.

## Test kinds

- **unit (C89)** — `tests/test_toolchain_probe.c` (new) for the device probe;
  `tests/test_serial.c` for the ready-message envelope integration.
- **prop (C89)** — `tests/prop.h` on-target properties (fixed seeds) where the
  device logic has an OS-independent core. No theft host target is anticipated:
  the probe is a thin `exec`-banner wrapper; the version-string extraction and
  the four diagnostic dialects are **bridge**-side (Rust), where they are
  proptested with `proptest`.
- **integration (Rust)** — `bridge/tests/*.rs` over the duplex mock device;
  **proptest** — `bridge/tests/props.rs` (the schema round-trip, the four
  dialect parsers, the argv-emission injection pin, the regex ReDoS bound);
  **conformance** — MCP Inspector CLI in CI.

## Architecture seam (resolved interpretation, confirmed in implement)

`DetectedToolchain` lives in `toolchains.allium`'s device-detection section and
is carried in the ready message, but a C89/Win32s device has **no regex engine**
and does not hold the (bridge-side) author-supplied definitions. So the
observable contract splits:

- **Device:** probes each catalogued build command at startup, captures its
  version **banner**, and emits one ready-`toolchains` entry per command.
- **Bridge:** applies each definition's `version_regex` to the banner to derive
  the canonical version string, and matches it against the enumerated,
  build-number-granular support matrix (`toolchain_supported`) to decide which
  tools to generate.

The propagate-stage obligations are mapped to whichever side the behaviour is
**observable** on; the exact locus of version extraction is an implement seam
the implement stage pins.

## Non-obligations (do NOT write tests asserting these)

- **No instruction-set policy.** The build tools enforce **no**
  architecture/CPU/FP/SIMD restriction on user builds — the project's
  i386/no-FP/C89/Win32s constraints bind only `mcp-w32s.exe`'s own source. A
  test that asserted a user flag like `/arch:SSE2`, `/G6`, `-bt=nt`, or an FPU
  target is rejected would be **wrong**. The only enforced boundary is
  injection-safety (catalogued command + neutralised tail).
- **Per-flag mappings are data, not pinned cases.** The typed-param→flag/
  directive templates are author-supplied definition content. Their correctness
  is covered by the schema round-trip + the argv-injection pin + one worked
  per-vendor example — not by an exhaustive hardcoded flag table (the point of
  5.2 is that flags are data).
- **The support matrix contents are data.** `toolchain_supported` is exercised by
  representative supported/unsupported version cases, not enumerated exhaustively.

---

## Device: toolchain detection (the ready `toolchains` array)

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.ToolchainDetected`, `rule-entity-creation.ToolchainDetected.1`, `entity-fields.DetectedToolchain` | unit | `probe_detects_installed_toolchain`: with a catalogued build command present (a stub on `PATH`/in the catalog), the probe emits one detection carrying `{vendor, command, version-banner}`; an uninstalled command yields none |
| (ready-array shape; `ReadyShape` prose; empty case) | wire | `test_serial.c` `ready_lists_detected_toolchains`: the ready envelope's `features.toolchains` is an array of `{vendor, command, version}`; **empty array when no build toolchain is installed** (not absent, not null) |
| `entity-fields.DetectedToolchain` (full-version retention) | unit | `probe_retains_full_version_banner`: the captured `version` is the **full** banner string (the build number that distinguishes service packs, e.g. `12.00.8804`), not a truncated major version |

The Open Watcom catalog entries (`wcc386`/`wlink`/`wlib`/`wasm`) and the MSVC
build commands (`cl`/`link`/`lib`/`ml`) being present in the device catalog is
what lets `DefinitionCommandsAreCatalogued` hold for the two built-ins; the
**pinning** test for that invariant is bridge-side (below).

## Bridge: the definition schema (toolchains-as-data)

| Obligations | Kind | Test |
|---|---|---|
| `entity-fields.ToolchainDefinition`, `entity-fields.RoleSpec`, `enum-comparable.ToolRole`, `enum-comparable.DefinitionSource` | integration + proptest | `definition_schema_roundtrips`: a `ToolchainDefinition` JSON deserializes `{name, vendor, source, version_probe, version_regex}` and per-role `{command, args-template, diagnostic}`; `source ∈ {built_in, config, registered}`, `role ∈ {compile, link, lib, assemble}`; the **two built-in definitions (MSVC, Open Watcom) load** and parse |
| `invariant.DefinitionCommandsAreCatalogued` | integration | **`definition_commands_must_be_catalogued`** (safety pin #1): every `RoleSpec.command` in every loaded definition (built_in/config/registered) is a device-catalogued command; the two built-ins name only catalogued commands; a definition naming an **uncatalogued** command is **rejected** — authorship can never introduce a new executable |

## Bridge: generated build tools + version gating

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.BuildToolGenerated`, `rule-entity-creation.BuildToolGenerated.1` | integration | `build_tools_generated_for_supported`: a detected **and supported** toolchain yields one `win32_<vendor>_<role>` tool per role, each with `device_cmd: null`, `destructive: true`, `read_only: false`; the name encodes the vendor (e.g. `win32_msvc_compile`, `win32_watcom_link`) |
| `rule-failure.BuildToolGenerated.1`, `rule-failure.BuildToolGenerated.2` | integration | **`unsupported_toolchain_generates_no_tool`**: a toolchain detected but whose version is **not** in the support matrix (`toolchain_supported` false), or with no matching ready session/role spec, produces **no** build tool — the first real exercise of the 5.0 G1 capability-prune (a generated tool is absent from `tools/list`) |

## Bridge: meta tools + runtime registration (hybrid authoring)

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.MetaToolsRegistered`, `rule-entity-creation.MetaToolsRegistered.1` | integration + conformance | `meta_tools_registered`: `win32_list_toolchains` and `win32_register_toolchain` are registered off `DeviceCapabilities.created`; `tools/list` shows both with 2020-12 schemas |
| `invariant.DiscoveryToolIsReadOnly` | integration | `discovery_tool_is_read_only`: `win32_list_toolchains` carries `read_only: true` (it returns the schema + definitions + guide and mutates nothing) |
| `invariant.RegistrationRequiresOptIn`, `rule-failure.ToolchainRegistered.1` | integration | **`registration_requires_opt_in`** (safety pin #2): `win32_register_toolchain` always names a required capability, so (with the 5.0 `AdvertisedToolsAreCapable`) it is advertised **only** when the operator set `toolchain_registration`; a registration attempt when the tool is unadvertised is **refused** |
| `entity-fields.DeviceCapabilities` (content-changed: +`toolchain_registration`) | integration | `capabilities_carry_toolchain_registration`: the cap parses from the device ready features; **default off** when absent |
| `rule-success.ToolchainRegistered`, `rule-entity-creation.ToolchainRegistered.1` | integration | `registration_creates_definition`: with opt-in on, a `win32_register_toolchain` call creates a `source: registered` definition that generates further build tools; the registered definition is **still** bound by `DefinitionCommandsAreCatalogued` (a registered definition naming an uncatalogued command is refused — covered by safety pin #1) |

## Bridge: build results — the compile-error-≠-tool-error pin

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.BuildResultProduced`, `rule-entity-creation.BuildResultProduced.1`, `entity-fields.BuildResult`, `surface-actor.BuildRuntime`, `surface-provides.BuildRuntime` | integration | `build_result_produced_on_ok`: once a build tool's `exec` call is `ok`, the bridge parses stdout+stderr into a `BuildResult{success, exit_code, dialect}` carried as `structuredContent`; `success = (exit_code == 0)`; `dialect` names the parser used (e.g. `msvc_cc`, `watcom_link`); `BuildOutputParsed` fires only when the call is `ok` |
| `rule-failure.BuildResultProduced.1` (+ 5.0 `invariant.SuccessResultsAreStructured`) | integration | **`compile_error_is_not_a_tool_error`** (semantic pin): a **nonzero** compiler exit still rides an `isError: false` result with structured `{success:false, exit_code, diagnostics}`; `isError: true` occurs **only** when the toolchain fails to *run* (the generic recoverable path) — a failed compile is data, not a tool error |

## Bridge: diagnostics — the four dialect parsers

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.BuildDiagnosticRecorded`, `rule-entity-creation.BuildDiagnosticRecorded.1`, `entity-fields.Diagnostic`, `enum-comparable.Severity`, `surface-actor.DiagnosticRuntime`, `surface-provides.DiagnosticRuntime` | integration + proptest | `diagnostic_recorded_msvc_cc`: the `msvc_cc` dialect parses `lexer.c(50): error C2065: 'tre': undeclared identifier` → `{file:"lexer.c", line:50, column:null, severity:error, code:"C2065", message:…}`; severity keywords map onto `error\|warning\|fatal\|info` |
| `entity-optional.Diagnostic.line` | proptest | `diagnostic_msvc_link`: the `msvc_link` dialect (distinct grammar) parses `obj : error LNK2019: …` and `LINK : fatal error LNK1104: …` → `line:null, column:null` (link-level errors carry no source position) |
| (watcom_cc dialect) | proptest | `diagnostic_watcom_cc`: parses `err.c(9): Error! E1011: Symbol 'x' has not been declared` → `{file, line:9, severity:error, code:"E1011"}` |
| (watcom_link dialect) | proptest | `diagnostic_watcom_link`: parses bare 4-digit `wlink` codes (e.g. `1014 stack segment not found`) → `{code:"1014", message, line:null}` |
| `entity-optional.Diagnostic.column` | proptest | `diagnostic_column_optional`: modern `cl /diagnostics:column` form `lexer.c(50,20): error C2065: …` → `column:20`; the default (VC6 / Open Watcom / modern-cl-default) form → `column:null` |

The four dialect parsers run line-by-line over **both** stdout and stderr (the
stream split is behavioural/unpinned).

## Bridge: safety pins (gate-bypass surfaces — weed hammers these)

These are the build subsystem's safety boundary. The first two are the
gate-bypass dimension the weed/review stages must actively try to defeat; the
third is the ReDoS surface flagged in PHASE5.md.

| Pin | Kind | Test |
|---|---|---|
| `invariant.DefinitionCommandsAreCatalogued` (safety pin #1, above) | integration | `definition_commands_must_be_catalogued` — a definition (incl. a runtime-registered one) cannot drive an uncatalogued program |
| **`BuildArgvIsCatalogued`** (prose SECURITY INVARIANT in `mcp-bridge.allium`) | integration + proptest | **`build_argv_is_injection_safe`** (safety pin #3): every build tool reaches the device only through a catalogued `exec` whose `argv[0]` is the role's `command`, with flags/`wlink` directives drawn **solely** from the definition's typed-param template. A crafted call-time value (a source path like `x&calc`, a `wlink`-directive-looking token, an embedded space/quote) is passed as a **single** argv element via the `{}` slot — never re-split, never shell/directive-interpreted, never introducing a second command or flag. Asserted against the device's `CatalogValidateArgs` + `ShellTailNeutralised` gate (no bypass on either the shell route **or** the Open Watcom directive route) |
| diagnostic-regex ReDoS bound (PHASE5.md: agent-supplied regexes are a ReDoS surface) | proptest | **`diagnostic_regex_is_bounded`** (safety pin #4): an author-/agent-supplied `diagnostic.regex` is bounded in match time (no catastrophic backtracking); an adversarial pattern (nested quantifiers) against adversarial compiler output stays within the bridge's bound — a runaway pattern is rejected or time-capped, never hangs the bridge |

## Bridge: end-to-end + conformance

| Obligations | Kind | Test |
|---|---|---|
| (data-flow chain: detection → generation → call → exec → parse) | integration | `build_pipeline_end_to_end`: a detected+supported toolchain generates a build tool; calling it sends the composed catalogued `exec` to the mock device; the mock's `{exit_code, stdout, stderr}` reply is parsed into a `BuildResult` + `Diagnostic`s in `structuredContent`, `isError:false` — exercises the full chain through the 5.0 `ToolCallSucceeded`/`BuildRuntime`/`DiagnosticRuntime` seams |
| (tool-shape conformance) | conformance | MCP Inspector CLI in CI: `tools/list` shows the generated `win32_<vendor>_<role>` tools + `win32_list_toolchains`/`win32_register_toolchain` with 2020-12 `inputSchema` (`additionalProperties:false`) and honest `destructive`/`readOnly` hints; `tools/call` on a build tool round-trips to `{content, structuredContent, isError:false}` |

## Floor

33 new obligations → **≥4 new device tests** (probe unit + ready-array wire) and
**≥18 new bridge tests** (schema round-trip + the four dialect parsers + tool
generation/prune + meta tools + registration/opt-in + the build-result and
diagnostic rules + the four safety pins + the end-to-end chain + the extended
conformance run), each citing its obligation IDs.

The four named safety/semantic pins are non-negotiable:
`definition_commands_must_be_catalogued`, `build_argv_is_injection_safe`,
`diagnostic_regex_is_bounded`, and `compile_error_is_not_a_tool_error`.

Combined with the existing floors — `tests/OBLIGATIONS-PHASE4.md` (≥163 device),
`bridge/OBLIGATIONS-5.0.md` (≥20 bridge), `tests/OBLIGATIONS-5.1.md` (≥24
device, ≥5 bridge) — the floor only grows: every pre-existing test keeps
passing.
