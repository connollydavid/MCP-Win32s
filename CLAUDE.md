# CLAUDE.md — AI Assistant Guide for MCP-Win32s

## Project Overview

MCP-Win32s is a **Model Context Protocol server for Win32 systems** that enables MCP clients (Claude Code, Claude Desktop) to interact with any Windows system from Windows 3.1 + Win32s 1.25a (1995) through Windows 11. It is a single Win32 console application (`win32shell.exe`) that runs unmodified across 30+ years of Windows versions.

**License:** Public Domain (Unlicense)

**Current status:** Phase 1 complete (test framework + JSON parser + CI). See Implementation Phases below.

## Repository Structure

```
MCP-Win32s/
├── .github/
│   └── workflows/
│       └── build-and-test.yml  # GitHub Actions CI (MinGW + Wine)
├── src/
│   ├── common.h                # Shared types (JsonCommand struct)
│   ├── json_parser.c           # JSON parsing + response building (~320 lines C89)
│   └── json_parser.h           # Parser/builder public API
├── tests/
│   ├── test_framework.h        # Minimal C89 test framework (header-only)
│   └── test_json.c             # 31 JSON parser unit tests
├── build.bat                   # VC++ 6.0 build script
├── build.sh                    # MinGW cross-compile build script
├── .gitignore                  # Ignores *.exe, *.o, *.obj
├── README.md                   # Technical specification (~3000 lines)
├── LICENSE                     # Unlicense (public domain)
└── CLAUDE.md                   # This file
```

### Planned (not yet created)

```
src/
├── win32shell.c       # Main program
├── file_ops.c/.h      # File read/write/list operations
├── serial.c/.h        # Serial port handling
├── tcp.c/.h           # TCP socket handling (Winsock 1.1)
└── named_pipes.c/.h   # Named pipe support (Win95+)
tests/
├── test_file_ops.c    # File operation tests
├── test_serial.c      # Serial port tests
└── run_all_tests.bat  # Test runner
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
- Verify with: `objdump -d win32shell.exe | grep -E 'cpuid|cmpxchg|bswap|rdtsc|fld|fst'` (must be empty).

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
cl /W3 /O2 /TC /G3 /FIXED:NO /BASE:0x10000 win32shell.c ^
   kernel32.lib user32.lib wsock32.lib
```

### Visual Studio 2022 (modern development)

```batch
cl /W4 /O2 /TC /std:c11 /arch:IA32 /FIXED:NO /BASE:0x10000 ^
   win32shell.c kernel32.lib user32.lib wsock32.lib
```

### MinGW-w64 (CI / cross-compile from Linux)

```bash
i686-w64-mingw32-gcc -O2 -std=c89 -march=i386 -mtune=i386 \
  -Wall -Werror -pedantic -Wdouble-promotion -Wfloat-equal \
  -Wl,--dynamicbase -Wl,--image-base,0x10000 \
  -o win32shell.exe win32shell.c \
  -lkernel32 -luser32 -lwsock32
```

### Building Tests (MinGW example)

```bash
i686-w64-mingw32-gcc -O2 -std=c89 -Wall -I../src \
  -o test_json.exe test_json.c ../src/json_parser.c -lkernel32
```

### Running Tests

```bash
# On Windows
test_json.exe

# On Linux CI (via Wine)
wine test_json.exe
```

## Code Conventions

### Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Global variables | `g_` prefix | `g_tests_run`, `g_tests_failed` |
| Functions | camelCase or Win32 style | `ExecuteCommand`, `ParseJson` |
| Constants/macros | UPPER_SNAKE_CASE | `MAX_PATH`, `GENERIC_READ` |
| Structs | PascalCase | `JsonCommand`, `TransportCaps` |

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

| Transport | Availability | API |
|-----------|-------------|-----|
| Serial (COM ports) | All Win32 systems | `CreateFileA("COM1:", ...)` |
| TCP (Winsock 1.1) | WfW 3.11+ with TCP/IP-32, Win95+ | `socket()`, `bind()`, `listen()`, `accept()` |
| Named Pipes | Win95+ only, **not** Win32s | `CreateFileA("\\\\.\\pipe\\name", ...)` |

Runtime detection with graceful fallback is required.

## Dependencies

### C-level (compile-time, all provided by the OS)

| Library | Purpose |
|---------|---------|
| `kernel32.dll` | Core Win32 API (file I/O, process control, memory) |
| `user32.dll` | User interface (`MessageBoxA`) |
| `wsock32.dll` | Winsock 1.1 (TCP/IP sockets) |

**No external C libraries.** The project produces a single standalone executable with zero DLL dependencies beyond what the OS provides.

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

## Implementation Phases

| Phase | Focus | Status |
|-------|-------|--------|
| 1 | Test framework + JSON parser + CI | **Complete** |
| 2 | Serial/file operations + tests | Not started |
| 3 | Command execution + protocol | Not started |
| 4 | MCP integration | Not started |
| 5 | Cross-platform testing | Not started |
| 6 | Documentation & polish | Not started |

**Note:** GitHub Actions CI was integrated into Phase 1 (not a separate phase). All subsequent phases inherit CI validation automatically.

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
