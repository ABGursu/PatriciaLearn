@echo off
REM Build script for PatriciaLearn using MinGW
REM Usage: build_mingw.bat

echo Checking for MinGW g++...
where g++ >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: g++ not found in PATH!
    echo Please install MinGW-w64 and add it to PATH
    echo Download from: https://www.msys2.org/
    exit /b 1
)

echo Building PatriciaLearn with MinGW...
g++ -O3 -std=c++20 -ffast-math -march=native src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c -o patricia.exe -static-libgcc -static-libstdc++

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Output: patricia.exe
) else (
    echo.
    echo Build failed!
    exit /b 1
)
