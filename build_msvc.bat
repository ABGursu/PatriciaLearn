@echo off
REM Build script for PatriciaLearn using MSVC
REM Usage: build_msvc.bat
REM Note: Run from Visual Studio Developer Command Prompt or after vcvarsall.bat

echo Checking for MSVC cl...
where cl >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: MSVC cl not found!
    echo Please run from Visual Studio Developer Command Prompt
    echo Or run: "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
    exit /b 1
)

echo Building PatriciaLearn with MSVC...
cl /O2 /std:c++20 /EHsc /W3 src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c /Fe:patricia.exe

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Output: patricia.exe
) else (
    echo.
    echo Build failed!
    exit /b 1
)

