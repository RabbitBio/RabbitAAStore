# sdsl-protein Usage Guide

## Build
```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```
> GCC 8 requires linking `stdc++fs`, handled by CMakeLists.txt.

## Executable
`build/test`

## Command Line Arguments
- `-f, --fasta <file>`: Input FASTA file (mutually exclusive with `--load`)
- `-l, --load <file|dir>`: Load from binary archive; if directory, default filename `protein_store.bin`
- `-o, --output <file|dir>`: Save built/loaded store; if directory, uses default filename
- `-L, --min-length <uint>`: Minimum sequence length filter (sequences shorter than this will be skipped, default 0 = no filtering)

## Storage Format (Reference `ProteinAAStore`)
- 5-bit encoded sequences: `data_` (`sdsl::int_vector<5>`)
- Start positions array: `start_positions_` (start index of each sequence, with sentinel at end, length = `n_ + 1`)
- Number of sequences: `n_`
- `save()` write order: `n_` → `start_positions_` size and data → `data_.serialize`
- `load()` reads in same order, validates `start_positions_.size() == n_ + 1`

## Using This Format
If others want to use this storage format, simply copy the `src/ProteinAAStore.hpp` file and the relevant SDSL library header files. The `ProteinAAStore` class is self-contained and depends on SDSL's `int_vector` and serialization functionality.

## Main Features
- `build(seqs)` / `add(seq)` / `finalize()`: Build and incremental addition
-`add(seq)`: When inserting sequences incrementally, pre-reserving enough capacity using `init(size)` for the `int_vector` is strongly recommended to avoid costly `resize()`-induced reallocations.
- `get(i)`: Return i-th sequence (string)
- `length(i)`: Return length of i-th sequence
- `checksum(i)`: Lightweight access (no string allocation) summing encoded values
- `total_length()`: Total length of all sequences
- `memory_usage()` / `memory_breakdown()`: Memory usage estimation
- `save(path)` / `load(path)`: Binary archive read/write

## Usage Examples
```bash
# Build from FASTA and save
./test -f /path/input.fa -o /tmp/store_dir

# Load from archive and test
./test -l /tmp/store_dir
```
