# Building PatriciaLearn with MinGW

## Prerequisites

1. Install MinGW-w64 (MSYS2 recommended):
   - Download from: https://www.msys2.org/
   - Or use standalone MinGW-w64: https://www.mingw-w64.org/downloads/

2. Ensure `g++` is in your PATH:
   ```bash
   g++ --version
   ```

## Building

### Option 1: Using Makefile (if you have make installed)

```bash
cd engine
make mingw
```

### Option 2: Using the batch script (Windows)

```bash
cd engine
build_mingw.bat
```

### Option 3: Manual compilation

```bash
cd engine
g++ -O3 -std=c++20 -ffast-math -march=native src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c -o patricia.exe -static-libgcc -static-libstdc++
```

## Build Options

- **Optimization**: `-O3` (maximum optimization)
- **C++ Standard**: `C++20`
- **Math**: `-ffast-math` (fast floating point operations)
- **Architecture**: `-march=native` (optimize for your CPU)
- **Linking**: Static linking for libgcc and libstdc++ (portable executable)

## Troubleshooting

### Error: "g++: command not found"
- Make sure MinGW is installed and added to PATH
- Try using full path: `C:\mingw64\bin\g++.exe`

### Error: "std::array file not found" or similar
- This is usually a linter/IDE issue, not a compiler issue
- The code should compile fine with MinGW

### Error: "undefined reference to..."
- Make sure all source files are included:
  - `src/patricia.cpp`
  - `src/learning.cpp` (newly added)
  - `src/fathom/src/tbprobe.c`

## Output

The compiled executable will be `patricia.exe` in the `engine` directory.

