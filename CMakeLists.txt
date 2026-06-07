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

# CMake's GNU Release default is -O3; pin -O2 to match the project's historic
# builds and keep codegen predictable on the 16 MB Win32s budget. Must be set
# after project() (which populates the *_RELEASE flags) and only for GCC/Clang;
# MSVC/VC6 keeps its own /O2-class Release defaults.
if(NOT MSVC)
  set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
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
  src/charset_tables.c
  src/charset_tables_data.c
  src/file_ops.c
  src/serial.c
  src/transport.c
  src/tcp.c
  src/feat.c
  src/ready.c
  src/argv.c
  src/binfmt.c
  src/catalog.c
  src/exec_ops.c
  src/pty_exec.c
  src/toolchain_probe.c
  src/mem_ops.c
  src/audit.c)

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
  src/mcp-w32s.c ${CORE_SOURCES})
target_compile_definitions(test_serial PRIVATE TEST_BUILD)
target_link_libraries(test_serial PRIVATE kernel32 user32)

add_executable(test_base64 tests/test_base64.c src/base64.c)
target_link_libraries(test_base64 PRIVATE kernel32)

add_executable(test_file_ops tests/test_file_ops.c src/file_ops.c)
target_link_libraries(test_file_ops PRIVATE kernel32)

add_executable(test_pbt_base64 tests/test_pbt_base64.c src/base64.c)
target_link_libraries(test_pbt_base64 PRIVATE kernel32)

# --- Phase 4 test targets ---------------------------------------------------
add_executable(test_feat tests/test_feat.c src/feat.c)
target_link_libraries(test_feat PRIVATE kernel32)

add_executable(test_argv tests/test_argv.c src/argv.c src/base64.c)
target_link_libraries(test_argv PRIVATE kernel32)

# argv_echo: helper child for test_argv's spawn-based PBT roundtrip.
add_executable(argv_echo tests/argv_echo.c src/base64.c)
target_link_libraries(argv_echo PRIVATE kernel32)
add_dependencies(test_argv argv_echo)

add_executable(test_binfmt tests/test_binfmt.c src/binfmt.c src/feat.c)
target_link_libraries(test_binfmt PRIVATE kernel32)

add_executable(test_catalog tests/test_catalog.c src/catalog.c)
target_link_libraries(test_catalog PRIVATE kernel32)

add_executable(test_exec_ops
  tests/test_exec_ops.c src/exec_ops.c src/feat.c src/binfmt.c)
target_link_libraries(test_exec_ops PRIVATE kernel32)

add_executable(test_pty_exec tests/test_pty_exec.c src/pty_exec.c src/feat.c)
target_link_libraries(test_pty_exec PRIVATE kernel32)

# --- 5.2 build-toolchain test targets --------------------------------------
add_executable(test_toolchain_probe
  tests/test_toolchain_probe.c src/toolchain_probe.c
  src/exec_ops.c src/catalog.c src/feat.c src/binfmt.c)
target_link_libraries(test_toolchain_probe PRIVATE kernel32)

# --- 5.3 memory peek/poke test targets -------------------------------------
# test_mem_ops drives the spawn-retain table + the process-tier RPM/WPM
# round-trip, so it needs TEST_BUILD (MemResetForTest) and the mem_target
# helper child next to it. audit.c is linked because MemPoke audits.
add_executable(test_mem_ops
  tests/test_mem_ops.c src/mem_ops.c src/audit.c
  src/feat.c src/catalog.c src/argv.c)
target_compile_definitions(test_mem_ops PRIVATE TEST_BUILD)
target_link_libraries(test_mem_ops PRIVATE kernel32 user32)

# mem_target: a long-sleeping helper child for the process-tier round-trip.
add_executable(mem_target tests/mem_target.c)
target_link_libraries(mem_target PRIVATE kernel32)
add_dependencies(test_mem_ops mem_target)

add_executable(test_audit tests/test_audit.c src/audit.c)
target_link_libraries(test_audit PRIVATE kernel32 user32)

# --- 5.4 text-encoding test targets ----------------------------------------
# test_encoding exercises the OS-independent charset tables (M2: the exhaustive
# per-page round-trip on the bijective subset + the documented non-bijection
# cases + strict narrowing). Pure - links only the table sources.
add_executable(test_encoding
  tests/test_encoding.c src/charset_tables.c src/charset_tables_data.c)
target_link_libraries(test_encoding PRIVATE kernel32)

# wire_client: the 4.0b wire-contract smoke client. A test tool, so a
# static wsock32 link is fine here (the SERVER stays runtime-loaded).
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/smoke/wire_client.c)
  add_executable(wire_client tests/smoke/wire_client.c)
  target_link_libraries(wire_client PRIVATE kernel32 wsock32)
endif()

# binfmt fixtures + the command catalog travel next to the test binaries.
add_custom_command(TARGET test_binfmt POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/tests/fixtures
          $<TARGET_FILE_DIR:test_binfmt>/fixtures)
add_custom_command(TARGET test_catalog POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/catalog
          $<TARGET_FILE_DIR:test_catalog>/catalog)
# (test_serial's exec-integration tests also read it from this copy.)

# CMAKE_CROSSCOMPILING_EMULATOR (set by the toolchain: empty for native
# WSL-interop execution, "wine" otherwise) is applied to each test command.
foreach(t test_json test_transport test_tcp test_serial
          test_base64 test_file_ops test_pbt_base64
          test_feat test_argv test_binfmt test_catalog
          test_exec_ops test_pty_exec test_toolchain_probe
          test_mem_ops test_audit test_encoding)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# The smoke run (server over /TCP + wire_client) needs process orchestration;
# it lives as a CI step and in build.sh smoke, not as a ctest entry.
