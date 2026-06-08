# 5.5 (listCommands discovery verb) device test obligations — propagated 2026-06-08

Derived with `allium plan` from `specs/catalog.allium` and
`specs/mcp-protocol.allium`, diffed against the pre-5.5 baseline (`470ded9`, the
5.4 merge): **9 new device obligations** — 5 in `catalog.allium`, 4 in
`mcp-protocol.allium`. The implementation does **not** exist yet — these are the
forward checklist the implement stage traces each test to.

Two existing obligation IDs change **content, not identity** (no new ID; the
existing dispatch test grows the new verb):

- `rule-success.CommandDispatch` / `rule-success.CommandUnknown` — the additive
  whitelist `{echo, read, write, list, delete, copy, move, mkdir, rmdir, exec,
  ptyExec, listCommands}` gains `listCommands`; formerly `listCommands` was an
  `"unknown command"`. (`ptyExec` and `exec` were already in the set from Phase 4.)

`listCommands` is a **read-only discovery verb**: it asks the loaded catalog for
its entries and returns them. It spawns nothing, mutates nothing, and **bypasses
the exec gate entirely** — there is no `argv`, so no `CatalogValidateArgs` /
`args_allowed` path. This is the device counterpart of the bridge's
`win32_list_commands` relay (bridge/OBLIGATIONS-5.5.md).

Test kinds (reuse the existing device harnesses — do not rebuild):

- **unit (C89)** — `tests/test_catalog.c` against the catalog listing/serialiser
  (the `CatalogListing` snapshot + the per-entry JSON flatten); `tests/test_json.c`
  for the `listCommands` request parse + the listing response shape.
- **wire (C89)** — `tests/test_serial.c` main-loop integration over the mock
  transport: `{"cmd":"listCommands","id":..}` round-trips through dispatch to an
  ok `{"status":"ok","commands":[...]}` response.
- **prop (C89, on-target)** — `tests/prop.h` for the listing-reflects-catalog
  property if the entry set is parameterisable; otherwise an exhaustive unit
  sweep over the loaded catalog (the listing is a pure read over fixed data — no
  OS-independent generative logic beyond JSON escaping, already host-fuzzed).
- **conformance (Wine)** — the bridge↔device Wine round-trip is covered by the
  bridge's Inspector conformance job (`win32_list_commands` → device
  `listCommands` → listing back); **noted here, not duplicated** on the device side.

## Catalog: the listing snapshot

| Obligation | kind | Test |
|---|---|---|
| `entity-fields.CatalogListing` | unit | `test_catalog.c` `listing_shape`: a `CatalogListing` records `{catalog, request_id, entry_count}`; `request_id` round-trips the request id; `entry_count` is an integer |
| `entity-relationship.Catalog.entries` | unit | `test_catalog.c` `entries_navigable`: `Catalog.entries` navigates to the loaded `CatalogEntry` set (`catalog = this`) — the navigable relation the listing reads (each entry has `{name, kind, supports_win32s}`) |
| `rule-success.CatalogListed`, `rule-entity-creation.CatalogListed.1` | unit | `test_catalog.c` `list_commands_reports_loaded`: a `ListCommandsRequested(request_id)` produces a `CatalogListing{catalog, request_id, entry_count: catalog.entry_count}` and emits `CatalogListingReady` carrying the serialised listing. On the wire each entry flattens to `{name, description, builtin, destructive, flags}` (the host's per-entry payload, **not** modelled `CatalogEntry` state — wire-shape verified, see non-obligations); assert correct JSON escaping of each field |
| `invariant.ListingReflectsLoadedCatalog` **(SAFETY-ADJACENT PIN)** | unit (+ exhaustive sweep) | `test_catalog.c` `listing_reflects_loaded_catalog`: `listing.entry_count == catalog.entry_count` — the listing is the **whole** loaded set, **nothing added or hidden**, regardless of whether `enforced` is on (the catalog is consulted for routing even under `/UNSAFE`). Drive with several loaded catalogs (≥30-entry default, a small one, an unenforced one) and assert the reported count and the serialised entry set match the loaded entries exactly |
| (gate-bypass non-interference) **(SAFETY PIN)** | unit | `list_commands_never_invokes_gate`: `listCommands` produces only the read-only `CatalogListing` — it **never** mutates the catalog and **never** invokes the exec gate (no `argv`, no `CatalogValidateArgs` / `args_allowed` call). Assert the gate rules are untouched: a `listCommands` against an **enforced** catalog returns the full listing **without** any `ExecRejected` / args-validation path firing, and the catalog state (`entry_count`, `enforced`) is unchanged after the call |

## Protocol: dispatch + the listCommands request/response

| Obligation | kind | Test |
|---|---|---|
| `rule-success.ListCommandsCommand`, `rule-failure.ListCommandsCommand.1` | wire | `test_serial.c` `dispatch_list_commands`: `{"cmd":"listCommands","id":..}` reaches its handler via the main loop (the additive-verb dispatch — formerly `"unknown command"`); emits `ListCommandsRequested(request_id: cmd.id)`. The `rule-failure` arm: a non-`listCommands` verb does **not** trigger this rule |
| `rule-success.CommandDispatch` / `rule-success.CommandUnknown` (content: `listCommands` added to the whitelist) | wire | `test_serial.c` (existing `unknown_command` test must still pass): `listCommands` is now **dispatched, not rejected** — the prior "unknown command" path for it is gone; a still-unknown verb (e.g. `bogus`) is still `"unknown command"` |
| `rule-success.ListCommandsResult`, `rule-entity-creation.ListCommandsResult.1` | unit + wire | `test_json.c` `build_list_commands_response` (shape) + `test_serial.c` `list_commands_round_trip` (main-loop): `CatalogListingReady(request_id, listing)` produces a `Response{status:ok, key:"commands", value:listing}` correlated to the request id — `{"id":..,"status":"ok","commands":[ {name,description,builtin,destructive,flags}, ... ]}` |
| `entity-fields.Command` (content: parse of `cmd:"listCommands"`) | unit | `test_json.c` `parse_list_commands_command`: `{"cmd":"listCommands","id":..}` parses to a `JsonCommand` with the `listCommands` verb and no argv path (mirrors the existing `parse_list_command` / `parse_exec_command` convention); unknown-key tolerance preserved |

## Recorded non-obligations (no device test asserts these)

- **The per-entry wire fields `{description, builtin, destructive, flags}` are
  NOT modelled `CatalogEntry` state.** They are wire-shape fields the host
  serialises per entry (like the exec envelope's flattened keys in
  mcp-protocol.allium). Their presence/escaping is verified at the
  **C/conformance level** (the `list_commands_reports_loaded` JSON assertion +
  the Inspector conformance round-trip), not as a spec-modelled entity-field
  obligation — there is no `entity-fields.CatalogEntry` obligation covering them.
- **The bridge↔device Wine round-trip is not duplicated on the device side.**
  `win32_list_commands` → device `listCommands` → listing back is covered by the
  bridge's Inspector conformance job (bridge/OBLIGATIONS-5.5.md); the device side
  asserts the request parse, the dispatch, the listing snapshot, and the response
  shape — the cross-module trigger chain (`ListCommandsRequested` →
  `CatalogListed`/`CatalogListingReady` → `ListCommandsResult`) is exercised by
  the `list_commands_round_trip` wire test, which wires mcp-protocol↔catalog in
  one main-loop pass.
- **The catalog *contents* (which commands a given catalog whitelists) are an
  operator/data concern, not a 5.5 obligation.** The listing reports whatever is
  loaded; the test pins `entry_count` fidelity and round-trip shape, not a
  specific command set (the existing `load_count_at_least_30` covers the default
  catalog size).

## Floor

New: **≥6 device** (`test_catalog.c` ≥4 = `listing_shape` + `entries_navigable`
+ `list_commands_reports_loaded` + `listing_reflects_loaded_catalog` +
`list_commands_never_invokes_gate`; `test_json.c` ≥2 =
`parse_list_commands_command` + `build_list_commands_response`;
`test_serial.c` ≥2 = `dispatch_list_commands` / `list_commands_round_trip`, plus
the existing `unknown_command` test which must still pass with `listCommands` now
dispatched). Existing floors only grow — every pre-5.5 device test keeps passing
(`OBLIGATIONS-PHASE4.md` ≥163, `OBLIGATIONS-5.1.md` ≥24, `OBLIGATIONS-5.2.md` ≥4,
`OBLIGATIONS-5.3.md` ≥14, `OBLIGATIONS-5.4.md` ≥18 device); specifically the
`test_serial.c` unknown-command test must still pass with `listCommands` now
dispatched, not rejected (the 5.1 copy/move/mkdir/rmdir precedent).
