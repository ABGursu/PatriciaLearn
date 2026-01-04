# Patricia Chess Engine 5.2: Learning System Technical Documentation

**Version:** 1.0  
**Date:** 2025  
**Author:** A. B. Gursu  
**Based on:** Patricia Chess Engine 5.2 by Adam Kulju

---

## Table of Contents

1. [Introduction](#introduction)
2. [System Architecture](#system-architecture)
3. [Data Structures](#data-structures)
4. [How Learning Works](#how-learning-works)
5. [Experience Book](#experience-book)
6. [File Format](#file-format)
7. [UCI Commands](#uci-commands)
8. [Search Integration](#search-integration)
9. [Performance Considerations](#performance-considerations)
10. [Usage Examples](#usage-examples)
11. [Technical Details](#technical-details)

---

## Introduction

The Learning system in Patricia Chess Engine 5.2 is an advanced experience-based learning mechanism that allows the engine to learn from its own games and improve its play over time. The system stores position evaluations, move choices, and search depths from previous games, creating a persistent knowledge base that enhances move ordering, provides opening book functionality, and improves overall playing strength.

### Key Features

- **Persistent Learning**: Stores experience data across engine sessions
- **Asynchronous Loading**: Experience files load in background threads without blocking search
- **Experience Book**: Automatic move selection from learned positions (opening book functionality)
- **Move Ordering Enhancement**: Uses experience to improve search efficiency
- **Multi-PV Support**: Learns from multiple principal variations
- **File Compatibility**: Compatible with HypnoS experience file format
- **Thread-Safe**: Fully thread-safe implementation for multi-threaded search

### Benefits

1. **Improved Opening Play**: Experience Book provides instant moves for known positions
2. **Better Move Ordering**: Experience data guides search toward previously successful moves
3. **Reduced Search Time**: Known positions can skip search entirely
4. **Adaptive Play**: Engine learns from its games and adapts its strategy
5. **Knowledge Accumulation**: Experience grows over time, making the engine stronger

---

## System Architecture

### Overview

The Learning system is implemented as a namespace (`Learning::`) with the following key components:

```
Learning System
├── ExperienceData (Core storage class)
│   ├── Hash Map (dense_hash_map for fast lookups)
│   ├── Entry Lists (Linked lists for multiple moves per position)
│   ├── New Experience Buffers (PV and MultiPV)
│   └── Background Loader Thread
├── File I/O (Binary format with signature)
├── Experience Book (Move selection system)
└── UCI Interface (Commands and options)
```

### Core Components

#### 1. ExperienceData Class

The `ExperienceData` class is the heart of the learning system. It manages:

- **Hash Map Storage**: Uses Google's `dense_hash_map` for O(1) position lookups
- **Entry Linking**: Multiple moves per position stored as linked lists
- **Background Loading**: Asynchronous file loading in separate thread
- **Write Buffering**: 16MB write buffer for efficient disk I/O
- **Entry Merging**: Automatic deduplication and merging of experience entries

#### 2. Entry Structure

Each experience entry (`ExpEntry`) contains:

- **Zobrist Key** (8 bytes): Unique position identifier
- **Move** (4 bytes): The move played from this position
- **Value** (4 bytes): Evaluation score in centipawns
- **Depth** (4 bytes): Search depth at which this move was found
- **Count** (2 bytes): Number of times this move was encountered
- **Padding** (2 bytes): Alignment to 24 bytes total

#### 3. Extended Entry Structure

`ExpEntryEx` extends `ExpEntry` with:

- **Next Pointer**: Links multiple moves for the same position
- **Quality Functions**: Calculates move quality for selection
- **Positional Quality**: Advanced quality calculation with lookahead

---

## Data Structures

### ExpEntry Structure

```cpp
struct ExpEntry {
  uint64_t key;      // Zobrist key (8 bytes)
  int32_t move;      // Move stored as int32_t (4 bytes)
  int32_t value;     // Score in centipawns (4 bytes)
  int32_t depth;     // Search depth (4 bytes)
  uint16_t count;    // Encounter count (2 bytes)
  uint8_t padding[2]; // Padding to 24 bytes (2 bytes)
};
```

**Total Size**: 24 bytes (fixed for file format compatibility)

### ExpEntryEx Structure

```cpp
struct ExpEntryEx : ExpEntry {
  ExpEntryEx* next;  // Linked list pointer for multiple moves
};
```

**Key Methods**:
- `merge()`: Merges two entries (combines counts, updates value/depth if better)
- `compare()`: Compares entry quality (for sorting)
- `quality()`: Calculates simple quality score
- `quality_positional()`: Advanced quality with lookahead evaluation
- `find()`: Finds specific move in linked list

### Storage Organization

```
Hash Map (Zobrist Key → ExpEntryEx*)
  └── Position Key 1
      └── ExpEntryEx (Move A) → ExpEntryEx (Move B) → ExpEntryEx (Move C)
  └── Position Key 2
      └── ExpEntryEx (Move X) → ExpEntryEx (Move Y)
```

Each position can have multiple moves stored as a linked list, sorted by quality.

---

## How Learning Works

### 1. Experience Collection

Experience is collected during search in two ways:

#### A. Principal Variation (PV) Experience

Collected from the best move found at each search iteration:

```cpp
if (depth >= MinDepth && best_move_found) {
  Learning::add_pv_experience(
    position.zobrist_key,
    best_move,
    best_score,
    search_depth
  );
}
```

**Conditions**:
- Minimum depth: 4 plies (configurable via `MinDepth`)
- Only from main search thread (thread_id == 0)
- Not during data generation mode
- Learning must not be paused

#### B. Multi-PV Experience

Collected when `MultiPV > 1`, storing alternative moves from all threads:

```cpp
if (MultiPV > 1) {
  // Collect unique moves from all threads
  // Store moves with depth >= MinDepth
  Learning::add_multipv_experience(
    position.zobrist_key,
    alternative_move,
    alternative_score,
    alternative_depth
  );
}
```

**Benefits**:
- Learns alternative lines, not just best moves
- Provides more diverse experience data
- Useful for opening book variety

### 2. Experience Storage

#### In-Memory Storage

New experience entries are stored in temporary buffers:

- `_newPvExp`: Principal variation entries
- `_newMultiPvExp`: Multi-PV entries

These are linked into the main hash map immediately for fast access during search.

#### Entry Linking Algorithm

When a new entry is added:

1. **Check if position exists**: Lookup in hash map
2. **If new position**: Insert as head of new linked list
3. **If position exists**:
   - Check if same move exists → **Merge** (update count, value, depth)
   - If different move → **Insert sorted** by quality (compare function)

#### Quality Comparison

Entries are compared using a scaled quality metric:

```cpp
scaled_quality = value * max(depth/10, 1) * max(count/3, 1)
```

Comparison order:
1. Scaled quality (primary)
2. Count (secondary)
3. Depth (tertiary)

### 3. Experience Persistence

#### Saving to Disk

Experience is saved to disk:

- **After each search**: New entries appended to file
- **On engine shutdown**: All new entries saved
- **On file change**: Previous entries saved before loading new file

#### Save Process

1. **Deduplication**: Remove duplicate entries using hash set
2. **Filtering**: Only entries with `depth >= MinDepth` are saved
3. **Buffered Write**: 16MB buffer for efficient disk I/O
4. **Append Mode**: New entries appended to existing file

#### File Format

See [File Format](#file-format) section for detailed specification.

### 4. Experience Retrieval

#### Probing Experience

```cpp
const ExpEntryEx* exp = Learning::probe(position.zobrist_key);
```

Returns the head of the linked list for the position, or `nullptr` if not found.

#### Finding Best Entry

```cpp
const ExpEntryEx* best = Learning::find_best_entry(position.zobrist_key);
```

Iterates through linked list to find highest quality entry.

---

## Experience Book

The Experience Book is an advanced feature that uses learned experience as an opening book, automatically selecting moves from known positions without searching.

### How It Works

1. **Position Check**: When search starts, check if position has experience data
2. **Quality Calculation**: Calculate quality scores for all moves meeting minimum depth
3. **Move Selection**: Select move based on quality and width settings
4. **Instant Move**: Return move immediately, skipping search entirely

### Quality Calculation

The Experience Book uses **positional quality with lookahead**:

```cpp
quality = base_quality + evaluation_improvement_score
```

**Base Quality**: `count * (10 - evalImportance)`

**Evaluation Improvement**: Calculated by:
1. Playing the move and looking ahead up to 10 moves
2. Tracking evaluation changes for both sides
3. Weighting by `evalImportance` (0-10)

**Draw Detection**: Positions leading to draws are filtered out.

### Configuration Options

#### Experience Book Enabled
- **Type**: Check (boolean)
- **Default**: `false`
- **Description**: Enable/disable Experience Book functionality

#### Experience Book Width
- **Type**: Spin (integer)
- **Default**: `1`
- **Range**: 1-20
- **Description**: Number of top moves to randomly select from
  - `1`: Always play best move
  - `>1`: Random selection from top N moves (adds variety)

#### Experience Book Eval Importance
- **Type**: Spin (integer)
- **Default**: `5`
- **Range**: 0-10
- **Description**: Weight given to evaluation differences vs. move count
  - `0`: Only count matters (most played moves)
  - `10`: Only evaluation matters (best scoring moves)
  - `5`: Balanced (default)

#### Experience Book Min Depth
- **Type**: Spin (integer)
- **Default**: `27`
- **Range**: 4-64
- **Description**: Minimum search depth required for book moves
  - Higher = more reliable but fewer positions
  - Lower = more positions but less reliable

#### Experience Book Max Moves
- **Type**: Spin (integer)
- **Default**: `16`
- **Range**: 1-100
- **Description**: Maximum number of moves into game to use Experience Book
  - Prevents using book in endgame
  - Measured in half-moves (ply)

### Usage Example

```uci
setoption name Experience Book Enabled value true
setoption name Experience Book Width value 3
setoption name Experience Book Eval Importance value 7
setoption name Experience Book Min Depth value 25
setoption name Experience Book Max Moves value 20
```

This configuration:
- Enables Experience Book
- Selects randomly from top 3 moves
- Prioritizes evaluation (70%) over count (30%)
- Requires minimum depth 25
- Uses book for first 20 moves

---

## File Format

### File Structure

```
[Signature String] (variable length)
[Entry 1] (24 bytes)
[Entry 2] (24 bytes)
...
[Entry N] (24 bytes)
```

### Signature

**PatriciaLearn Signature**: `"SugaR Experience version 2"`  
**HypnoS Compatibility**: Also accepts HypnoS signature for file compatibility

The signature is written as a null-terminated string at the beginning of the file.

### Entry Format

Each entry is exactly 24 bytes:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 8 | `key` | Zobrist key (uint64_t) |
| 8 | 4 | `move` | Move (int32_t) |
| 12 | 4 | `value` | Score in centipawns (int32_t) |
| 16 | 4 | `depth` | Search depth (int32_t) |
| 20 | 2 | `count` | Encounter count (uint16_t) |
| 22 | 2 | `padding` | Padding bytes (0x00) |

### File Operations

#### Loading

1. Open file in binary mode
2. Read and verify signature
3. Calculate entry count: `(file_size - signature_length) / 24`
4. Allocate memory for entries
5. Read entries sequentially
6. Link entries into hash map (merge duplicates)

#### Saving

1. Open file in append mode
2. If new file: Write signature
3. Filter entries (depth >= MinDepth)
4. Deduplicate using hash set
5. Write entries in 16MB buffered chunks
6. Flush buffer

#### Defragmentation

The `defrag` command:
1. Loads entire experience file
2. Merges duplicate entries
3. Re-saves file (removes fragmentation)
4. Reduces file size by removing duplicates

---

## UCI Commands

### UCI Options

#### Experience Enabled
```uci
option name Experience Enabled type check default true
```
Enable/disable the entire Learning system.

#### Experience File
```uci
option name Experience File type string default patricia.exp
```
Filename for experience data file. Can be changed at runtime.

#### Experience Readonly
```uci
option name Experience Readonly type check default false
```
If enabled, prevents writing new experience data (read-only mode).

#### Experience Book Options
See [Experience Book](#experience-book) section for all book-related options.

### UCI Commands

#### defrag
**Syntax**: `defrag [filename]`

Defragments an experience file by:
- Loading all entries
- Merging duplicates
- Re-saving in optimized format

If no filename is provided, uses the "Experience File" option value.

**Example**:
```uci
defrag patricia.exp
```

Or without filename (uses Experience File option):
```uci
defrag
```

**Output**:
```
Defragmenting experience file: patricia.exp
info string Defragmentation complete
```

#### merge
**Syntax**: `merge <target_file> <source_file1> [source_file2] ...`

Merges multiple experience files into one target file.

**Example**:
```uci
merge combined.exp file1.exp file2.exp file3.exp
```

**Output**:
```
Merging experience files: 
	combined.exp
	file1.exp
	file2.exp
	file3.exp
Target file: combined.exp
info string Merge complete
```

**Use Cases**:
- Combining experience from multiple engines
- Merging training data
- Consolidating fragmented files

#### exp
**Syntax**: `exp`

Displays experience data for the current position (synthetic view).

**Example Output**:
```
Experience: 

 1: e2e4, depth: 15, eval: cp 25
 2: d2d4, depth: 14, eval: cp 20
 3: g1f3, depth: 13, eval: cp 15
```

#### expex
**Syntax**: `expex`

Displays extended experience data with quality scores and counts (extended view).

**Example Output**:
```
info string Experience for position key: 12345678901234567890

 1: e2e4, depth: 15, eval: cp 25, count:  125, quality: 450
 2: d2d4, depth: 14, eval: cp 20, count:   98, quality: 380
 3: g1f3, depth: 13, eval: cp 15, count:   76, quality: 320
```

**Fields**:
- **Move**: Algebraic notation
- **Depth**: Search depth
- **Eval**: Evaluation in centipawns (or mate score)
- **Count**: Number of times encountered
- **Quality**: Calculated quality score

---

## Search Integration

### Move Ordering Enhancement

When experience is enabled (but Experience Book is disabled), the engine uses experience data to improve move ordering:

```cpp
if (Learning::enabled() && !useExpBookMove) {
  const auto* bestExp = Learning::find_best_entry(position.zobrist_key);
  if (bestExp && bestExp->depth >= MinDepth) {
    // Move experience move to front of root moves
    // This improves search efficiency
  }
}
```

**Benefits**:
- Better move ordering → faster search
- More likely to find best move early
- Reduced search tree size

### Experience Book Integration

When Experience Book is enabled:

1. **Before Search**: Check if position has experience data
2. **Quality Calculation**: Calculate quality for all moves
3. **Move Selection**: Select move based on width and quality
4. **Skip Search**: Return move immediately if found

```cpp
if (useExpBookMove) {
  printf("bestmove %s\n", move);
  return; // Skip entire search
}
```

### Learning During Search

Experience is collected:

1. **After Each Iteration**: PV experience saved
2. **Multi-PV Mode**: Alternative moves also saved
3. **End of Search**: All experience flushed to file

**Thread Safety**:
- Only main thread (thread_id == 0) saves experience
- Helper threads contribute to Multi-PV data
- Atomic operations prevent race conditions

---

## Performance Considerations

### Memory Usage

**Hash Map Overhead**: ~24 bytes per unique position + pointer overhead

**Example**: 1 million positions ≈ 30-40 MB RAM

**Optimization**: Uses `dense_hash_map` for minimal overhead

### Disk I/O

**Write Buffer**: 16MB buffer reduces disk writes

**Append Mode**: New entries appended (no file rewriting)

**Background Loading**: File loading in separate thread (non-blocking)

### Search Performance

**Lookup Speed**: O(1) hash map lookup (very fast)

**Move Ordering**: Minimal overhead, significant benefit

**Experience Book**: Instant moves (zero search time)

### Benchmarks

Typical performance impact:

- **Experience Enabled**: < 1% search overhead
- **Experience Book**: 0% overhead (skips search)
- **File Loading**: Non-blocking (background thread)
- **File Saving**: < 10ms per search (buffered writes)

---

## Usage Examples

### Basic Setup

```uci
# Enable learning
setoption name Experience Enabled value true

# Set experience file
setoption name Experience File value my_experience.exp

# Start playing (experience will be collected automatically)
position startpos
go movetime 1000
```

### Experience Book Setup

```uci
# Enable Experience Book
setoption name Experience Book Enabled value true
setoption name Experience Book Width value 1
setoption name Experience Book Min Depth value 25

# Now engine will use book moves for known positions
position startpos
go movetime 1000
```

### Combining Experience Files

```uci
# Merge multiple experience files
merge combined.exp engine1.exp engine2.exp engine3.exp

# Use combined file
setoption name Experience File value combined.exp
```

### Defragmenting Experience File

```uci
# Defragment to reduce file size and improve performance
defrag patricia.exp
```

### Viewing Experience Data

```uci
# Set up a position
position startpos moves e2e4 e7e5

# View experience for this position
exp

# View extended information
expex
```

### Read-Only Mode

```uci
# Enable read-only mode (no learning, only using existing data)
setoption name Experience Readonly value true

# Engine will use experience but not save new data
```

---

## Technical Details

### Thread Safety

The Learning system is fully thread-safe:

- **Hash Map**: Protected by internal locking (dense_hash_map is thread-safe for reads)
- **Write Operations**: Only main thread writes (thread_id == 0)
- **Background Loading**: Separate thread with mutex protection
- **Atomic Flags**: Learning pause/resume uses atomic operations

### Entry Merging Algorithm

When the same move is encountered multiple times:

```cpp
void merge(const ExpEntry* exp) {
  // Merge count (capped at uint16_t max)
  count = min(count + exp->count, 65535);
  
  // Update value/depth if new entry is better
  if (depth == exp->depth) {
    value = (value + exp->value) / 2;  // Average
  } else if (depth < exp->depth) {
    value = exp->value;  // Use better depth
    depth = exp->depth;
  }
}
```

### Quality Calculation Details

#### Simple Quality

```cpp
quality = count * (10 - evalImportance) + (value * evalImportance) / 100
```

#### Positional Quality (with Lookahead)

1. Play move and look ahead up to 10 moves
2. Track evaluation changes for both sides
3. Calculate weighted improvement:
   ```
   improvement = sum(our_eval_changes) - sum(opponent_eval_changes)
   quality = base_quality + (improvement * evalImportance) / weight
   ```

### Minimum Depth Filtering

Only entries with `depth >= MinDepth` (default 4) are:
- Saved to disk
- Used for move ordering
- Considered for Experience Book

**Rationale**: Shallow searches are unreliable and can pollute experience data.

### File Compatibility

The system is compatible with HypnoS experience files:

- **Signature Detection**: Accepts both PatriciaLearn and HypnoS signatures
- **Format Compatibility**: Same 24-byte entry format
- **Seamless Loading**: HypnoS files load without conversion

### Bench Mode

Special handling for benchmark mode:

- **Single-Shot Write**: Only first PV experience saved (prevents file growth during benchmarks)
- **Multi-PV Disabled**: No Multi-PV experience during benchmarks
- **Touch File**: Creates empty file with signature (for testing)

---

## Conclusion

The Learning system in Patricia Chess Engine 5.2 is a sophisticated experience-based learning mechanism that:

1. **Learns from Games**: Automatically collects experience during play
2. **Improves Performance**: Enhances move ordering and provides instant book moves
3. **Persists Knowledge**: Saves experience across sessions
4. **Provides Flexibility**: Configurable options for different use cases
5. **Maintains Compatibility**: Works with HypnoS experience files

The system is designed for both casual users (automatic learning) and advanced users (fine-tuned configuration), making it a powerful tool for improving engine strength and playing style.

---

## Appendix: Code References

### Key Files

- `src/learning.h`: Learning system interface and data structures
- `src/learning.cpp`: Learning system implementation
- `src/search.h`: Search integration (experience collection and usage)
- `src/uci.h`: UCI command parsing and option handling

### Key Functions

- `Learning::init()`: Initialize learning system
- `Learning::probe()`: Lookup experience for position
- `Learning::add_pv_experience()`: Add principal variation experience
- `Learning::add_multipv_experience()`: Add multi-PV experience
- `Learning::save()`: Save experience to disk
- `Learning::find_best_entry()`: Find best move for position

---

**End of Documentation**

