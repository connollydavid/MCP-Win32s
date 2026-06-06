#
# vc6-nmake.cmake - build with the Visual C++ 6.0 toolchain, no IDE.
#
# CMake removed the "Visual Studio 6" project generator in 3.6, but its docs
# still bless building WITH the VC6 tools via the NMake Makefiles generator.
# That is exactly this path - it drives cl.exe 12.00 + nmake from the same
# CMakeLists.txt, so there is no hand-maintained .dsp/.dsw to drift.
#
# Usage (on a Windows host with the VC6 tools on PATH - run vcvars first):
#     cmake --preset vc6
#     cmake --build --preset vc6
#
# NOT exercised in CI (the runners have no VC6). It is the documented,
# runnable VC6 build definition, kept faithful by living next to the others.
#
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER cl)

# VC6 flag vocabulary mapped to the project's build intent:
#   /TC  treat all as C    /G3  386 codegen    /W3  warnings
set(CMAKE_C_FLAGS_INIT "/nologo /W3 /TC /G3")
set(CMAKE_C_FLAGS_RELEASE_INIT "/O2 /DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG_INIT   "/Zi /Od /D_DEBUG")

# Win32s load requirements (also set in CMakeLists for MSVC; harmless to pin).
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "/nologo /subsystem:console /machine:I386 /FIXED:NO /BASE:0x10000")
