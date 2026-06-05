# CLAUDE.md — AI Assistant Guide for MCP-Win32s

## Project Overview

MCP-Win32s is a **Model Context Protocol server for Win32 systems** that enables MCP clients (Claude Code, Claude Desktop) to interact with any Windows system from Windows 3.1 + Win32s 1.25a (1995) through Windows 11. It is a single Win32 console application (`mcp-w32s.exe`) that runs unmodified across 30+ years of Windows versions.

**License:** Public Domain (Unlicense)

**Current status:** Phase 2 complete (file operations + base64 + echo + 87 tests incl. PBT). See Implementation Phases below.

## Repository Structure

```
MCP-Win32s/
├── .claude/
│   └── settings.json           # Enables the allium@juxt-plugins plugin
├── .github/
│   └── workflows/
│       └── build-and-test.yml  # GitHub Actions CI (MinGW + Wine)
├── specs/
│   ├── file-ops.allium         # Allium 3 spec: file read/write/list/delete
│   └── mcp-protocol.allium     # Allium 3 spec: command dispatch + responses
├── src/
│   ├── base64.c                # Base64 encode/decode (integer-only)
│   ├── base64.h                # Base64 public API
│   ├── common.h                # Shared types (JsonCommand struct)
│   ├── file_ops.c              # File read/write/list/delete
│   ├── file_ops.h              # File ops public API
│   ├── json_parser.c           # JSON parsing + response building (~334 lines C89)
│   ├── json_parser.h           # Parser/builder public API
│   ├── mcp-w32s.c              # Main executable (protocol loop, dispatch)
│   ├── serial.c                # Serial port init + config builders
│   └── serial.h                # Serial/transport API
├── tests/
│   ├── prop.h                  # Minimal C89 property-based testing framework
│   ├── test_base64.c           # 14 base64 encode/decode tests
│   ├── test_file_ops.c         # 10 file operation tests
│   ├── test_framework.h        # Minimal C89 test framework (header-only)
│   ├── test_json.c             # 31 JSON parser unit tests
│   ├── test_pbt_base64.c       # 4 property-based tests (4000 random trials)
│   └── test_serial.c           # 28 serial + main loop tests
├── vc6/
│   ├── mcp-w32s.dsw            # VC++ 6.0 workspace
│   └── mcp-w32s.dsp            # VC++ 6.0 project (Debug + Release)
├── vendor/
│   └── theft/                  # Vendored PBT library with autoshrinking (host-side only)
├── AGENTS.md                   # Condensed agent guide (constraints + layout)
├── PLAN.md                     # Phase plans; Phase 3 is the detailed exec spec
├── build.bat                   # VC++ 6.0 build script
├── build.sh                    # MinGW cross-compile build script
├── .gitignore                  # Ignores *.exe, *.o, *.obj, VC6 output
├── README.md                   # Technical specification (~3000 lines)
├── LICENSE                     # Unlicense (public domain)
└── CLAUDE.md                   # This file
```

### Planned (not yet created)

```
src/
├── transport.c/.h     # Transport abstraction: vtable interface + backend registry (foundational, pre-Phase 3)
├── tcp.c/.h           # TCP backend, Winsock 1.1, runtime-probed (foundational, pre-Phase 3)
├── named_pipes.c/.h   # Named pipe backend (Win95+) — Phase 4+
└── (Phase 3 modules)  # feat, exec_ops, pty_exec, argv, binfmt, catalog, ready — see PLAN.md
specs/
├── transport.allium   # Foundational: backend-agnostic transport lifecycle
├── process-ops.allium # Phase 3: Process/ExecResult/Capabilities
├── catalog.allium     # Phase 3: command catalog
└── (distill backfill) # base64, json-parser, serial — see Specification Workflow
catalog/
└── win32-commands.json # Phase 3: machine-readable command docs + whitelist
tests/
├── mock_transport.c/.h # In-memory transport backend for asserting response bytes
└── host/              # Phase 3: theft-based host-native PBT harness
```

## Hard Technical Constraints

These constraints are non-negotiable. Violating any of them breaks compatibility with the primary target platform (Windows 3.1 + Win32s 1.25a).

### Language

- **C89 only.** No C99, C11, or C++ features.
- All variable declarations must be at the top of blocks (no mixed declarations and code).
- No `//` comments — use `/* */` only (C89 compliance).
- No `inline`, `restrict`, `_Bool`, `long long`, or other C99+ keywords.

### Floating Point

- **Absolute ban on floating-point operations.** All arithmetic must be integer-only.
- Modern CPUs (Intel Ivy Bridge 2012+) deprecate FCS/FDS registers, breaking old FPU exception handling.
- Compiler flags `-Wdouble-promotion -Wfloat-equal` enforce this on MinGW.

### CPU Target

- **i386 (80386) minimum.** No 486+ or Pentium instructions.
- Prohibited instructions: `CPUID`, `CMPXCHG`, `BSWAP`, `XADD`, `RDTSC`, `CMPXCHG8B`, `CMOVcc`, all SSE/SSE2/MMX.
- Use `-march=i386 -mtune=i386` (MinGW) or `/G3` (VC++ 6.0) or `/arch:IA32` (VS2022).
- Verify with: `objdump -d mcp-w32s.exe | grep -E 'cpuid|cmpxchg|bswap|rdtsc|fld|fst'` (must be empty).

### Win32 API

- **Win32s 1.25a API subset only** (February 1995 baseline).
- **ANSI APIs only:** `CreateFileA`, `CreateProcessA`, `MessageBoxA` — never `W` (Unicode) variants.
- **Winsock 1.1 only:** Link `wsock32.lib`, never `ws2_32.lib`.
- **No:** Shell32, advapi32, COM/OLE, MFC/ATL, modern networking (WinHTTP/WinINet).
- **No threading.** Win32s does not support multithreading. Single-threaded only.
- **No async I/O.** Synchronous operations only.

### Linker

- **`/FIXED:NO`** (or MinGW equivalent) — must include relocation info (Win32s requirement).
- **`/BASE:0x10000`** — Win32s uses shared memory space; default 0x400000 is unavailable.

### Memory

- 16MB per application limit on Win32s.
- Use `malloc`/`free` for portable code, `GlobalAlloc`/`GlobalFree` for Win32s-specific code.
- No C++ `new`/`delete`. No RAII.

## Build Commands

### Visual C++ 6.0 (reference retro build)

```batch
cl /W3 /O2 /TC /G3 /Isrc /FIXED:NO /BASE:0x10000 ^
   src\mcp-w32s.c src\serial.c src\json_parser.c ^
   kernel32.lib user32.lib wsock32.lib
```

### Visual Studio 2022 (modern development)

```batch
cl /W4 /O2 /TC /std:c11 /arch:IA32 /Isrc /FIXED:NO /BASE:0x10000 ^
   src\mcp-w32s.c src\serial.c src\json_parser.c ^
   kernel32.lib user32.lib wsock32.lib
```

### MinGW-w64 (CI / cross-compile from Linux)

```bash
i686-w64-mingw32-gcc -O2 -std=c89 -march=i386 -mtune=i386 \
  -Wall -Werror -pedantic -Wdouble-promotion -Wfloat-equal \
  -Wl,--dynamicbase -Wl,--image-base,0x10000 \
  -Isrc -o mcp-w32s.exe \
  src/mcp-w32s.c src/serial.c src/json_parser.c \
  -lkernel32 -luser32 -lwsock32
```

### Building Tests (MinGW example)

```bash
# JSON parser tests
i686-w64-mingw32-gcc -O2 -std=c89 -Wall -Isrc \
  -o tests/test_json.exe tests/test_json.c src/json_parser.c -lkernel32

# Serial + main loop tests
i686-w64-mingw32-gcc -O2 -std=c89 -Wall -Isrc -DTEST_BUILD \
  -o tests/test_serial.exe tests/test_serial.c \
  src/mcp-w32s.c src/serial.c src/json_parser.c \
  -lkernel32 -luser32 -lwsock32
```

### Running Tests

```bash
# On Windows
tests\test_json.exe
tests\test_serial.exe

# On Linux CI (via Wine)
wine tests/test_json.exe
wine tests/test_serial.exe
```

### Running Tests Locally (native GCC, for quick iteration)

```bash
gcc -std=c89 -Wall -Werror -pedantic -Isrc \
  -o test_json tests/test_json.c src/json_parser.c && ./test_json
```

## Code Conventions

### Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Global variables | `g_` prefix | `g_tests_run`, `g_tests_failed` |
| Functions | camelCase or Win32 style | `ExecuteCommand`, `ParseJson` |
| Constants/macros | UPPER_SNAKE_CASE | `MAX_PATH`, `GENERIC_READ` |
| Structs | PascalCase | `JsonCommand`, `TransportConfig` |

### Strings

- Use `lstrlen`, `lstrcpy`, `lstrcat` (Win32s ANSI string functions).
- Fixed-size `char` arrays preferred over dynamic allocation.
- Base64 encoding for all binary data in JSON payloads.
- No `wchar_t`, no Unicode.

### Error Handling

- Win32 API errors via `GetLastError()`.
- Explicit handle checks: `if (hFile == INVALID_HANDLE_VALUE)`.
- No exceptions (not available in C89 or Win32s).
- Return codes: 0 = failure, nonzero = success (Win32 convention).

### File I/O

- `CreateFileA` / `ReadFile` / `WriteFile` for all I/O (files, COM ports, pipes).
- `FindFirstFileA` / `FindNextFileA` for directory listing.

### Includes

```c
#include <windows.h>    /* Win32 API (monolithic) */
#include <stdio.h>      /* ANSI C I/O */
#include <stdlib.h>     /* ANSI C utilities */
#include <string.h>     /* ANSI C string functions */
#include "json_parser.h" /* Project headers — quoted, relative */
```

No C++ headers (`<iostream>`, `<vector>`, etc.) ever.

## Protocol

- JSON-over-newline-delimited text.
- Hand-coded JSON parser (no external libraries — they won't compile on VC++ 6.0).
- Request format: `{"cmd":"...", "id":"...", "path":"...", ...}`
- Response format: `{"id":"...", "status":"ok|error", ...}`
- Binary data transmitted as Base64 within JSON fields.

## Transport Layers

| Transport | Availability | I/O primitives |
|-----------|-------------|-----|
| Serial (COM ports) | All Win32 systems | `CreateFileA("COM1:", ...)` + `ReadFile`/`WriteFile` |
| TCP (Winsock 1.1) | WfW 3.11+ with TCP/IP-32, Win95+ | `socket`/`bind`/`listen`/`accept` + **`recv`/`send`** |
| Named Pipes | Win95+ only, **not** Win32s | `CreateFileA("\\\\.\\pipe\\name", ...)` + `ReadFile`/`WriteFile` |

Runtime detection with graceful fallback is required.

### Transport abstraction (vtable, not HANDLE)

A `SOCKET` is **not** a Win32 file handle on Win32s/Win9x — `ReadFile`/`WriteFile` cannot drive a socket; you must use `recv`/`send`. So the protocol core must never touch a raw `HANDLE`. All I/O goes through a backend-agnostic interface (`src/transport.{c,h}`):

- A `Transport` is a struct of function pointers (`read`/`write`/`close`, optional `accept` for server backends) plus backend-private state (a `union` of `HANDLE`/`SOCKET`/`void*`).
- Backends register in a table (`{kind, name, probe, open}`); the core selects/auto-detects via the registry. This is the single seam for **future backends** — TCP now, then UDP/HTTP-3 (QUIC), then exotic message/RDMA transports — added without changing the core.
- Newline-JSON framing (`LineBuffer`) lives **above** the transport, so any reliable ordered byte backend works unchanged. Message-oriented backends set a flag to bypass framing.
- Network-capable backends (TCP, future QUIC/RDMA) are **runtime-loaded** (`LoadLibraryA`/`GetProcAddress`), never statically imported — otherwise the binary fails to load on hosts lacking the DLL (e.g. `wsock32.dll` on bare Win32s). CI asserts `objdump -p mcp-w32s.exe | grep -i wsock32` is empty.

This layer is **foundational work that lands before Phase 3** (exec responses flow over it). See PLAN.md "Transport Abstraction Layer".

## Dependencies

### C-level (compile-time, all provided by the OS)

| Library | Purpose |
|---------|---------|
| `kernel32.dll` | Core Win32 API (file I/O, process control, memory) |
| `user32.dll` | User interface (`MessageBoxA`) |
| `wsock32.dll` | Winsock 1.1 (TCP/IP sockets) |

**No external C libraries in the shipped binary.** The project produces a single standalone executable with zero DLL dependencies beyond what the OS provides.

### Vendored (test-time only, never linked into mcp-w32s.exe)

| Library | Location | Purpose |
|---------|----------|---------|
| theft | `vendor/theft/` | Host-native property-based testing with autoshrinking (C99/POSIX — Linux test harness only) |

### Python-level (modern MCP bridge side)

```
pyserial    # Serial port access
mcp         # Model Context Protocol SDK
```

### Build tools

| Tool | Purpose |
|------|---------|
| Visual C++ 6.0 | Reference retro compiler |
| MinGW-w64 | Cross-compile from Linux for CI |
| Visual Studio 2015+ | Modern Windows development |
| Wine | Run Win32 binaries on Linux for CI testing |

## CI/CD

GitHub Actions runs on **every push and pull request** (`.github/workflows/build-and-test.yml`):

1. Install MinGW-w64 and Wine on Ubuntu
2. Build with strict C89/i386 flags: `-std=c89 -march=i386 -mtune=i386 -Wall -Werror -pedantic -Wdouble-promotion -Wfloat-equal`
3. Run unit tests via Wine
4. Verify no FPU instructions in application object code (`objdump -d json_parser.o | grep fld|fst`)
5. Verify no 486+ instructions in application object code (`objdump -d json_parser.o | grep cpuid|cmpxchg|bswap`)
6. Upload compiled artifacts

**Strategy: CI from day one.** Every commit must pass the full pipeline. Binary instruction verification is done on application `.o` files only (not CRT startup code, which contains MinGW runtime FPU/atomic instructions that don't affect Win32s compatibility).

### Running Tests Locally

```bash
# Linux (MinGW cross-compile + Wine)
./build.sh test

# Linux (native GCC, for quick iteration)
gcc -std=c89 -Wall -Werror -pedantic -Isrc -o test_json tests/test_json.c src/json_parser.c && ./test_json

# Windows (VC++ 6.0)
build.bat test
```

## Specification & Test Workflow (Allium + theft)

Behaviour is specified in [Allium](https://juxt.github.io/allium/) (`specs/*.allium`, language version 3) **before** it is implemented. The Allium plugin (`allium@juxt-plugins`, enabled via `.claude/settings.json`) provides six skills. Every phase passes through this lifecycle, and each skill has a defined job:

| Stage | Skill | When | Output |
|-------|-------|------|--------|
| 1. Discover | `/allium:elicit` | Phase planning — turn phase goals and open questions into draft entities/rules through structured Q&A | Draft spec content |
| 2. Specify | `/allium:tend` | ALL spec writing and editing — new specs, refinements, syntax fixes, migrations. Never hand-edit `.allium` files outside tend | Valid `specs/*.allium` (`allium check` clean) |
| 3. Derive tests | `/allium:propagate` | Before implementation — generate the test obligations the specs imply | Obligation list: unit + property + state-machine tests |
| 4. Implement | (normal coding) | Code to the spec; every test traces to a propagated obligation | `src/*.c` + `tests/*.c` |
| 5. Audit | `/allium:weed` | Before marking a phase Complete — find spec↔code drift | Drift report; zero drift is the completion gate |
| 6. Backfill | `/allium:distill` | Whenever code exists without a spec — reverse-engineer one | New `specs/*.allium` |

`/allium:allium` is the language reference for any syntax or semantics question.

**Current spec coverage:** `file-ops.allium`, `mcp-protocol.allium`.
**Known gaps (distill targets, scheduled in Phase 3):** `base64`, `json_parser`, `serial` have implementations but no specs.

### Property-based testing: two frameworks, two jobs

| Framework | Where it runs | Role |
|-----------|--------------|------|
| `tests/prop.h` (homegrown, C89) | On the target binary — Wine in CI, real Win32s in Phase 5 | Properties that must hold on the shipped C89 code path; fixed seeds, no shrinking |
| `vendor/theft` (vendored, C99/POSIX) | Host-native Linux build only (`gcc -std=c99`, no Wine) | Deep generative testing with **autoshrinking** for OS-independent modules (`base64`, `json_parser`, `argv`, `catalog`) |

theft can never compile for the Win32s target (it is C99 + POSIX) — that is by design. It hammers portable pure-logic modules natively at high trial counts, and its autoshrinker reduces failing inputs to minimal counterexamples. Properties propagated from a spec are implemented in theft first (find bugs fast), then mirrored in `prop.h` at lower trial counts (prove they hold on the actual C89/i386 build).

**Status:** theft is vendored but not yet wired into `build.sh`/CI — wiring it in (`tests/host/`, `./build.sh host-pbt`, CI step) is a Phase 3 deliverable. See PLAN.md.

## Implementation Phases

| Phase | Focus | Status |
|-------|-------|--------|
| 1 | Test framework + JSON parser + CI + serial init + main exe | **Complete** |
| 2 | File operations + base64 + echo + 87 tests incl. PBT | **Complete** |
| 3 | Command execution + catalog + feature uplift + theft harness + spec backfill (detailed plan in PLAN.md) | **Spec'd — in progress** |
| 4 | MCP integration | Not started |
| 5 | Cross-platform testing | Not started |
| 6 | Documentation & polish | Not started |

**Note:** GitHub Actions CI was integrated into Phase 1 (not a separate phase). All subsequent phases inherit CI validation automatically. Phases 3+ additionally inherit the Allium lifecycle gates above: tend-written specs before code, propagate-derived tests, weed-clean before Complete.

**Foundational work before Phase 3:** the transport abstraction (`src/transport.{c,h}` + TCP backend) makes network a first-class peer of serial and must land before command execution, since exec's ready message and stdout/stderr responses flow over the transport. It is not a numbered sub-phase — see PLAN.md "Transport Abstraction Layer". It follows the same Allium + theft rigor.

## Common Mistakes to Avoid

1. **Using C99+ features** — `for (int i = 0; ...)` won't compile on VC++ 6.0. Declare variables at block top.
2. **Using `//` comments** — Not valid C89. Use `/* */`.
3. **Using Unicode APIs** — `CreateFileW` does not exist in Win32s. Always use `A` suffix.
4. **Linking ws2_32** — Win32s has Winsock 1.1 only. Link `wsock32`.
5. **Using floating-point** — Banned entirely. Use integer arithmetic.
6. **Forgetting `/FIXED:NO /BASE:0x10000`** — Binary won't load on Win32s without these.
7. **Using 486+ instructions** — `CPUID`, `CMPXCHG`, `BSWAP` etc. will crash on a 386.
8. **Using threads** — Win32s is single-threaded only.
9. **Dynamic memory without cleanup** — No RAII; every `malloc` needs a matching `free`.
10. **Using `printf` format specifiers beyond C89** — No `%zu`, `%lld`, etc.

## Key Design Decisions

- **Win32s 1.25a** chosen over 1.30/1.30c for stability (1.30c crashes on QEMU/VMware).
- **Base address 0x10000** because Win32s occupies the default 0x400000 region.
- **No floating-point** because modern CPUs deprecate FCS/FDS, breaking old FPU code.
- **JSON hand-parser** because no external JSON library compiles on VC++ 6.0.
- **Base64 for file transfers** to avoid encoding issues across different code pages.
- **DBCS-aware string handling** required for CJK systems (`IsDBCSLeadByte`, `CharNext`).
