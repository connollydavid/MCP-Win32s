#!/bin/sh
# build.sh - MinGW cross-compile build script for MCP-Win32s
#
# Usage: ./build.sh          (build everything)
#        ./build.sh test     (build and run tests)
#
# Test execution: on WSL2 with a Windows host the MinGW PEs run NATIVELY
# via WSL interop (real kernel32/wsock32). Wine is only a fallback when no
# Windows host is reachable.
#
# Requirements: i686-w64-mingw32-gcc (+ optionally wine)
#
# This is free and unencumbered software released into the public domain.

set -e

CC="i686-w64-mingw32-gcc"
CFLAGS="-O2 -std=c89 -march=i386 -mtune=i386"
WARNINGS="-Wall -Werror -pedantic -Wdouble-promotion -Wfloat-equal"
LDFLAGS="-Wl,--dynamicbase -Wl,--image-base,0x10000"

echo "=== MCP-Win32s Build (MinGW-w64, C89, i386) ==="
echo ""

# Build main executable.
# NOTE: no -lwsock32 - the TCP backend is runtime-loaded (LoadLibraryA),
# so the binary carries no static Winsock import and still loads on bare
# Win32s. CI asserts this with objdump.
echo "Building mcp-w32s.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -o mcp-w32s.exe \
    src/mcp-w32s.c src/transport.c src/serial.c src/tcp.c \
    src/json_parser.c src/base64.c src/file_ops.c \
    -lkernel32 -luser32

# Build JSON parser tests
echo "Building test_json.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -o tests/test_json.exe \
    tests/test_json.c src/json_parser.c \
    -lkernel32

# Build serial + main loop tests
# Includes mcp-w32s.c with -DTEST_BUILD to exclude main() and expose
# ProcessBuffer/ProcessCommand for testing.
echo "Building test_serial.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -Itests -DTEST_BUILD -o tests/test_serial.exe \
    tests/test_serial.c tests/mock_transport.c \
    src/mcp-w32s.c src/transport.c src/serial.c src/json_parser.c \
    src/base64.c src/file_ops.c \
    -lkernel32 -luser32

# Build transport abstraction tests
echo "Building test_transport.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -Itests -o tests/test_transport.exe \
    tests/test_transport.c tests/mock_transport.c src/transport.c \
    -lkernel32

# Build TCP backend tests (the test's own client side links wsock32;
# the backend under test still resolves Winsock at runtime).
echo "Building test_tcp.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -Itests -o tests/test_tcp.exe \
    tests/test_tcp.c src/tcp.c src/transport.c \
    -lkernel32 -lwsock32

# Build base64 tests
echo "Building test_base64.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -o tests/test_base64.exe \
    tests/test_base64.c src/base64.c \
    -lkernel32

# Build file ops tests
echo "Building test_file_ops.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -o tests/test_file_ops.exe \
    tests/test_file_ops.c src/file_ops.c \
    -lkernel32

# Build property-based tests (PBT)
echo "Building test_pbt_base64.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -Itests -o tests/test_pbt_base64.exe \
    tests/test_pbt_base64.c src/base64.c \
    -lkernel32

echo ""
echo "Build complete."

# Run tests if requested
if [ "$1" = "test" ]; then
    # Prefer native Windows execution (WSL2 interop); fall back to Wine.
    RUNNER=""
    if [ ! -e /proc/sys/fs/binfmt_misc/WSLInterop ] && \
       [ ! -e /proc/sys/fs/binfmt_misc/WSLInterop-late ]; then
        RUNNER="wine"
    fi

    echo ""
    if [ -z "$RUNNER" ]; then
        echo "=== Running Tests (native Windows via WSL interop) ==="
    else
        echo "=== Running Tests (Wine fallback) ==="
    fi
    echo ""
    echo "--- JSON Parser Tests ---"
    $RUNNER tests/test_json.exe
    echo ""
    echo "--- Transport Tests ---"
    $RUNNER tests/test_transport.exe
    echo ""
    echo "--- TCP Backend Tests (real Winsock) ---"
    $RUNNER tests/test_tcp.exe
    echo ""
    echo "--- Serial + Main Loop Tests ---"
    $RUNNER tests/test_serial.exe
    echo ""
    echo "--- Base64 Tests ---"
    $RUNNER tests/test_base64.exe
    echo ""
    echo "--- File Ops Tests ---"
    $RUNNER tests/test_file_ops.exe
    echo ""
    echo "--- Property-Based Tests (Base64) ---"
    $RUNNER tests/test_pbt_base64.exe
    echo ""
    echo "=== All tests passed ==="
fi
