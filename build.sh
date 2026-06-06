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

cmake --preset mingw
cmake --build --preset mingw

if [ "$1" = "test" ]; then
    ctest --preset mingw --output-on-failure
fi
