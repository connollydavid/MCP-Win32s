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

# RT_MANIFEST (the UTF-8 activeCodePage manifest), embedded in mcp-w32s.exe only.
# On MSVC, rc.exe compiles src/mcp-w32s.rc directly. On MinGW the manifest is
# shipped as a PREBUILT, byte-deterministic resource (src/mcp-w32s.res, generated
# from mcp-w32s.rc + mcp-w32s.manifest): compiling the committed .res to a coff
# object needs NO preprocessor, so it builds on every runner - including the
# MSYS2 native-Windows CI runner whose windres (binutils 2.46) cannot spawn a
# preprocessor to compile the .rc directly ("CreateProcess failed: The system
# cannot find the file specified", even with a full-path .exe). The SAME manifest
# lands in every binary; the Ubuntu CI job regenerates the .res from source and
# byte-diffs it, so the committed artifact can never drift from the .rc/.manifest.
enable_language(RC)   # MSVC: rc.exe compiles the .rc. (MinGW uses the .res below.)
if(MSVC)
  set(MCP_MANIFEST src/mcp-w32s.rc)
else()
  # Resolve windres to a FULL PATH. The custom command below runs via cmd.exe on
  # the MSYS2 native runner, where a bare `i686-w64-mingw32-windres` is "not
  # recognized" - CMake's own RC rule resolves it internally, but the
  # ${CMAKE_RC_COMPILER} variable stays the bare toolchain name. find_program runs
  # at configure time in the MSYS2 PATH and yields the full path cmd.exe can spawn.
  find_program(MCP_WINDRES NAMES "${CMAKE_RC_COMPILER}" windres REQUIRED)
  set(MCP_MANIFEST ${CMAKE_BINARY_DIR}/mcp-w32s_manifest.obj)
  add_custom_command(
    OUTPUT ${MCP_MANIFEST}
    COMMAND ${MCP_WINDRES} --input-format=res -O coff
            ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp-w32s.res ${MCP_MANIFEST}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/mcp-w32s.res
    COMMENT "Embedding the prebuilt RT_MANIFEST (res -> coff, no preprocessor)"
    VERBATIM)
  set_source_files_properties(${MCP_MANIFEST} PROPERTIES
    EXTERNAL_OBJECT TRUE GENERATED TRUE)
endif()

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
#
# PE version stamp: the native-Win32 floor is Windows NT 3.1 (1993), whose loader
# rejects a PE stamped with a newer OS/subsystem version (ERROR_BAD_FORMAT) - and
# mingw/MSVC default the stamp to 4.0. Stamp 3.10 so the 1993 loader accepts it;
# 3.10 still loads on every later Windows (they accept older stamps). The Win32s/
# Win9x loaders do not enforce this, so only NT 3.1 surfaced it (see the host
# repo's plan/PHASE6.md, NT 3.1 floor validation).
if(MSVC)
  add_link_options(/FIXED:NO /BASE:0x10000 /MACHINE:I386 /SUBSYSTEM:CONSOLE,3.10)
else()
  add_link_options(-Wl,--dynamicbase -Wl,--image-base,0x10000
    -Wl,--major-os-version,3 -Wl,--minor-os-version,10
    -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10)
endif()

include_directories(src tests)

# Core modules shared by the executable and the serial/main-loop tests.
set(CORE_SOURCES
  src/json_parser.c
  src/base64.c
  src/charset_tables.c
  src/charset_tables_data.c
  src/encoding.c
  src/file_ops.c
  src/serial.c
  src/uart.c
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
  src/audit.c
  src/strutil.c)

# --- main executable -------------------------------------------------------
# The TCP backend is runtime-loaded (LoadLibraryA), so we deliberately do NOT
# link wsock32 - the binary must still load on bare Win32s without TCP/IP-32.
#
# src/mcp-w32s.rc embeds the UTF-8 activeCodePage manifest at RT_MANIFEST ID 1.
# It is attached to THIS target only (never ${CORE_SOURCES}), so the test
# binaries that share the core sources do not pick up a second manifest.
#
# Default-manifest collision: empirically verified that this MinGW-w64 ld does
# NOT inject its own RT_MANIFEST at ID 1 (the default link yields exactly one
# manifest - our own), and its ld does NOT recognise -Wl,--disable-default-manifest
# (it errors out the link). So no suppression flag is added; doing so would
# both be unnecessary and break the MinGW link. (Verified: build/m5 probe,
# wrestool -l shows a single --type=24 --name=1.)
# The shipped device must run on bare Win32s 1.25a, which has no msvcrt.dll
# (Win32s ships the older CRTDLL.DLL). MinGW links the C runtime to msvcrt.dll
# by default, so under MinGW we drop the standard CRT (-nostdlib) and supply our
# own tiny freestanding runtime (src/mini_crt.c) over kernel32+user32 only.
# libgcc is kept for compiler-emitted helpers (e.g. __chkstk). Result:
# mcp-w32s.exe imports ONLY kernel32 + user32 (asserted in CI). The test
# binaries deliberately keep the ordinary CRT - they run on the dev host / Wine,
# never on Win32s. The VC6 toolchain path is unchanged.
# (Phase 6 Finding #1 remediation; see plan/PHASE6.md in the host repo.)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_executable(mcp-w32s src/mcp-w32s.c ${CORE_SOURCES} src/mini_crt.c ${MCP_MANIFEST})
    target_link_options(mcp-w32s PRIVATE -nostdlib -mconsole)
    target_link_libraries(mcp-w32s PRIVATE kernel32 user32 gcc)
else()
    add_executable(mcp-w32s src/mcp-w32s.c ${CORE_SOURCES} ${MCP_MANIFEST})
    target_link_libraries(mcp-w32s PRIVATE kernel32 user32)
endif()

# --- tests -----------------------------------------------------------------
enable_testing()

add_executable(test_json tests/test_json.c src/json_parser.c)
target_link_libraries(test_json PRIVATE kernel32)

add_executable(test_transport
  tests/test_transport.c tests/mock_transport.c src/transport.c)
target_link_libraries(test_transport PRIVATE kernel32)

# test_tcp's own client side links wsock32 directly; the backend under test
# still resolves Winsock at runtime.
add_executable(test_tcp tests/test_tcp.c src/tcp.c src/transport.c src/strutil.c)
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

# test_file_ops drives the tier-aware path seam (file_ops.c over EncOpenPath),
# so it links the encoding codec/tables + feat (the -W uplift + g_features).
add_executable(test_file_ops
  tests/test_file_ops.c src/file_ops.c src/encoding.c
  src/charset_tables.c src/charset_tables_data.c src/feat.c)
target_link_libraries(test_file_ops PRIVATE kernel32 user32)

add_executable(test_pbt_base64 tests/test_pbt_base64.c src/base64.c)
target_link_libraries(test_pbt_base64 PRIVATE kernel32)

# test_strutil: the prop.h on-target mirror of the McpStrCpyN properties
# (bounded/NUL/prefix/no-split), proving the device's own DBCS-aware bounded
# copy holds on the shipped C89/i386 path. The deep 50k-trial theft host run
# lives in tests/host/theft_strutil.c (built by build.sh host-pbt).
add_executable(test_strutil tests/test_strutil.c src/strutil.c)
target_link_libraries(test_strutil PRIVATE kernel32 user32)

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

# catalog.c serialises the listCommands listing via json_parser.c's JsonEscape.
add_executable(test_catalog tests/test_catalog.c src/catalog.c src/json_parser.c src/strutil.c)
target_link_libraries(test_catalog PRIVATE kernel32)

# exec_ops.c widens the cmdline for the -W spawn (encoding.c), so the
# standalone exec target links the codec + tables too.
add_executable(test_exec_ops
  tests/test_exec_ops.c src/exec_ops.c src/feat.c src/binfmt.c
  src/encoding.c src/charset_tables.c src/charset_tables_data.c src/strutil.c)
# TEST_BUILD exposes ExecClearHandleInheritForTest (the static
# ClearHandleInherit hook) so the NT 3.1 DuplicateHandle fallback is testable.
target_compile_definitions(test_exec_ops PRIVATE TEST_BUILD)
target_link_libraries(test_exec_ops PRIVATE kernel32)

add_executable(test_pty_exec tests/test_pty_exec.c src/pty_exec.c src/feat.c)
target_link_libraries(test_pty_exec PRIVATE kernel32)

# --- 5.2 build-toolchain test targets --------------------------------------
add_executable(test_toolchain_probe
  tests/test_toolchain_probe.c src/toolchain_probe.c
  src/exec_ops.c src/catalog.c src/json_parser.c src/feat.c src/binfmt.c
  src/encoding.c src/charset_tables.c src/charset_tables_data.c src/strutil.c)
target_link_libraries(test_toolchain_probe PRIVATE kernel32)

# --- 5.3 memory peek/poke test targets -------------------------------------
# test_mem_ops drives the spawn-retain table + the process-tier RPM/WPM
# round-trip, so it needs TEST_BUILD (MemResetForTest) and the mem_target
# helper child next to it. audit.c is linked because MemPoke audits.
add_executable(test_mem_ops
  tests/test_mem_ops.c src/mem_ops.c src/audit.c
  src/feat.c src/catalog.c src/json_parser.c src/argv.c src/strutil.c)
target_compile_definitions(test_mem_ops PRIVATE TEST_BUILD)
target_link_libraries(test_mem_ops PRIVATE kernel32 user32)

# mem_target: a long-sleeping helper child for the process-tier round-trip.
add_executable(mem_target tests/mem_target.c)
target_link_libraries(mem_target PRIVATE kernel32)
add_dependencies(test_mem_ops mem_target)

add_executable(test_audit tests/test_audit.c src/audit.c src/strutil.c)
target_link_libraries(test_audit PRIVATE kernel32 user32)

# --- 5.4 text-encoding test targets ----------------------------------------
# test_encoding exercises the OS-independent charset tables (the exhaustive
# per-page round-trip on the bijective subset + the documented non-bijection
# cases + strict narrowing) AND the pure UTF-8<->UTF-16 codec on-target mirror
# (encoding.c). Pure - links only the table + codec sources.
add_executable(test_encoding
  tests/test_encoding.c src/encoding.c
  src/charset_tables.c src/charset_tables_data.c src/feat.c)
target_link_libraries(test_encoding PRIVATE kernel32 user32)

# test_pbt_encoding: the prop.h on-target mirror of the four codec properties
# (round-trip / lossy-total / never-invalid / truncation-clean), proving they
# hold on the shipped C89/i386 codec path. The deep 50k-trial theft host run
# lives in tests/host/theft_encoding.c (built by build.sh host-pbt).
add_executable(test_pbt_encoding tests/test_pbt_encoding.c src/encoding.c)
# Codec-only: exclude the Win32 tier surface so this links without feat/tables.
target_compile_definitions(test_pbt_encoding PRIVATE ENCODING_HOST_PURE)
target_link_libraries(test_pbt_encoding PRIVATE kernel32)

# test_uart: the prop.h on-target mirror of the theft UART properties, proving
# the pure detection ladder + driving logic hold on the shipped C89/i386 path.
# UART_HOST_PURE excludes the asm IN/OUT seam + transport wiring (links uart.c only).
add_executable(test_uart tests/test_uart.c src/uart.c src/strutil.c)
target_compile_definitions(test_uart PRIVATE UART_HOST_PURE)
target_link_libraries(test_uart PRIVATE kernel32)

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
          test_base64 test_file_ops test_pbt_base64 test_strutil
          test_feat test_argv test_binfmt test_catalog
          test_exec_ops test_pty_exec test_toolchain_probe
          test_mem_ops test_audit test_encoding test_pbt_encoding test_uart)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# The smoke run (server over /TCP + wire_client) needs process orchestration;
# it lives as a CI step and in build.sh smoke, not as a ctest entry.
