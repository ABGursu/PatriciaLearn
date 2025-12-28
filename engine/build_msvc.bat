@echo off
REM Build script for PatriciaLearn using MSVC
REM Usage: build_msvc.bat
REM Note: Run from Visual Studio Developer Command Prompt or after vcvarsall.bat
REM This script must be run from the engine directory

REM Change to script directory (engine folder)
cd /d "%~dp0"

REM Verify we're in the correct directory
if not exist "src\patricia.cpp" (
    echo ERROR: Cannot find src\patricia.cpp
    echo Please run this script from the engine directory
    exit /b 1
)

if not exist "src\learning.cpp" (
    echo ERROR: Cannot find src\learning.cpp
    echo Please run this script from the engine directory
    exit /b 1
)

if not exist "src\fathom\src\tbprobe.c" (
    echo ERROR: Cannot find src\fathom\src\tbprobe.c
    echo Please run this script from the engine directory
    exit /b 1
)

echo Checking for MSVC cl...
where cl >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: MSVC cl not found!
    echo Please run from Visual Studio Developer Command Prompt
    echo Or run: "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
    exit /b 1
)

echo.
echo Building PatriciaLearn with MSVC...
echo Working directory: %CD%
echo.

cl /O2 /std:c++20 /EHsc /W3 src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c /Fe:patricia.exe

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Output: patricia.exe
    if exist "patricia.exe" (
        echo File location: %CD%\patricia.exe
    )
) else (
    echo.
    echo Build failed!
    exit /b 1
)
