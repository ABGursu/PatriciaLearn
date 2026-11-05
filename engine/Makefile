.PHONY: build datagen aarch64 cross-aarch64 cross-aarch64-clang mingw msvc clang

EXE := patricia
SOURCES := src/patricia.cpp src/learning.cpp src/fathom/src/tbprobe.c

# Include path for source files (relative to engine directory)
INCLUDES := -Isrc

CXX := clang++
CXXFLAGS := -O3 -march=native -std=c++20 -ffast-math $(INCLUDES)
LINKER :=

ifeq ($(OS), Windows_NT)
	DETECTED_OS := Windows
	SUFFIX := .exe
	LINKER := -static
else
	DETECTED_OS := $(shell uname -s)
	LINKER := -lm
	CXXFLAGS += -pthread
	SUFFIX :=
endif

OUT := $(EXE)$(SUFFIX)

# --- Native build ----------------------------------------------------------
# Note: Run 'make' from the engine directory
build: $(SOURCES)
	@echo Building from: $(shell pwd)
	@echo Sources: $(SOURCES)
	$(CXX) $^ $(CXXFLAGS) -o $(OUT) $(LINKER)

default: build

datagen: datagen/datagen.cpp
	$(CXX) $^ $(CXXFLAGS) -o data $(LINKER)

# --- Native AArch64 build (on ARM host) -----------------------------------
aarch64: CXX := clang++
aarch64: CXXFLAGS := -O3 -std=c++20 -ffast-math -mcpu=native -pthread -static-libstdc++ -static-libgcc
aarch64: OUT := $(EXE)_aarch64$(SUFFIX)
aarch64: $(SOURCES)
	$(CXX) $^ $(CXXFLAGS) -o $(OUT) $(LINKER)

# --- Cross-compile for AArch64 (fully static) -----------------------------
# Usage: make cross-aarch64 CROSS=aarch64-linux-gnu
cross-aarch64: CROSS ?= aarch64-linux-gnu
cross-aarch64: CXX := $(CROSS)-g++
cross-aarch64: CXXFLAGS := -O3 -std=c++20 -ffast-math -march=armv8-a -pthread -static -static-libstdc++ -static-libgcc
cross-aarch64: OUT := $(EXE)_aarch64$(SUFFIX)
cross-aarch64: $(SOURCES)
	$(CXX) $(CXXFLAGS) $^ -o $(OUT)

# --- Cross-compile for AArch64 using clang (requires proper sysroot) -----
cross-aarch64-clang: CXX := clang++
cross-aarch64-clang: CXXFLAGS := --target=aarch64-linux-gnu -O3 -std=c++20 -ffast-math -march=armv8-a -pthread -static
cross-aarch64-clang: OUT := $(EXE)_aarch64$(SUFFIX)
cross-aarch64-clang: $(SOURCES)
	$(CXX) $(CXXFLAGS) $^ -o $(OUT)

# --- MinGW build (Windows) -------------------------------------------------
# Note: Run 'make mingw' from the engine directory
mingw: CXX := g++
mingw: CXXFLAGS := -O3 -std=c++20 -ffast-math -march=native $(INCLUDES)
mingw: SUFFIX := .exe
mingw: OUT := $(EXE)$(SUFFIX)
mingw: $(SOURCES)
	@echo Building from: $(shell pwd)
	@echo Sources: $(SOURCES)
	$(CXX) $(CXXFLAGS) $^ -o $(OUT) -static

# --- MSVC build (Windows) --------------------------------------------------
# Note: Run 'make msvc' from the engine directory
# Requires Visual Studio Developer Command Prompt
msvc: CXX := cl
msvc: CXXFLAGS := /O2 /std:c++20 /EHsc /W3 /I src
msvc: SUFFIX := .exe
msvc: OUT := $(EXE)$(SUFFIX)
msvc: $(SOURCES)
	@echo Building from: $(shell pwd)
	@echo Sources: $(SOURCES)
	$(CXX) $(CXXFLAGS) $^ /Fe:$(OUT)

# --- Clang build (Windows) -------------------------------------------------
# Note: Run 'make clang' from the engine directory
clang: CXX := clang++
clang: CXXFLAGS := -O3 -std=c++20 -ffast-math -march=native $(INCLUDES)
clang: SUFFIX := .exe
clang: OUT := $(EXE)$(SUFFIX)
clang: $(SOURCES)
	@echo Building from: $(shell pwd)
	@echo Sources: $(SOURCES)
	$(CXX) $(CXXFLAGS) $^ -o $(OUT)
