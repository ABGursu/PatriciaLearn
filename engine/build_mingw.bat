@echo off
REM Build script for PatriciaLearn using MinGW
REM Usage: build_mingw.bat
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

echo Checking for MinGW g++...

REM First check PATH
where g++ >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    set CXX=g++
    set FOUND=1
    goto :found
)

REM Check MSYS2 default locations
if exist "C:\msys64\mingw64\bin\g++.exe" (
    set CXX=C:\msys64\mingw64\bin\g++.exe
    set FOUND=1
    goto :found
)

if exist "C:\msys64\ucrt64\bin\g++.exe" (
    set CXX=C:\msys64\ucrt64\bin\g++.exe
    set FOUND=1
    goto :found
)

echo ERROR: g++ not found!
echo Please install MinGW-w64 and add it to PATH
echo Or install MSYS2: https://www.msys2.org/
exit /b 1

:found

echo.
echo Building PatriciaLearn with MinGW...
echo Using compiler: %CXX%
echo Working directory: %CD%
echo.

%CXX% -O3 -std=c++20 -ffast-math -march=native src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c -o patricia.exe -static
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Compilation failed. Check errors above.
    exit /b 1
)

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
