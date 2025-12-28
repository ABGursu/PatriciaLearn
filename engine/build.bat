@echo off
REM Build script for PatriciaLearn - Auto-detects compiler
REM Usage: build.bat [compiler]
REM Compilers: g++, clang++, cl (MSVC)
REM This script must be run from the engine directory

setlocal enabledelayedexpansion

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

if not exist "nets\firefly.nnue" (
    echo WARNING: Cannot find nets\firefly.nnue
    echo NNUE files should be in the nets\ directory
)

set SOURCES=src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c
set OUT=patricia.exe

REM Check for compiler argument
if "%1"=="" (
    REM Auto-detect compiler
    REM First check PATH
    where g++ >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        set CXX=g++
        set CXXFLAGS=-O3 -std=c++20 -ffast-math -march=native
        set LINKER=-static
        echo Using MinGW g++ (from PATH)
        goto :build
    )
    
    REM Check MSYS2 default locations
    if exist "C:\msys64\mingw64\bin\g++.exe" (
        set CXX=C:\msys64\mingw64\bin\g++.exe
        set CXXFLAGS=-O3 -std=c++20 -ffast-math -march=native
        set LINKER=-static
        echo Using MinGW g++ (from C:\msys64\mingw64\bin)
        goto :build
    )
    
    if exist "C:\msys64\ucrt64\bin\g++.exe" (
        set CXX=C:\msys64\ucrt64\bin\g++.exe
        set CXXFLAGS=-O3 -std=c++20 -ffast-math -march=native
        set LINKER=-static
        echo Using MinGW g++ (from C:\msys64\ucrt64\bin)
        goto :build
    )
    
    REM Check for Clang in PATH
    where clang++ >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        set CXX=clang++
        set CXXFLAGS=-O3 -std=c++20 -ffast-math -march=native
        set LINKER=
        echo Using Clang++ (from PATH)
        goto :build
    )
    
    REM Check MSYS2 Clang
    if exist "C:\msys64\clang64\bin\clang++.exe" (
        set CXX=C:\msys64\clang64\bin\clang++.exe
        set CXXFLAGS=-O3 -std=c++20 -ffast-math -march=native
        set LINKER=
        echo Using Clang++ (from C:\msys64\clang64\bin)
        goto :build
    )
    
    REM Check for MSVC
    where cl >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        set CXX=cl
        set CXXFLAGS=/O2 /std:c++20 /EHsc /W3
        set LINKER=
        echo Using MSVC
        goto :build
    )
    
    echo ERROR: No C++ compiler found!
    echo Please install one of: MinGW-w64, Clang, or MSVC
    echo Or specify compiler: build.bat g++  or  build.bat clang++  or  build.bat cl
    exit /b 1
) else (
    set CXX=%1
    if "%CXX%"=="g++" (
        set CXXFLAGS=-O3 -std=c++20 -ffast-math -march=native
        set LINKER=-static
    ) else if "%CXX%"=="clang++" (
        set CXXFLAGS=-O3 -std=c++20 -ffast-math -march=native
        set LINKER=
    ) else if "%CXX%"=="cl" (
        set CXXFLAGS=/O2 /std:c++20 /EHsc /W3
        set LINKER=
    ) else (
        echo Unknown compiler: %CXX%
        exit /b 1
    )
)

:build
echo.
echo Building PatriciaLearn...
echo Working directory: %CD%
echo Compiler: %CXX%
echo Sources: %SOURCES%
echo.

if "%CXX%"=="cl" (
    REM MSVC syntax
    %CXX% %CXXFLAGS% %SOURCES% /Fe:%OUT% %LINKER% 2>&1
) else (
    REM GCC/Clang syntax
    %CXX% %CXXFLAGS% %SOURCES% -o %OUT% %LINKER% 2>&1
)

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Output: %OUT%
    if exist "%OUT%" (
        echo File location: %CD%\%OUT%
    )
) else (
    echo.
    echo Build failed!
    exit /b 1
)
