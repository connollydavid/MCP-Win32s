#!/bin/sh
# build.sh - thin wrapper around the CMake build (single source of truth).
#
# Usage: ./build.sh          (configure + build)
#        ./build.sh test     (configure + build + run tests)
#
# The real build definition lives in CMakeLists.txt + CMakePresets.json. The
# "mingw" preset cross-compiles for the i386/Win32s target; on the WSL2+Windows
# dev host the test PEs run natively via WSL interop, otherwise under Wine
# (selected automatically by toolchains/mingw-w64-i386.cmake).
#
# This is free and unencumbered software released into the public domain.
set -e

# host-pbt: the Phase 4 theft harness - native gcc -std=c99 + ASan/UBSan
# on the OS-independent modules (see tests/host/README.md). Runs BEFORE
# the cross-compiled suite in CI (fail fast with shrunk counterexamples).
if [ "$1" = "host-pbt" ]; then
    SAN="-fsanitize=address,undefined"
    HFLAGS="-std=c99 -O1 -g $SAN -Wall -Werror -Ivendor/theft/inc -Itests/host -Isrc"
    mkdir -p build/host/theft
    for f in vendor/theft/src/*.c; do
        # no -Werror for vendored code (not patched)
        gcc -std=c99 -O1 -g $SAN -Wall -D_POSIX_C_SOURCE=199309L \
            -Ivendor/theft/inc -c "$f" -o "build/host/theft/$(basename "$f" .c).o"
    done
    gcc $HFLAGS tests/host/theft_base64.c  src/base64.c      build/host/theft/*.o -lm -o build/host/theft_base64
    gcc $HFLAGS tests/host/theft_json.c    src/json_parser.c build/host/theft/*.o -lm -o build/host/theft_json
    gcc $HFLAGS tests/host/theft_argv.c    src/argv.c        build/host/theft/*.o -lm -o build/host/theft_argv
    gcc $HFLAGS tests/host/theft_catalog.c src/catalog.c     build/host/theft/*.o -lm -o build/host/theft_catalog
    # mem_ops: only the two pure arithmetic guards compile natively
    # (MEM_OPS_HOST_PURE excludes the Win32 surface) - the off-by-overflow pin.
    gcc $HFLAGS -DMEM_OPS_HOST_PURE tests/host/theft_mem.c src/mem_ops.c build/host/theft/*.o -lm -o build/host/theft_mem
    # UBSan runs in recover mode: theft's own PRNG prints one benign
    # shift diagnostic (vendored, not patched). The modules under test
    # are expected to stay diagnostic-free - see tests/host/README.md.
    build/host/theft_base64
    build/host/theft_json
    build/host/theft_argv
    build/host/theft_catalog
    build/host/theft_mem
    echo "host-pbt: all properties passed"
    exit 0
fi

cmake --preset mingw
cmake --build --preset mingw

if [ "$1" = "test" ]; then
    ctest --preset mingw --output-on-failure
fi
