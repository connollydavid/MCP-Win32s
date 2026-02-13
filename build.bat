@echo off
REM build.bat - Visual C++ 6.0 build script for MCP-Win32s
REM
REM Usage: build.bat          (build everything)
REM        build.bat test     (build and run tests)
REM
REM Requires: Visual C++ 6.0 (or later) in PATH
REM
REM This is free and unencumbered software released into the public domain.

echo === MCP-Win32s Build (Visual C++, C89, i386) ===
echo.

REM Build JSON parser tests
echo Building test_json.exe ...
cd tests
cl /W3 /O2 /TC /G3 /I..\src test_json.c ..\src\json_parser.c kernel32.lib
if errorlevel 1 goto :error
cd ..

echo Build complete.

REM Run tests if requested
if "%1"=="test" (
    echo.
    echo === Running Tests ===
    echo.
    tests\test_json.exe
    if errorlevel 1 goto :error
    echo.
    echo === All tests passed ===
)

goto :done

:error
echo.
echo BUILD FAILED
exit /b 1

:done
