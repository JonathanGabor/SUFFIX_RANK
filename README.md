# SUFFIX RANK
## Suffix sorting for large inputs

### To Run
```
cd src
make
./suffixrank.sh INPUT_FOLDER [CHUNK_SIZE] [WORD_LENGTH] [--verify]
```
CHUNK_SIZE:  positive power of 2 (default 16777216).
  Note that the algorithm will use 28 times this amount of RAM in bytes (so by default, it will use ~470 MB).

WORD_LENGTH: bytes per symbol, 1..4 (default 1)

--verify: verify the correctness of the suffix array after creation

Sample input folder can be downloaded from [here](https://drive.google.com/file/d/1B9muEMI97aF8-Zj_SCxHzA1tMCjtNCbR/view).  Larger inputs are available [here](https://barsky.ca/marina/SR/suffix_rank/index.html#datasets-link).

The output suffix array will be in the "output" folder.
