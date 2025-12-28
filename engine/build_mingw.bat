@echo off
REM ============================================================================
REM Build script for PatriciaLearn - Optimized for modern CPUs
REM Supports: BMI2, AVX2, AVX512, POPCNT
REM Usage: build_mingw.bat
REM ============================================================================

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

echo ============================================================================
echo  PatriciaLearn Build Script - Maximum Performance Edition
echo ============================================================================
echo.
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

echo Compiler: %CXX%
echo.
echo Building with MAXIMUM PERFORMANCE optimizations:
echo   - Native CPU architecture (march=native)
echo   - AVX2, AVX512, BMI2, POPCNT enabled
echo   - Link-Time Optimization (LTO)
echo   - Fast math operations
echo   - Loop unrolling
echo.
echo Please wait, LTO compilation takes longer but produces faster code...
echo.

REM ============================================================================
REM COMPILER FLAGS EXPLANATION:
REM   -O3                 : Maximum optimization level
REM   -std=c++20          : C++20 standard
REM   -march=native       : Use all CPU features available on this machine
REM   -mtune=native       : Tune code for this specific CPU
REM   -mavx2              : Enable AVX2 (256-bit SIMD)
REM   -mavx512f           : Enable AVX-512 Foundation
REM   -mavx512bw          : Enable AVX-512 Byte/Word operations
REM   -mavx512dq          : Enable AVX-512 Doubleword/Quadword operations
REM   -mavx512vl          : Enable AVX-512 Vector Length extensions
REM   -mbmi2              : Enable BMI2 (PEXT/PDEP instructions for bitboards)
REM   -mpopcnt            : Enable POPCNT (population count for bitboards)
REM   -flto               : Link-Time Optimization (whole program optimization)
REM   -ffast-math         : Fast floating point (allows reordering)
REM   -funroll-loops      : Unroll loops for better performance
REM   -fomit-frame-pointer: Free up a register
REM   -DNDEBUG            : Disable assertions for release build
REM   -static             : Static linking (no DLL dependencies)
REM ============================================================================

set CXXFLAGS=-O3 -std=c++20 -march=native -mtune=native -mavx2 -mavx512f -mavx512bw -mavx512dq -mavx512vl -mbmi2 -mpopcnt -flto -ffast-math -funroll-loops -fomit-frame-pointer -DNDEBUG -static

%CXX% %CXXFLAGS% src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c -o patricia.exe

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ============================================================================
    echo  Compilation failed!
    echo  If AVX512 is not supported, try the fallback build...
    echo ============================================================================
    echo.
    echo Trying fallback build without AVX512...
    set CXXFLAGS=-O3 -std=c++20 -march=native -mtune=native -mavx2 -mbmi2 -mpopcnt -flto -ffast-math -funroll-loops -fomit-frame-pointer -DNDEBUG -static
    %CXX% %CXXFLAGS% src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c -o patricia.exe
    if %ERRORLEVEL% NEQ 0 (
        echo.
        echo Fallback build also failed. Check errors above.
        exit /b 1
    )
)

echo.
echo ============================================================================
echo  BUILD SUCCESSFUL!
echo ============================================================================
echo.
echo Output: %CD%\patricia.exe
echo.
echo This build is optimized for YOUR specific CPU with:
echo   * AVX2/AVX512 vector instructions
echo   * BMI2 for fast bitboard operations (PEXT/PDEP)
echo   * POPCNT for fast bit counting
echo   * Link-Time Optimization for maximum speed
echo.
echo NOTE: This executable will ONLY work on CPUs with similar features.
echo       Do not distribute this build to others.
echo ============================================================================
