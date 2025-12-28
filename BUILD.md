# Building PatriciaLearn 5.1

## Quick Start

### Windows (Auto-detect compiler)
```bash
cd engine
build.bat
```

This will automatically detect and use available compiler (g++, clang++, or MSVC).

### Windows (Specific compiler)
```bash
cd engine
build_mingw.bat    # For MinGW
build_clang.bat    # For Clang
build_msvc.bat     # For MSVC (requires Visual Studio Developer Command Prompt)
```

### Using Makefile
```bash
cd engine
make build         # Default (clang++)
make mingw         # MinGW g++
make clang         # Clang++
make msvc          # MSVC (requires make and Visual Studio)
```

## Manual Compilation

### MinGW
```bash
g++ -O3 -std=c++20 -ffast-math -march=native src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c -o patricia.exe -static-libgcc -static-libstdc++
```

### Clang
```bash
clang++ -O3 -std=c++20 -ffast-math -march=native src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c -o patricia.exe
```

### MSVC
```bash
cl /O2 /std:c++20 /EHsc /W3 src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c /Fe:patricia.exe
```

## Source Files

The engine requires these source files:
- `src/patricia.cpp` - Main entry point
- `src/learning.cpp` - Experience learning module
- `src/fathom/src/tbprobe.c` - Tablebase probing

## Compiler Requirements

- **C++20** support required
- **MinGW-w64**: Download from https://www.msys2.org/
- **Clang**: Download from https://releases.llvm.org/
- **MSVC**: Included with Visual Studio

## Build Options

- `-O3` / `/O2`: Maximum optimization
- `-std=c++20` / `/std:c++20`: C++20 standard
- `-ffast-math`: Fast floating point (GCC/Clang only)
- `-march=native`: Optimize for your CPU (GCC/Clang only)
- `-static-libgcc -static-libstdc++`: Static linking for portable executable (MinGW only)

## Troubleshooting

### "g++ not found"
- Install MinGW-w64 and add to PATH
- Or use `build.bat` to auto-detect available compiler

### "cl not found" (MSVC)
- Run from Visual Studio Developer Command Prompt
- Or run: `vcvarsall.bat x64` before building

### "undefined reference" errors
- Make sure all source files are included:
  - `src/patricia.cpp`
  - `src/learning.cpp`
  - `src/fathom/src/tbprobe.c`

### Path issues on Windows
- Use backslashes (`\`) in batch files
- Use forward slashes (`/`) in Makefile

## Output

Successful build creates `patricia.exe` in the `engine` directory.

