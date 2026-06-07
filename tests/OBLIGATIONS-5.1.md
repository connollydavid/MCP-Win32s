# 5.1 (API-first file ops) test obligations — propagated 2026-06-07

Derived with `allium plan` from `specs/file-ops.allium`,
`specs/mcp-protocol.allium` and `specs/mcp-bridge.allium`, diffed against
the pre-change baseline: **83 new obligations** (49 file-ops, 28
protocol, 6 bridge). Three existing obligation IDs change content, not
identity: `entity-fields.Command` (gains `dest`), `entity-fields.Tool`
(gains `device_cmd`/`destructive`/`read_only`), `entity-fields.Directory`
(gains `status`/`is_empty`).

Test kinds:

- **unit (C89)** — `tests/test_file_ops.c` against the new
  `file_ops.h` functions (`FileOpCopy`/`FileOpMove`/`FileOpMakeDir`/
  `FileOpRemoveDir`), `tests/test_json.c` for parsing.
- **wire (C89)** — `tests/test_serial.c` main-loop integration over the
  mock transport: full request→envelope round-trips.
- **prop (C89)** — `tests/prop.h` on-target properties (fixed seeds).
  No theft host targets: the new ops are thin Win32 API wrappers with no
  OS-independent logic (the parser's totality is already host-fuzzed).
- **integration (Rust)** — `bridge/tests/integration.rs` over the duplex
  mock device; **proptest** — `bridge/tests/props.rs`;
  **conformance** — MCP Inspector CLI in CI.

Recorded intentional gaps are NOT obligations (no tests assert them):
re-used paths after delete (host succeeds; model is silent),
implementation-defined error vocabulary beyond the five pinned reasons,
MoveFileA's directory-rename tolerance.

## Device: copy

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.FileCopySuccess`, `rule-failure.FileCopySuccess.{1,2}`, `entity-fields.FileCopyResult`, `transition-{rejected,terminal}.FileCopyResult.status` | unit | `copy_creates_dest`: copy src→missing dest succeeds; dest readable; src still present. Failure arms (src missing / dest present) covered by the two tests below |
| `rule-success.FileCopySourceMissing`, `rule-failure.FileCopySourceMissing.1`, `when-presence.FileCopyResult.error_reason` | unit | `copy_source_missing_errors`: reason is exactly `"file not found"` |
| `rule-success.FileCopyDestExists`, `rule-failure.FileCopyDestExists.{1,2}` | unit | `copy_dest_exists_errors`: **the fail-if-exists pin** — reason exactly `"file exists"`, dest content untouched. Also covers src=dest (same path → dest exists) |
| (FileCopySuccess content fidelity) | prop | `copy_preserves_content`: random binary buffers write→copy→read, dest byte-identical to src |

## Device: move

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.FileMoveSuccess`, `rule-failure.FileMoveSuccess.{1,2}`, `entity-fields.FileMoveResult`, `transition-{rejected,terminal}.FileMoveResult.status` | unit | `move_renames`: move src→missing dest succeeds; dest readable with src's content; **src gone** (read fails) |
| `rule-success.FileMoveSourceMissing`, `rule-failure.FileMoveSourceMissing.1`, `when-presence.FileMoveResult.error_reason` | unit | `move_source_missing_errors`: reason exactly `"file not found"` |
| `rule-success.FileMoveDestExists`, `rule-failure.FileMoveDestExists.{1,2}` | unit | `move_dest_exists_errors`: MoveFileA rename semantics — reason exactly `"file exists"`, both files untouched |

## Device: mkdir / rmdir / Directory lifecycle

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.MakeDirSuccess`, `rule-failure.MakeDirSuccess.1`, `entity-fields.MakeDirResult`, `transition-{rejected,terminal}.MakeDirResult.status`, `transition-edge.Directory.missing.present` | unit | `mkdir_creates`: mkdir on a missing path succeeds; list of parent shows the new dir |
| `rule-success.MakeDirAlreadyExists`, `rule-failure.MakeDirAlreadyExists.1`, `when-presence.MakeDirResult.error_reason` | unit | `mkdir_existing_errors`: reason exactly `"directory exists"` |
| (spec comment pin: single level only) | unit | `mkdir_no_recursive_create`: mkdir `a\b\c` with `a\b` missing **fails** (any reason) — guards against an implementer "helpfully" adding mkdir -p |
| `rule-success.RemoveDirSuccess`, `rule-failure.RemoveDirSuccess.{1,2}`, `entity-fields.RemoveDirResult`, `transition-{rejected,terminal}.RemoveDirResult.status`, `transition-edge.Directory.present.deleted` | unit | `rmdir_empty_succeeds`: rmdir on an empty dir succeeds; subsequent list of parent omits it |
| `rule-success.RemoveDirNotEmpty`, `rule-failure.RemoveDirNotEmpty.{1,2}` | unit | `rmdir_nonempty_errors`: **the non-empty refusal pin** (no recursive delete) — reason exactly `"directory not empty"`, dir and contents untouched |
| `rule-success.RemoveDirNotFound`, `rule-failure.RemoveDirNotFound.1`, `when-presence.RemoveDirResult.error_reason` | unit | `rmdir_missing_errors`: reason exactly `"directory not found"` |
| `rule-success.DirectoryCreatedMissing`, `transition-terminal.Directory.status`, `transition-rejected.Directory.status` | unit (reachability) | `mkdir_then_rmdir_lifecycle`: full walk missing→present→deleted on one path; rmdir again errors (deleted is terminal — "directory not found") |

## Device: wire protocol (dispatch + envelopes)

| Obligations | Kind | Test |
|---|---|---|
| `entity-fields.Command` (extended), parse of `dest` | unit | `test_json.c` `parse_copy_command` / `parse_move_command`: `{"cmd":"copy","id":..,"path":..,"dest":..}` populates `JsonCommand.dest`; absent `dest` parses empty |
| `rule-success.{Copy,Move,Mkdir,Rmdir}Command`, `rule-failure.{Copy,Move,Mkdir,Rmdir}Command.1`, `rule-entity-creation.{Copy,Move,Mkdir,Rmdir}Command.1` | wire | `test_serial.c`: each of the four commands round-trips through the main loop to its handler (dispatch whitelist extension — formerly these were `"unknown command"`) |
| `rule-success.{Copy,Move,Mkdir,Rmdir}Success`, `rule-entity-creation.{Copy,Move,Mkdir,Rmdir}Success.1` | wire | ok envelopes: `{"id":..,"status":"ok","message":"copied"}` / `"moved"` / `"created"` / `"removed"` |
| `rule-success.{Copy,Move,Mkdir,Rmdir}Error`, `rule-entity-creation.{Copy,Move,Mkdir,Rmdir}Error.1` | wire | error envelopes: `{"id":..,"status":"error","error":<reason>}` carrying the pinned reasons (one representative per command, e.g. copy→`"file exists"`, rmdir→`"directory not empty"`) |

## Bridge: the file tool surface

| Obligations | Kind | Test |
|---|---|---|
| `rule-success.FileToolsRegistered`, `rule-entity-creation.FileToolsRegistered.1` | integration + conformance | `file_tools_all_advertised`: `tools/list` over the duplex contains exactly the eight `win32_*` file tools (plus the 5.0 echo tool), each with a 2020-12 inputSchema; Inspector CLI confirms in CI |
| `invariant.FileToolHintsAreHonest` | integration | `file_tool_hints_are_honest`: per-tool annotations match the table — destructive: write_file/delete_file/move_file/remove_dir; read-only: read_file/list_dir; copy_file/make_dir neither; no tool both |
| `invariant.ShellFileBuiltinsNotExposed` | integration | `shell_builtins_not_exposed`: no advertised tool named `dir`/`type`/`del`/`copy`/`ren`/`md`/`rd` — the supersession pin |
| `invariant.FileToolsAreDirectRelays`, `entity-optional.Tool.device_cmd`, `entity-fields.Tool` (extended) | integration | `file_tools_relay_to_device_cmds`: calling each tool sends exactly its device command with name-mapped args — `win32_copy_file{source,dest}` → `{"cmd":"copy","path":source,"dest":dest}` observed at the mock device (one call per tool; the copy/move two-path mapping asserted explicitly) |
| (cross-module scenario: device reply → MCP result, 5.0 generic IDs) | integration | `file_tool_device_error_is_iserror`: mock device replies `{"status":"error","error":"file exists"}` to a copy → `isError: true` with that text; mock ok `message:"copied"` → success with structuredContent |

## Floor

83 obligations → **≥24 new device tests** (unit + wire + prop) and
**≥5 new bridge tests** (integration + the extended conformance run),
each citing its obligation IDs. Combined with the existing floors
(`tests/OBLIGATIONS-PHASE4.md` ≥163; `bridge/OBLIGATIONS-5.0.md` ≥20)
the floor only grows: every pre-existing test keeps passing —
specifically the `test_serial.c` unknown-command test must still pass
with the four new commands now dispatched, not rejected.
