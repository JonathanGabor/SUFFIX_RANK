# SUFFIX RANK
## Suffix sorting for large inputs

SUFFIX_RANK is an out-of-core suffix array construction implementation. The pipeline uses a libdivsufsort-based initial partial suffix sort inspired by SAscan, followed by prefix-doubling rank refinement across chunks.

### To Run
```
cd src
make
./suffixrank.sh INPUT_FILE [MEMORY_LIMIT] [--chunk-size N] [--verify]
```
`INPUT_FILE`: the single file to index (the string to build the suffix array for).

`MEMORY_LIMIT`: working-RAM budget, given with a human-readable suffix (`8G`, `512M`,
  `2048K`) or as plain bytes. Default `335544320` (~336 MB). The in-memory chunk
  size (in elements) is derived as `MEMORY_LIMIT / 20`, since total working RAM is
  about `20 x chunk_size` bytes.

`--chunk-size N`: pin the chunk size (elements) directly, bypassing the
  `MEMORY_LIMIT` derivation (handy for benchmarking). The chunk size need **not**
  be a power of two.

`--verify`: verify the correctness of the suffix array after creation.

**Note:** For inputs much larger than the chunk size, you may need to run
`ulimit -n 1024` (or higher) to increase the open-file descriptor limit.

Works on byte alphabets only, and on a **single input file** (one string with one
trailing sentinel). Multi-file input is not supported.

### Dependency

Install [y-256/libdivsufsort](https://github.com/y-256/libdivsufsort) before building. The Makefile defaults to `DIVSUFSORT_PREFIX=/usr/local`; override it on the command line if needed:

```
make DIVSUFSORT_PREFIX=/path/to/libdivsufsort/install
```

For a persistent local override, create `src/Makefile.local`

```
DIVSUFSORT_PREFIX = /path/to/libdivsufsort/install
```

You can also override `DSS_INC` and `DSS_LIB` directly there if your install layout is unusual.

Sample inputs can be downloaded from [here](https://drive.google.com/file/d/1B9muEMI97aF8-Zj_SCxHzA1tMCjtNCbR/view) (pick a single file from the archive).  Larger inputs are available [here](https://barsky.ca/marina/SR/suffix_rank/index.html#datasets-link).

The output suffix array will be in the "output" folder.

### Input encoding

The input is a single file, treated as one string with one trailing sentinel.

The initializer assigns each position an initial rank from a **k-mer bucket sort**:
it scans the file to find which byte values occur, picks the largest `k` whose
k-mer histogram fits the RAM budget, and ranks every position by its first `k`
characters. This lets the prefix-doubling loop start at prefix length `k` (and
then double: `k, 2k, 4k, ...`) instead of at 1, skipping the early iterations.

For divsufsort, the initializer also writes byte chunks: byte code `0` is reserved
for the sentinel, real input bytes are shifted up by 1, and code `255` is left for
the SAscan Z-transform increment — so every input byte must be `<= 253`.

### SAscan acknowledgement

The initial partial suffix sorting pass uses ideas and small helper code adapted from SAscan:

Juha Karkkainen and Dominik Kempa, "Engineering a Lightweight External Memory Suffix Array Construction Algorithm", Proc. 2nd International Conference on Algorithms for Big Data (ICABD), 2014.

The SAscan code used as reference is MIT licensed:

Copyright (c) 2014 Juha Karkkainen and Dominik Kempa. Permission is granted, free of charge, to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the software, subject to inclusion of the copyright and permission notice. The software is provided "as is", without warranty of any kind.
