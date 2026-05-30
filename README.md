# SUFFIX RANK
## Suffix sorting for large inputs

SUFFIX_RANK is an out-of-core suffix array construction implementation. The default pipeline uses a libdivsufsort-based initial partial suffix sort inspired by SAscan, followed by prefix-doubling rank refinement across chunks.

### To Run
```
cd src
make
./suffixrank.sh INPUT_FOLDER [CHUNK_SIZE] [--verify]
```
CHUNK_SIZE:  positive power of 2 (default 16777216).
  Note that the algorithm will use 28 times this amount of RAM in bytes (so by default, it will use ~31 MB).

--verify: verify the correctness of the suffix array after creation

The divsufsort path currently works on byte alphabets only.

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

Sample input folder can be downloaded from [here](https://drive.google.com/file/d/1B9muEMI97aF8-Zj_SCxHzA1tMCjtNCbR/view).  Larger inputs are available [here](https://barsky.ca/marina/SR/suffix_rank/index.html#datasets-link).

The output suffix array will be in the "output" folder.

### Input encoding

Files in `INPUT_FOLDER` are processed in sorted lexicographic filename order. Each file becomes one logical string and gets a distinct trailing sentinel.

The default initializer reserves byte codes for those sentinels before passing chunks to divsufsort, then shifts real input bytes above that reserved range. With `N` input files, real input bytes must be `<= 254 - N` so there is room for all sentinel codes plus the SAscan Z-transform increment.

### SAscan acknowledgement

The initial partial suffix sorting pass uses ideas and small helper code adapted from SAscan:

Juha Karkkainen and Dominik Kempa, "Engineering a Lightweight External Memory Suffix Array Construction Algorithm", Proc. 2nd International Conference on Algorithms for Big Data (ICABD), 2014.

The SAscan code used as reference is MIT licensed:

Copyright (c) 2014 Juha Karkkainen and Dominik Kempa. Permission is granted, free of charge, to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the software, subject to inclusion of the copyright and permission notice. The software is provided "as is", without warranty of any kind.
