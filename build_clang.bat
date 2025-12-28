@echo off
REM Build script for PatriciaLearn using Clang
REM Usage: build_clang.bat

echo Checking for Clang++...
where clang++ >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: clang++ not found in PATH!
    echo Please install LLVM/Clang and add it to PATH
    echo Download from: https://releases.llvm.org/
    exit /b 1
)

echo Building PatriciaLearn with Clang++...
clang++ -O3 -std=c++20 -ffast-math -march=native src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c -o patricia.exe

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Output: patricia.exe
) else (
    echo.
    echo Build failed!
    exit /b 1
)

