#
# mingw-w64-i386.cmake - cross-compile toolchain for the i386/Win32s target.
#
# This is the build that is actually exercised: on the WSL2+Windows dev host
# the resulting PEs run natively via WSL interop, and in CI they run under
# Wine. The emulator is selected automatically below.
#
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(TOOLCHAIN_PREFIX i686-w64-mingw32)
set(CMAKE_C_COMPILER  ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# i386-only codegen + the strict warning gate the project has always used.
set(CMAKE_C_FLAGS_INIT
    "-march=i386 -mtune=i386 -Wall -Werror -pedantic -Wdouble-promotion -Wfloat-equal")
# Match the historic -O2 (CMake's GNU Release default is -O3).
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")

# Run the PEs natively when WSL interop is present (the dev host); otherwise
# fall back to Wine (CI). Mirrors build.sh's RUNNER detection.
if(NOT (EXISTS "/proc/sys/fs/binfmt_misc/WSLInterop" OR
        EXISTS "/proc/sys/fs/binfmt_misc/WSLInterop-late"))
  find_program(WINE_EXECUTABLE wine)
  if(WINE_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${WINE_EXECUTABLE})
  endif()
endif()
