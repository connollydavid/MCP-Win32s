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

# On a Linux cross-host, constrain CMake's searches to the MinGW cross-sysroot.
# On a NATIVE Windows host (MSYS2 MINGW32 - used by the windows-latest CI job so
# the PEs run against real cmd.exe/Winsock/ConPTY instead of Wine), the
# compiler's own sysroot is already correct and the Linux path below does not
# exist, so leave CMake's default search alone there.
if(NOT CMAKE_HOST_WIN32)
  set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
  set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
else()
  # MSYS2's i686 windres fails to spawn its default preprocessor ("CreateProcess
  # failed: The system cannot find the file specified") when CMake drives it.
  # Point it at the mingw32 gcc explicitly and use a temp file rather than the
  # pipe spawn that misbehaves on Windows. (binutils 2.36+ --preprocessor-arg
  # syntax.) Linux cross-windres needs none of this and is left untouched.
  set(CMAKE_RC_FLAGS
      "--use-temp-file --preprocessor=i686-w64-mingw32-gcc --preprocessor-arg=-E --preprocessor-arg=-xc-header --preprocessor-arg=-DRC_INVOKED")
endif()

# i386-only codegen + the strict warning gate the project has always used.
# (The -O2 Release pin lives in CMakeLists.txt, where it can override CMake's
# GNU default after project() rather than merely seed it.)
set(CMAKE_C_FLAGS_INIT
    "-march=i386 -mtune=i386 -Wall -Werror -pedantic -Wdouble-promotion -Wfloat-equal")

# Run the PEs natively when WSL interop is present (the dev host); otherwise
# fall back to Wine (CI). Mirrors build.sh's RUNNER detection.
if(NOT (EXISTS "/proc/sys/fs/binfmt_misc/WSLInterop" OR
        EXISTS "/proc/sys/fs/binfmt_misc/WSLInterop-late"))
  find_program(WINE_EXECUTABLE wine)
  if(WINE_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${WINE_EXECUTABLE})
  endif()
endif()
