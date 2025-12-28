@echo off
REM Build script for PatriciaLearn - Auto-detects compiler
REM Usage: build.bat [compiler]
REM Compilers: g++, clang++, cl (MSVC)

setlocal enabledelayedexpansion

set SOURCES=src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c
set OUT=patricia.exe

REM Check for compiler argument
if "%1"=="" (
    REM Auto-detect compiler
    where g++ >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        set CXX=g++
        set CXXFLAGS=-O3 -std=c++20 -ffast-math -march=native
        set LINKER=-static-libgcc -static-libstdc++
        echo Using MinGW g++
        goto :build
    )
    
    where clang++ >nul 2>&1
    if !ERRORLEVEL! EQU 0 (
        set CXX=clang++
        set CXXFLAGS=-O3 -std=c++20 -ffast-math -march=native
        set LINKER=
        echo Using Clang++
        goto :build
    )
    
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
        set LINKER=-static-libgcc -static-libstdc++
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
echo Building PatriciaLearn...
echo Compiler: %CXX%
echo Sources: %SOURCES%

if "%CXX%"=="cl" (
    REM MSVC syntax
    %CXX% %CXXFLAGS% %SOURCES% /Fe:%OUT% %LINKER%
) else (
    REM GCC/Clang syntax
    %CXX% %CXXFLAGS% %SOURCES% -o %OUT% %LINKER%
)

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful! Output: %OUT%
) else (
    echo.
    echo Build failed!
    exit /b 1
)

