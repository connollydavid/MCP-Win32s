# AGENTS.md — MCP-Win32s

## What This Is

MCP server as a single Win32 console app (`mcp-w32s.exe`) running unmodified from Windows 3.1 + Win32s 1.25a (1995) through Windows 11. Protocol: newline-delimited JSON over serial port.

## Hard Constraints (Non-Negotiable)

Violating any of these breaks Windows 3.1 / Win32s compatibility.

- **C89 only**: No C99+ features. No `//` comments (use `/* */`). Variables declared at top of blocks, never mixed with code.
- **No floating-point**: All integer arithmetic. Compiler flags `-Wdouble-promotion -Wfloat-equal` enforce this.
- **i386 CPU minimum**: No 486+ instructions (`CPUID`, `CMPXCHG`, `BSWAP`, etc.). Use `-march=i386 -mtune=i386`.
- **Win32s API subset**: ANSI APIs only (`CreateFileA`, never `W` suffix). Winsock 1.1 only (`wsock32.lib`, never `ws2_32`). No Shell32, advapi32, COM, threads, or async I/O.
- **Linker**: Always `/FIXED:NO /BASE:0x10000` (MinGW: `-Wl,--dynamicbase -Wl,--image-base,0x10000`). Omitting these prevents loading on Win32s.

## Source Layout

```
src/
├── base64.c/h        # Base64 encode/decode (integer-only)
├── common.h          # Shared types (JsonCommand struct, size constants)
├── file_ops.c/h      # File read/write/list/delete (ANSI APIs)
├── json_parser.c/h   # Hand-coded JSON parser + response builder
├── serial.c/h        # Serial port init + command-line parser
└── mcp-w32s.c        # Main exe: protocol loop, dispatch, main()

tests/
├── prop.h            # Minimal C89 property-based testing framework
├── test_base64.c     # 14 base64 encode/decode tests
├── test_file_ops.c   # 10 file operation tests
├── test_framework.h  # Header-only test framework (TEST_CASE, TEST_ASSERT, RUN_TEST)
├── test_json.c       # 31 JSON parser tests
├── test_pbt_base64.c # 4 property-based tests (4000 random trials)
└── test_serial.c     # 28 serial + main loop tests
```

## Building

### MinGW (Linux / CI — primary workflow)

```bash
./build.sh           # Build all
./build.sh test      # Build + run tests via Wine
```

### Quick iteration (native GCC, JSON tests only)

```bash
gcc -std=c89 -Wall -Werror -pedantic -Isrc \
  -o test_json tests/test_json.c src/json_parser.c && ./test_json
```

Serial tests need MinGW cross-compile (they link Win32 APIs).

### VC++ 6.0 (retro)

```bash
build.bat           # Build all
build.bat test      # Build + run tests
```

## Testing

- Test framework (`tests/test_framework.h`) is header-only with `TEST_CASE(name)`, `TEST_ASSERT(cond, msg)`, `RUN_TEST(name)`, `print_test_summary()`.
- `main()` returns `g_tests_failed` as exit code.
- Serial tests compile `mcp-w32s.c` with `-DTEST_BUILD` to exclude `main()` and expose `ProcessBuffer`/`ProcessCommand` for testing.
- CI runs FPU/486 instruction verification on compiled `.o` files (not CRT).

## Adding New Code

- New source files go in `src/`, tests in `tests/`.
- Any new `.c` file must be added to `build.sh`, `build.bat`, and CI workflow.
- Follow naming: globals `g_`, functions camelCase, constants UPPER_SNAKE_CASE.
- Use `lstrlen`/`lstrcpy`/`lstrcat` for Win32 string ops; `strcmp`/`strcpy` for ANSI C.
- Every `malloc` gets a matching `free` — no RAII.

## Phase Status

Phase 2 complete (file ops, base64, echo, 87 tests incl. PBT). Phase 3+ (exec, TCP, named pipes) not started. See `CLAUDE.md` for full spec.
