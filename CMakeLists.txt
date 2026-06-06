#
# CMakeLists.txt - single source of truth for the MCP-Win32s build.
#
# One target definition feeds every toolchain: MinGW-w64 cross (the build
# actually exercised on the dev host and in CI), and VC6's cl/nmake via the
# "NMake Makefiles" generator (toolchains/vc6-nmake.cmake) for anyone who
# wants a period-authentic binary - no Visual Studio 6 IDE / .dsp required.
#
# Configure with a preset, e.g.:   cmake --preset mingw
#                                   cmake --build --preset mingw
#                                   ctest --preset mingw
#
# This is free and unencumbered software released into the public domain.
#
cmake_minimum_required(VERSION 3.21)
project(mcp-w32s C)

# C89 strict, no GNU extensions (matches the historic -std=c89 -pedantic).
set(CMAKE_C_STANDARD 90)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Win32s load requirements: the PE must carry relocations (/FIXED:NO) and be
# based at 0x10000 (the default 0x400000 region is occupied by Win32s).
# Applied to every executable we emit.
if(MSVC)
  add_link_options(/FIXED:NO /BASE:0x10000 /MACHINE:I386 /SUBSYSTEM:CONSOLE)
else()
  add_link_options(-Wl,--dynamicbase -Wl,--image-base,0x10000)
endif()

include_directories(src tests)

# Core modules shared by the executable and the serial/main-loop tests.
set(CORE_SOURCES
  src/json_parser.c
  src/base64.c
  src/file_ops.c
  src/serial.c
  src/transport.c
  src/tcp.c)

# --- main executable -------------------------------------------------------
# The TCP backend is runtime-loaded (LoadLibraryA), so we deliberately do NOT
# link wsock32 - the binary must still load on bare Win32s without TCP/IP-32.
add_executable(mcp-w32s src/mcp-w32s.c ${CORE_SOURCES})
target_link_libraries(mcp-w32s PRIVATE kernel32 user32)

# --- tests -----------------------------------------------------------------
enable_testing()

add_executable(test_json tests/test_json.c src/json_parser.c)
target_link_libraries(test_json PRIVATE kernel32)

add_executable(test_transport
  tests/test_transport.c tests/mock_transport.c src/transport.c)
target_link_libraries(test_transport PRIVATE kernel32)

# test_tcp's own client side links wsock32 directly; the backend under test
# still resolves Winsock at runtime.
add_executable(test_tcp tests/test_tcp.c src/tcp.c src/transport.c)
target_link_libraries(test_tcp PRIVATE kernel32 wsock32)

# test_serial includes mcp-w32s.c with TEST_BUILD to exclude main() and reach
# ProcessBuffer/ProcessCommand.
add_executable(test_serial
  tests/test_serial.c tests/mock_transport.c
  src/mcp-w32s.c src/transport.c src/serial.c
  src/json_parser.c src/base64.c src/file_ops.c)
target_compile_definitions(test_serial PRIVATE TEST_BUILD)
target_link_libraries(test_serial PRIVATE kernel32 user32)

add_executable(test_base64 tests/test_base64.c src/base64.c)
target_link_libraries(test_base64 PRIVATE kernel32)

add_executable(test_file_ops tests/test_file_ops.c src/file_ops.c)
target_link_libraries(test_file_ops PRIVATE kernel32)

add_executable(test_pbt_base64 tests/test_pbt_base64.c src/base64.c)
target_link_libraries(test_pbt_base64 PRIVATE kernel32)

# CMAKE_CROSSCOMPILING_EMULATOR (set by the toolchain: empty for native
# WSL-interop execution, "wine" otherwise) is applied to each test command.
foreach(t test_json test_transport test_tcp test_serial
          test_base64 test_file_ops test_pbt_base64)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
