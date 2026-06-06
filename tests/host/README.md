# Host-native theft PBT harness (`tests/host/`)

Phase 4 deliverable (`plan/PHASE4.md` -> "theft host-side PBT harness").

These are **property-based tests** for the OS-independent, pure-logic modules,
run with the vendored [theft](../../vendor/theft/) library and its
**autoshrinking** generators. Unlike the rest of the test suite they are built
**natively on Linux with `gcc -std=c99`** (POSIX) — *not* MinGW, *not* C89,
*not* under Wine. This is the one documented exception (see the project
`CLAUDE.md`, "Property-based testing: two frameworks, two jobs"): theft is
C99 + POSIX and can never target Win32s, so it hammers portable modules at high
trial counts to find bugs fast, with minimal counter-examples. The same
properties are mirrored in `tests/prop.h` at lower trial counts to prove they
hold on the actual C89/i386 build.

The **modules under test stay strict C89 and are compiled unmodified** into the
host build. Nothing in `src/` is patched.

## Files

| File | Module under test | Properties |
|------|-------------------|------------|
| `theft_base64.c`  | `src/base64.c`      | roundtrip `decode(encode(x))==x`; encoded-alphabet validity; length formula `4*ceil(n/3)` |
| `theft_json.c`    | `src/json_parser.c` | `ParseJsonCommand` never crashes/over-reads on arbitrary bytes (returns exactly 0/1); `parse(BuildJsonResponse(...))` value roundtrip (escape∘unescape identity) |
| `theft_argv.c`    | `src/argv.c`        | `ArgvJoin` output, re-tokenized by a reference `CommandLineToArgvW` tokenizer in the harness (Q8 rules), reproduces the original argv byte-for-byte |
| `theft_catalog.c` | `src/catalog.c`     | `CatalogLookup` totality; loaded entries are named; `CatalogValidateArgs` rejects unknown flags; glued `/A:v` and split `/A v` validate identically for arg-taking options |
| `win32_shim.h`, `windows.h` | — | Minimal Win32 shim so `src/catalog.c` (which `#include <windows.h>` and uses `CreateFileA`/`ReadFile`/`CloseHandle` + `lstr*`) compiles natively. See below. |

Trial counts: **50000** per property (fixed seeds, reproducible). The
heaviest property (`theft_json` fuzz) measured ~9 s wall-clock at 50000, well
under the 60 s budget, so it is kept at 50000.

## Building and running

The shipped C89 modules compile clean under the host flags as-is. theft's own
sources need `-D_POSIX_C_SOURCE=199309L` (timespec/nanosleep) and link `-lm`
(built-in float generators); build them separately so the harness keeps
`-Werror`:

```sh
# from the repository root
SAN="-fsanitize=address,undefined"
HFLAGS="-std=c99 -O1 -g $SAN -Wall -Werror -Ivendor/theft/inc -Itests/host -Isrc"

# theft objects once (no -Werror for vendored code; not patched)
mkdir -p build/host/theft
for f in vendor/theft/src/*.c; do
  gcc -std=c99 -O1 -g $SAN -Wall -D_POSIX_C_SOURCE=199309L \
      -Ivendor/theft/inc -c "$f" -o "build/host/theft/$(basename "$f" .c).o"
done

# one harness = one binary
gcc $HFLAGS tests/host/theft_base64.c  src/base64.c      build/host/theft/*.o -lm -o build/host/theft_base64
gcc $HFLAGS tests/host/theft_json.c    src/json_parser.c build/host/theft/*.o -lm -o build/host/theft_json
gcc $HFLAGS tests/host/theft_argv.c    src/argv.c        build/host/theft/*.o -lm -o build/host/theft_argv
gcc $HFLAGS tests/host/theft_catalog.c src/catalog.c     build/host/theft/*.o -lm -o build/host/theft_catalog

build/host/theft_base64
build/host/theft_json
build/host/theft_argv
build/host/theft_catalog        # run from repo root so catalog/win32-commands.json resolves
```

Each binary exits `0` when every property passes, non-zero otherwise. Under
`-fsanitize=address,undefined`, any over-read, overflow, or out-of-bounds in
a module under test fails the run.

> ASan/UBSan note: theft's PRNG (`vendor/theft/src/theft_random.c:79`) prints a
> `shift exponent 64 is too large` UBSan diagnostic from inside the vendored
> library. It is benign and not in any module under test.

### Catalog path

`theft_catalog` loads `catalog/win32-commands.json`. It tries, in order:
`$MCP_CATALOG`, `./catalog/win32-commands.json`, `../catalog/win32-commands.json`.
Run it from the repo root (or set `MCP_CATALOG`).

## The Win32 shim (`win32_shim.h` + `windows.h`)

`src/base64.c`, `src/json_parser.c`, and `src/argv.c` are pure ANSI C and
compile natively with no help. `src/catalog.c` is not: it `#include <windows.h>`
and uses `CreateFileA`/`ReadFile`/`CloseHandle` for file loading plus the Win32
ANSI string helpers `lstrcpynA`/`lstrcmpA`/`lstrcmpiA`/`lstrlenA`.

Rather than touch `src/`, the harness provides the **minimal** surface
`catalog.c` actually references:

- `windows.h` (this directory) is a one-line shim that includes `win32_shim.h`.
  Because `-Itests/host` is on the include path and Linux has no real
  `windows.h`, `catalog.c`'s `#include <windows.h>` resolves to it.
- `win32_shim.h` provides:
  - the scalar types/macros used (`HANDLE`, `DWORD`, `BOOL`, `LPVOID`, ...,
    `GENERIC_READ`, `OPEN_EXISTING`, `INVALID_HANDLE_VALUE`, `TRUE`/`FALSE`);
  - `CreateFileA`/`ReadFile`/`CloseHandle` implemented over `<stdio.h>`
    (read-only `fopen`/`fread`/`fclose`), faithful to the EOF-as-`read==0`
    contract `catalog.c` relies on;
  - `lstrcpynA` (Win32 bounded-copy semantics) and `lstrcmpA`/`lstrcmpiA`/
    `lstrlenA` mapped to `strcmp`/`strcasecmp`/`strlen`.

The shim is host-build-only and never linked into `mcp-w32s.exe`.
