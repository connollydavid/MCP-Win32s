# 5.4 (full-range text-encoding subsystem) test obligations — propagated 2026-06-07

Derived with `allium plan` from `specs/encoding.allium` (new), diffed against the
pre-5.4 baseline (`a93cccf`, the 5.3 merge): **15 new structural obligations**,
all in `encoding.allium` (the whole file is new). The additive edits to the
existing specs add **0 structural obligations**:

- `wire-contract.allium` `ReadyShape` gains the `encoding` provenance tag
  (`utf8_manifest` | `utf8_via_w` | `utf8_from_cp`) — a prose `@invariant` change,
  which yields no structural obligation (the same as 5.3's `ReadyShape`/`mem`
  change). Traced to a bridge test that the `encoding` field is **surfaced and
  purely informational** (it never gates transcoding — the device already emits
  UTF-8).
- `mcp-protocol.allium` — **no change landed.** The planned `output_kind:"utf8"`
  edit was a no-op: `output_kind` is an unmodelled wire detail, so there is no
  construct to extend. The wire-safety it would have implied is pinned instead by
  `NeverEmitInvalidUtf8` on the device side (exercised by the exec-output
  transcode test). Documented in `wire-contract.allium`, not asserted structurally.

Nothing was removed from coverage; the floor only grows.

**Five of the seven safety pins ride prose `@invariant`s and so carry no
structural ID** — traced to concrete tests instead (the 5.3 pattern, where
`PokeRequiresBothArmingLayers` was a surface `@guarantee`). Only the two
expression-bearing top-level invariants — `NeverEmitInvalidUtf8` and
`StrictNarrowingRejectsUnrepresentable` — get an `invariant.*` obligation. The
codec/table/path-scan pins are `@invariant`s inside their `contract` blocks; they
are pinned by the matching theft property / exhaustive sweep / named fixture below
and anchored to the `contract-signature.*` obligation of their host contract.

The two conversion rules (`PathConverted`, `OutputConverted`) are
**host-runtime-triggered** (external trigger source — the
`allium.rule.unreachableTrigger` info on both is the accepted producer-blindness
baseline: catalog.allium carries 12 of the same class, memory-ops its memory
triggers). They have no surface, so their obligations are device-internal
behaviour tests, not surface-exposure tests.

## Testability boundary (CI parity — load-bearing)

The dev host (WSL interop) and CI (Wine) are **NT-family**, so the device's
conversion tier there is **`wide`** (the delay-loaded `-W` file/process APIs).
That tier is **live in CI** — the `-W` uplift is exercised, not skipped:

- **`wide` tier — LIVE under Wine.** `CreateFileW`/`FindFirstFileW`/`FindNextFileW`
  create + list + read a CJK-named file end-to-end; exec-output transcode is
  exercised via a helper child writing known console-CP bytes. This is the
  real-Win32-on-Wine path, the source of truth for the NT family.
- **`codepage` tier — pure logic tested EVERYWHERE; live narrowing is Phase 6.**
  The codec and the codepage↔Unicode **tables are OS-independent** (our own C89
  data), so the exhaustive round-trip + the non-bijection assertions + the codec
  properties run natively, under Wine, and on real hardware identically. The
  `EncOpenPath`/narrowing **decision** is pure and tested on an NT host by forcing
  the fallback (`FEAT_FORCE_NO_WIDE_FILEAPI`). The **live** `CreateFileA`-with-
  narrowed-bytes on real Win32s/Win9x is **Phase 6 hardware** (the 5.3
  arena/shared_vm skip-with-reason precedent) — do not assert it against Wine.
- **`manifest` tier — NOT exercisable under Wine/pre-1903 → skip-with-reason.**
  The Win10-1903+ `activeCodePage=UTF-8` runtime effect (`GetACP()==65001`) needs
  real Win10; under Wine/pre-1903 the manifest is inert. The manifest **embedding**
  is **CI-asserted** (exactly one `RT_MANIFEST` at ID 1 containing `activeCodePage`,
  beside the existing import-table greps); the runtime UTF-8 effect is **Phase 6
  hardware**.

The **pure** half (codec + tables + the path-separator scan) is factored out
behind `ENCODING_HOST_PURE` (the `MEM_OPS_HOST_PURE` / `build.sh` precedent) so it
goes through the theft host-PBT harness at 50k trials with ASan/UBSan, then is
mirrored on-target in `prop.h` at lower trial counts.

## Test kinds

- **unit (C89)** — new `tests/test_encoding.c` (the on-target codec mirror; the
  exhaustive per-codepage table round-trip; the documented non-bijection cases as
  named assertions; the strict-narrowing reject per tier; the DBCS-safe scan
  fixtures; the tier→behaviour mapping); `tests/test_feat.c` for the OS-family→tier
  mapping + the `-W` uplift probe (`has_wide_fileapi`/`has_wide_createprocess` +
  the force-fallback mask); `tests/test_serial.c` for the `encoding` ready-tag
  envelope.
- **prop (C89, on-target)** — `tests/prop.h` mirrors of the four codec properties
  at lower trial counts (proves they hold on the shipped C89/i386 code path).
- **prop (theft, host-native)** — new `tests/host/theft_encoding.c`: the four codec
  properties at **50k trials** with autoshrinking — the strongest pin on the
  off-by-malformed-byte class.
- **integration (Wine, `wide` tier)** — `tests/test_file_ops.c`/`test_serial.c`:
  a CJK-named file create+list+read through the `-W` uplift; exec-output transcode
  from a child's console CP → UTF-8.
- **CI assertion (build)** — exactly one `RT_MANIFEST`@ID1 with `activeCodePage`;
  the nine `-W` APIs are GetProcAddress-only (objdump import-table grep, extending
  the existing wsock32/FPU/486 greps).
- **integration (Rust)** — `bridge/tests/`: the passthrough reconciliation over the
  duplex mock device (the device's UTF-8 rides through unchanged; the `encoding`
  tag is informational; no `encoding_rs`/`oem_cp` dependency).

---

## Contracts: the codec, the tables, the path scan

| Obligation | kind | Test |
|---|---|---|
| `contract-signature.Utf8Codec.utf8_to_utf16`, `contract-signature.Utf8Codec.utf16_to_utf8` | unit + **theft** | `theft_encoding.c` + `test_encoding.c` `codec_*`: the hand-rolled C89 UTF-8↔UTF-16 codec implements both directions; pure (no Win32, no FP, no `CP_UTF8`); the four pinned properties below ride these signatures |
| `contract-signature.CodepageTables.codepage_to_unicode`, `contract-signature.CodepageTables.unicode_to_codepage` | unit | `test_encoding.c` `table_*`: our own embedded SBCS + four-CJK-DBCS tables implement both directions; deterministic + identical on every host (replaces `MultiByteToWideChar`/`WideCharToMultiByte`); the `representable` predicate backs the reverse direction |
| `contract-signature.PathScanning.find_separators` | unit | `test_encoding.c` `find_separators_*`: the device's own byte-wise separator scan; the DBCS-safe property below rides this signature |

### The codec pins (theft host-PBT — `tests/host/theft_encoding.c`, 50k trials)

| Pin (prose `@invariant`, no structural ID) | kind | Test |
|---|---|---|
| **`Utf8RoundTripFidelity`** | **theft** + prop.h | `codec_round_trip`: a well-formed UTF-16 code-unit sequence survives `utf8_to_utf16(utf16_to_utf8(units)) == units` (identity on the well-formed subset) |
| **`LossyDecodeIsTotal`** | **theft** + prop.h | `decode_total`: `utf8_to_utf16` never fails or loops on **arbitrary** bytes — a malformed maximal subpart yields exactly one U+FFFD and the scanner advances to the next lead byte (always makes progress). The single strongest pin; theft generates arbitrary byte garbage |
| **`NeverEmitInvalidUtf8`** (`invariant.NeverEmitInvalidUtf8`) | **theft** + prop.h + unit | `never_invalid`: every encode output is well-formed UTF-8 — a lone surrogate becomes U+FFFD, never a CESU-8/WTF-8 sequence. The wire-safety floor; also asserted on the outbound device path (exec-output transcode) |
| **`TruncationIsBoundaryClean`** | **theft** + prop.h | `truncation_clean`: when an output buffer is exhausted, the emitted prefix is itself whole — the cut falls on a code-point boundary, never inside a multi-byte UTF-8 sequence or a UTF-16 surrogate pair |

### The table pins (exhaustive on-target — `tests/test_encoding.c`)

| Pin | kind | Test |
|---|---|---|
| **`CodepageRoundTripsOnBijectiveSubset`** | unit (exhaustive) | `table_round_trip_<cp>`: for the bijective subset of **every baked code page**, `unicode_to_codepage(cp, codepage_to_unicode(cp, b)) == b` — **every byte** for SBCS, **every valid lead/trail pair** for the four DBCS pages; `codepage_to_unicode` is TOTAL (an undefined position → U+FFFD, never a failure) |
| **non-bijection cases** (the carved-out exceptions — `docs/charset-bijection.md`) | unit (named assertions) | `nonbijection_*`: each documented exception is a **named assertion** — undefined SBCS positions (cp1252 `0x81 0x8D 0x8F 0x90 0x9D` → U+FFFD); cp932 wave-dash (`0x8160` → the Microsoft-canonical **U+FF5E**, not JIS U+301C); NEC/IBM duplicate rows; PUA mappings (cp932/cp950 user areas). The test asserts the **canonical mapping is applied**, not that it is the "right" Unicode answer (vendor-divergent by nature — a recorded non-obligation) |

### The path-scan pin

| Pin | kind | Test |
|---|---|---|
| **`PathSeparatorScanIsDbcsSafe`** | unit (fixture) + integration | `dbcs_safe_scan`: a cp932 name ending in a DBCS char whose **trail byte is `0x5C`** — `find_separators` on the UTF-8 form never counts it as a path separator (a UTF-8 continuation byte `0x80–0xBF` can never be `0x5C`). The one backward scan (the device's own `GetModuleFileName` path) uses a `CharNext`-correct forward walk. The integration half: the `0x5C`-trail CJK file round-trips through the `wide` tier (create→list→read) without mis-splitting |

---

## Enums, the conversion entity, the rules

| Obligation | kind | Test |
|---|---|---|
| `enum-comparable.EncodingTier` | unit | `test_feat.c` `os_family_maps_to_tier`: is_win10_1903+→`manifest`, is_nt(pre-1903)→`wide`, is_win32s/is_win9x→`codepage`; the tier is comparable and drives `EncOpenPath` |
| `enum-comparable.ConversionStatus`, `enum-comparable.ConversionDirection` | unit | `test_encoding.c` `status_direction_comparable`: `ok`/`lossy`/`rejected`/`truncated` and `inbound`/`outbound` compare as the conversion-result fields |
| `entity-fields.TextConversion` | unit | `conversion_result_shape`: a conversion records `{direction, tier, codepage, source, result, status}` — the byte/code-unit forms and the outcome |
| `rule-success.PathConverted`, `rule-entity-creation.PathConverted.1` | unit + integration | `inbound_path_conversion`: an inbound agent UTF-8 path is prepared for a file API on the host's tier → a `TextConversion{direction:inbound,…}`; on the `codepage` tier an unrepresentable code point → `status:rejected` and **no file touched**; otherwise `ok` (or `lossy` if the source itself carried a replacement) |
| `rule-success.OutputConverted`, `rule-entity-creation.OutputConverted.1` | unit + integration | `outbound_output_conversion`: an OS-sourced name/listing/child-exec-output is rendered to the UTF-8 wire → a `TextConversion{direction:outbound,…}`; the result is always well-formed UTF-8; an undefined/garbage source byte → U+FFFD (`status:lossy`), never a failure |

## Expression invariants (the two with structural IDs)

| Obligation | kind | Test |
|---|---|---|
| `invariant.NeverEmitInvalidUtf8` **(SAFETY PIN — wire floor)** | **theft** + unit | `never_invalid` (above) on the codec; on the device, the outbound exec-output/listing transcode produces only well-formed UTF-8 (a `rejected` conversion produced no wire output, so it is exempt) |
| `invariant.StrictNarrowingRejectsUnrepresentable` **(SAFETY PIN — no wrong-file)** | unit (per tier) | `strict_narrowing_rejects`: on the **`codepage`** tier, narrowing a UTF-8 path with a code point the target page cannot represent → `status:rejected`, the device never substitutes `'?'` and never touches a different file. On the **`manifest`** and **`wide`** tiers the device reaches full Unicode, never narrows, and so **never rejects for this reason** — asserted as the negative case per tier |

---

## The seven safety pins (consolidated)

| # | Pin | Structural ID | Host (theft) | Device | Notes |
|---|---|---|---|---|---|
| 1 | `Utf8RoundTripFidelity` | — (`@invariant`) | `codec_round_trip` | prop.h mirror | identity on the well-formed subset |
| 2 | `LossyDecodeIsTotal` | — (`@invariant`) | `decode_total` | prop.h mirror | never fails/loops on arbitrary bytes |
| 3 | `NeverEmitInvalidUtf8` | `invariant.NeverEmitInvalidUtf8` | `never_invalid` | exec-output transcode | the wire-safety floor |
| 4 | `TruncationIsBoundaryClean` | — (`@invariant`) | `truncation_clean` | prop.h mirror | cut on a code-point boundary |
| 5 | `StrictNarrowingRejectsUnrepresentable` | `invariant.StrictNarrowingRejectsUnrepresentable` | — | `strict_narrowing_rejects` (per tier) | no silent `'?'`; no wrong-file |
| 6 | `CodepageRoundTripsOnBijectiveSubset` | — (`@invariant`) | (tables are pure — host too) | `table_round_trip_<cp>` + `nonbijection_*` | exhaustive; documented exceptions carved out |
| 7 | `PathSeparatorScanIsDbcsSafe` | — (`@invariant`) | (scan is pure — host too) | `dbcs_safe_scan` (cp932 `0x5C`-trail) | structural: scan UTF-8 before narrowing |

## Bridge: the passthrough reconciliation

| Obligation | kind | Test |
|---|---|---|
| `wire-contract` `ReadyShape` `encoding` tag (prose `@invariant`, no structural ID) | integration | `encoding_tag_informational`: the ready `features.encoding` is surfaced (`utf8_manifest`/`utf8_via_w`/`utf8_from_cp`) and is **purely informational** — it never selects bridge transcoding; the device's text rides through unchanged regardless of its value |
| (device guarantees valid UTF-8 → the bridge stops transcoding) | integration | `passthrough_validates_utf8`: a device payload that is already UTF-8 round-trips through the bridge byte-for-byte; `from_utf8_lossy` survives only as belt-and-suspenders; **file-content base64 is still never transcoded** (raw bytes) |
| (no new dependency) | build | `no_encoding_dep`: the bridge adds **no** `encoding_rs`/`oem_cp` crate — the prior plan's crate set is dropped (the device owns all transcoding) |

## Recorded non-obligations (no test asserts these)

- **No live Win10-`manifest`-UTF-8 runtime verification.** The `activeCodePage`
  runtime effect (`GetACP()==65001`) needs real Win10 1903+; under Wine/pre-1903 it
  is inert. The manifest **embedding** is CI-asserted; the runtime UTF-8 effect is
  **Phase 6 hardware** — do not assert it against Wine.
- **No live pre-NT `codepage`-tier narrowing verification.** The
  `CreateFileA`-with-narrowed-bytes path on real Win32s/Win9x is Phase 6. The
  **tables** and the `EncOpenPath`/narrowing **decision** are pure-tested
  everywhere (exhaustive round-trip; force-fallback on an NT host) — do not assert
  the live syscall outcome against Wine.
- **Unicode normalization (NFC/NFD) is out of scope.** We move code points
  faithfully; we do not normalize. No test asserts normalized equality.
- **Code pages beyond the baked SBCS + four-CJK-DBCS set are not obligations.**
  They accrete later as generated tables.
- **The non-bijection *policy choice* at an ambiguous row is the documented choice,
  not a "correct Unicode" claim.** The test asserts the Microsoft-canonical mapping
  is **applied** (e.g. cp932 `0x8160`→U+FF5E), not that it is the right answer —
  vendor-divergence is the carved-out exception, not a defect.
- **File read/write DATA is never transcoded.** Only paths, listings, and
  exec-output are interpreted as text; the data path stays raw bytes + base64. No
  test asserts data-path transcoding.

## Floor

New: **≥18 device** (`test_encoding.c` ≥13 = the codec on-target mirror ≥4 + the
exhaustive table round-trip ≥5 across SBCS+the four DBCS + the non-bijection named
assertions ≥6 + the strict-narrowing reject ≥3 + the DBCS-safe scan ≥2;
`test_feat.c` ≥2 for the tier mapping + the `-W` uplift probe; `test_serial.c` ≥1
for the `encoding` ready tag; `test_file_ops.c`/`test_serial.c` ≥2 for the live
`wide`-tier CJK round-trip + exec-output transcode) + **≥3 bridge**
(`encoding_tag_informational`, `passthrough_validates_utf8`, `no_encoding_dep`) +
the **theft host-pbt** (the four codec properties — round-trip / lossy-total /
never-invalid / truncation-clean — @ 50k trials) + the **CI build assertions**
(one `RT_MANIFEST`@ID1 with `activeCodePage`; the nine `-W` APIs GetProcAddress-
only). Existing floors only grow (`OBLIGATIONS-PHASE4.md` ≥163 device,
`OBLIGATIONS-5.0.md` ≥20 bridge, `OBLIGATIONS-5.1.md` ≥24 device/≥5 bridge,
`OBLIGATIONS-5.2.md` ≥4 device/≥18 bridge, `OBLIGATIONS-5.3.md` ≥14 device/≥6
bridge).
