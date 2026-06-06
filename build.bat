@echo off
REM build.bat - thin wrapper around the CMake build (single source of truth).
REM
REM Usage: build.bat          (configure + build)
REM        build.bat test     (configure + build + run tests)
REM
REM The real build definition lives in CMakeLists.txt + CMakePresets.json.
REM This uses the "vc6" preset, which drives the Visual C++ 6.0 tools via the
REM NMake Makefiles generator - no Visual Studio 6 IDE required. Run from a
REM shell where the VC6 tools are on PATH (e.g. after VCVARS32.BAT).
REM
REM This is free and unencumbered software released into the public domain.

cmake --preset vc6 || goto :error
cmake --build --preset vc6 || goto :error

if "%1"=="test" (
    ctest --preset vc6 --output-on-failure || goto :error
)

goto :done

:error
echo.
echo BUILD FAILED
exit /b 1

:done
