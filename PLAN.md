# Phase 2 Implementation: File Operations + Echo + Base64

## Context

MCP-Win32s Phase 1 is complete: JSON parser, serial init, main loop, CI, 59 tests passing. Phase 2 adds file ops (read/write/list/delete), base64 codec, and an echo command, wired into `ProcessCommand` dispatch in `src/mcp-w32s.c`. All work must compile clean under MinGW MinGW C89/i386 strict and run under Wine.

**Hard constraints (non-negotiable, see CLAUDE.md):**
- C89 only — vars at block top, `/* */` comments, no `//`, no `long long`, no `inline`
- No floating point (banned). No 486+ instructions. No threads. No async I/O.
- ANSI Win32 APIs only: `CreateFileA`, `ReadFile`, `WriteFile`, `FindFirstFileA`, `FindNextFileA`, `FindClose`, `DeleteFileA`
- Single-level JSON. Use existing `BuildJsonResponse(id, status, key, value, buf, size)` API.
- Branch: create `claude/phase-2-file-ops` from `main`.

## Files to create

### 1. `src/base64.h`
```c
#ifndef BASE64_H
#define BASE64_H
/* Returns output length (excl. NUL). out_size must be >= ((in_len+2)/3)*4 + 1. */
int Base64Encode(const unsigned char *in, int in_len, char *out, int out_size);
/* Returns decoded byte count, or -1 on invalid input. Ignores whitespace. */
int Base64Decode(const char *in, unsigned char *out, int out_size);
#endif
```

### 2. `src/base64.c`
- Standard alphabet `A-Za-z0-9+/`, `=` padding.
- Integer-only. No tables larger than 64 bytes for encode; reverse-lookup table or switch for decode.
- Decode: skip `\n\r\t ` whitespace; reject any other non-alphabet char with `-1`.

### 3. `src/file_ops.h`
```c
#ifndef FILE_OPS_H
#define FILE_OPS_H
#include <windows.h>
#include "common.h"

/* All return 1 on success, 0 on failure. On failure, errMsg gets a short reason. */

/* Reads file into raw buffer. *outLen set to bytes read. Caller-provided buf. */
int FileOpRead(const char *path, unsigned char *buf, int bufSize,
               int *outLen, char *errMsg, int errSize);

/* Writes raw bytes to file (CREATE_ALWAYS). */
int FileOpWrite(const char *path, const unsigned char *data, int dataLen,
                char *errMsg, int errSize);

/* Lists dir contents into out (newline-separated, dirs suffixed with '\\'). */
int FileOpList(const char *path, char *out, int outSize,
               char *errMsg, int errSize);

/* Deletes file. */
int FileOpDelete(const char *path, char *errMsg, int errSize);
#endif
```

Implementation notes:
- `FileOpRead`: `CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)`. Read in a loop until EOF or buffer full. If file larger than `bufSize`, return 0 with errMsg "file too large".
- `FileOpWrite`: `CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)`. Write all bytes; check `bytesWritten == dataLen`.
- `FileOpList`: `FindFirstFileA(path + "\\*")` if `path` doesn't already end in `*`. Append each `cFileName`; if `dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY`, append `\\`. Append `\n` between entries (no trailing newline). Skip `.` and `..`. Bound-check against `outSize`. Always `FindClose`.
- All errMsg writes: use `lstrcpyn` or manual bounded copy. Common reasons: "file not found", "access denied", "path too long", "buffer overflow", "delete failed".
- Format `GetLastError()` numerically only if needed; prefer short literal strings.

### 4. Modify `src/mcp-w32s.c` — extend `ProcessCommand`

Replace the stub dispatch block (currently the `if (strcmp(cmd.cmd,"exec")...)` chain that returns "command received") with real handlers. Keep `exec` returning the stub for now (Phase 3).

Add include: `#include "base64.h"` and `#include "file_ops.h"`.

Dispatch logic (declare all locals at function top, C89):
```c
/* Add to top of ProcessCommand: */
unsigned char raw[MCP_MAX_DATA];   /* 64KB */
char b64[MCP_MAX_DATA];            /* base64 output */
char errMsg[128];
char fileList[MCP_MAX_DATA];
int rawLen;
int b64Len;
```

Branches:
- `cmd == "echo"`: `BuildJsonResponse(cmd.id, "ok", "data", cmd.line, response, sizeof(response))`.
- `cmd == "read"`: `FileOpRead(cmd.path, raw, sizeof(raw), &rawLen, errMsg, sizeof(errMsg))`. On success, `Base64Encode(raw, rawLen, b64, sizeof(b64))`, then respond `("ok", "data", b64)`. On failure, respond `("error", "error", errMsg)`.
- `cmd == "write"`: `rawLen = Base64Decode(cmd.data, raw, sizeof(raw))`. If `< 0`, respond `("error","error","invalid base64")`. Else `FileOpWrite(cmd.path, raw, rawLen, ...)`. Success: `("ok","message","written")`.
- `cmd == "list"`: `FileOpList(cmd.path, fileList, sizeof(fileList), errMsg, sizeof(errMsg))`. Success: `("ok","files",fileList)`. Failure: `("error","error",errMsg)`.
- `cmd == "delete"`: `FileOpDelete(cmd.path, errMsg, sizeof(errMsg))`. Success: `("ok","message","deleted")`.
- `cmd == "exec"`: keep stub (`"command received"`) — Phase 3.
- Unknown: `("error","error","unknown command")`.

Watch buffer sizes: `raw` and `b64` each 64KB → ~128KB stack. `fileList` 64KB. Plus `response` 128KB. Total ~256KB stack — fine on Win32 (default 1MB), but consider making them `static` to be safe. **Use `static` for all four large buffers** to keep stack usage minimal (acceptable: single-threaded program).

### 5. `tests/test_base64.c`
Use `test_framework.h`. Required cases (~12 tests):
- Encode empty → "" (len 0)
- Encode "f" → "Zg==", "fo" → "Zm8=", "foo" → "Zm9v", "foob" → "Zm9vYg=="
- Encode "foobar" → "Zm9vYmFy"
- Decode each of the above back to original (compare byte-for-byte and length)
- Decode rejects bad char (e.g. `"AB!="` → -1)
- Decode accepts whitespace (`"Zm9v\nYmFy"` → "foobar")
- Roundtrip 256-byte sequence (0..255), verify identical bytes after decode
- Encode buffer-too-small returns 0 (or whatever your contract says — be consistent with header)

`main()` runs all RUN_TEST + `print_test_summary()` + returns `g_tests_failed`.

### 6. `tests/test_file_ops.c`
Use `tempnam` or fixed temp paths under `%TEMP%` via `GetTempPathA`. Each test must clean up via `DeleteFileA`. ~10 tests:
- Write then read small text file → bytes identical
- Write then read 4KB binary blob (all 256 byte values cycled) → identical
- Read non-existent file → returns 0, errMsg non-empty
- Delete existing file → success, file gone (verify `FileOpRead` then fails)
- Delete non-existent file → returns 0
- List temp dir → contains the file we just wrote, has `\n` separator
- List non-existent dir → returns 0
- Write then overwrite same file with shorter content → read returns shorter content
- Read file larger than `bufSize` → returns 0 with "too large"
- List dir contains a subdir → entry ends with `\\`

Use `GetTempPathA(MAX_PATH, tmpDir)` and build paths like `tmpDir + "mcp_test_XXXX.tmp"`. Avoid hardcoding `C:\TEMP`.

### 7. Update `build.sh`
Add three new build steps before `if [ "$1" = "test" ]`:
```sh
echo "Building test_base64.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS -Isrc -o tests/test_base64.exe \
    tests/test_base64.c src/base64.c -lkernel32

echo "Building test_file_ops.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS -Isrc -o tests/test_file_ops.exe \
    tests/test_file_ops.c src/file_ops.c -lkernel32
```

Also add `src/base64.c src/file_ops.c` to the **main exe** build line and the **test_serial.exe** build line.

In the `test` block add:
```sh
echo "--- Base64 Tests ---"; wine tests/test_base64.exe
echo "--- File Ops Tests ---"; wine tests/test_file_ops.exe
```

### 8. Update `.github/workflows/build-and-test.yml`
- Add `src/base64.c src/file_ops.c` to main exe and test_serial build steps.
- Add two new "Build" steps mirroring `build.sh` for `test_base64.exe` and `test_file_ops.exe`.
- Add two new "Run" steps: `wine tests/test_base64.exe` and `wine tests/test_file_ops.exe`.
- Add `/tmp/base64.o` and `/tmp/file_ops.o` to the FPU/486 verify steps (compile them, include in `objdump` invocation).

### 9. Update `vc6/mcp-w32s.dsp`
Add `src\base64.c` and `src\file_ops.c` to both Debug and Release `# Begin Source File` blocks. Match the existing pattern for `serial.c`.

### 10. Update `CLAUDE.md` and `README.md`
- `CLAUDE.md`: Phase status table → mark Phase 2 complete. Update "Repository Structure" to list `base64.c/.h` and `file_ops.c/.h`. Update test counts.
- `README.md`: Phase 2 checklist → tick the boxes. Update test count summary.

## Verification

Local (Linux):
```sh
./build.sh test
```
Expected: all builds succeed with `-Werror -pedantic`, all 4 test binaries pass under Wine, total test count ≥ 80.

Spot-check binary cleanliness:
```sh
i686-w64-mingw32-objdump -d /tmp/base64.o /tmp/file_ops.o | grep -E 'fld|fst[^r]|fadd|fmul|cpuid|cmpxchg|bswap'
```
Must produce no output.

CI: push branch, confirm green check on GitHub Actions.

## Order of work

1. Create branch.
2. base64.h → base64.c → test_base64.c → build & test in isolation.
3. file_ops.h → file_ops.c → test_file_ops.c → build & test.
4. Wire into mcp-w32s.c ProcessCommand. Add echo. Run test_serial.exe — existing tests must still pass.
5. Update build.sh, CI workflow, .dsp.
6. Update docs.
7. Commit per logical step (3–5 commits). Open PR.

## Reuse — do not reinvent

- `BuildJsonResponse` (json_parser.c:289) for **all** responses.
- `JsonCommand` struct (common.h) — fields already include `path`, `data`, `line`.
- `test_framework.h` macros — do not write a new test harness.
- Constants in common.h: `MCP_MAX_DATA`, `MCP_MAX_PATH_LEN`, `MCP_MAX_RESPONSE`.

## Things to NOT do

- Do not add chunked-read support — Phase 3.
- Do not implement TCP or named-pipe transports — Phase 3+.
- Do not add the `exec` command — Phase 3.
- Do not introduce any new external dependency.
- Do not touch json_parser.c or serial.c logic.
- Do not change `BuildJsonResponse`'s single-key signature (file list goes in one string value).
