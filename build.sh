#!/bin/sh
# build.sh - MinGW cross-compile build script for MCP-Win32s
#
# Usage: ./build.sh          (build everything)
#        ./build.sh test     (build and run tests via Wine)
#
# Requirements: i686-w64-mingw32-gcc, wine (for test execution)
#
# This is free and unencumbered software released into the public domain.

set -e

CC="i686-w64-mingw32-gcc"
CFLAGS="-O2 -std=c89 -march=i386 -mtune=i386"
WARNINGS="-Wall -Werror -pedantic -Wdouble-promotion -Wfloat-equal"
LDFLAGS="-Wl,--dynamicbase -Wl,--image-base,0x10000"

echo "=== MCP-Win32s Build (MinGW-w64, C89, i386) ==="
echo ""

# Build main executable
echo "Building mcp-w32s.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -o mcp-w32s.exe \
    src/mcp-w32s.c src/serial.c src/json_parser.c \
    -lkernel32 -luser32 -lwsock32

# Build JSON parser tests
echo "Building test_json.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -o tests/test_json.exe \
    tests/test_json.c src/json_parser.c \
    -lkernel32

# Build serial + main loop tests
# Includes mcp-w32s.c with -DTEST_BUILD to exclude main() and expose
# ProcessBuffer/ProcessCommand for testing
echo "Building test_serial.exe ..."
$CC $CFLAGS $WARNINGS $LDFLAGS \
    -Isrc -DTEST_BUILD -o tests/test_serial.exe \
    tests/test_serial.c src/mcp-w32s.c src/serial.c src/json_parser.c \
    -lkernel32 -luser32 -lwsock32

echo ""
echo "Build complete."

# Run tests if requested
if [ "$1" = "test" ]; then
    echo ""
    echo "=== Running Tests (Wine) ==="
    echo ""
    echo "--- JSON Parser Tests ---"
    wine tests/test_json.exe
    echo ""
    echo "--- Serial + Main Loop Tests ---"
    wine tests/test_serial.exe
    echo ""
    echo "=== All tests passed ==="
fi
