# Building PatriciaLearn 5.1

## Project Structure

```
PatriciaLearn/
├── engine/                    # Build scripts and Makefile are here
│   ├── src/                   # Source files
│   │   ├── patricia.cpp       # Main entry point
│   │   ├── learning.cpp       # Experience learning module
│   │   ├── fathom/src/        # Tablebase probing
│   │   │   └── tbprobe.c
│   │   └── ...                # Other source files
│   ├── nets/                  # NNUE network files
│   │   ├── firefly.nnue
│   │   ├── rw3.nnue
│   │   └── allie.nnue
│   ├── build.bat              # Auto-detect compiler
│   ├── build_mingw.bat        # MinGW specific
│   ├── build_clang.bat        # Clang specific
│   ├── build_msvc.bat         # MSVC specific
│   └── Makefile               # Make-based build
```

## Quick Start

### Windows (Recommended)

**From engine directory:**
```bash
cd engine
build.bat
```

**Or from project root:**
```bash
cd "C:\Github\Chess Dev\PatriciaLearn\engine"
build.bat
```

The build scripts automatically:
- Change to the correct directory (`engine/`)
- Verify all required files exist
- Detect available compiler
- Build with correct paths

### Using Specific Compiler

```bash
cd engine
build_mingw.bat    # MinGW g++
build_clang.bat    # Clang++
build_msvc.bat     # MSVC (requires VS Developer Command Prompt)
```

### Using Makefile

```bash
cd engine
make build         # Default (clang++)
make mingw         # MinGW g++
make clang         # Clang++
make msvc          # MSVC
```

## Source Files

The build includes these source files (all paths relative to `engine/` directory):

- `src/patricia.cpp` - Main entry point
- `src/learning.cpp` - Experience learning module  
- `src/fathom/src/tbprobe.c` - Tablebase probing

## NNUE Network Files

NNUE networks are embedded at compile time from:
- `nets/firefly.nnue` - Middlegame network
- `nets/rw3.nnue` - Endgame network
- `nets/allie.nnue` - Additional network

These files must be present in `engine/nets/` directory.

## Build Script Features

All build scripts (`build*.bat`):
- ✅ Automatically change to `engine/` directory
- ✅ Verify required source files exist
- ✅ Check for NNUE network files
- ✅ Use correct relative paths
- ✅ Work from any directory (script location is used)

## Manual Compilation

### MinGW
```bash
cd engine
g++ -O3 -std=c++20 -ffast-math -march=native src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c -o patricia.exe -static-libgcc -static-libstdc++
```

### Clang
```bash
cd engine
clang++ -O3 -std=c++20 -ffast-math -march=native src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c -o patricia.exe
```

### MSVC
```bash
cd engine
cl /O2 /std:c++20 /EHsc /W3 src\patricia.cpp src\learning.cpp src\fathom\src\tbprobe.c /Fe:patricia.exe
```

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
- `-Isrc` / `/I src`: Include path for source headers

## Troubleshooting

### "Cannot find src\patricia.cpp"
- Make sure you're running the script from the `engine/` directory
- Or the script will auto-change to its directory location

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
- Build scripts use `cd /d "%~dp0"` to auto-change to script directory
- Use backslashes (`\`) in batch files
- Use forward slashes (`/`) in Makefile (works on both Windows and Linux)

## Output

Successful build creates `patricia.exe` in the `engine/` directory.

## Verification

After building, verify the executable:
```bash
cd engine
.\patricia.exe bench
```
