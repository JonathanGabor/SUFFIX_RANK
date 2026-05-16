# SUFFIX RANK
## Suffix sorting for large inputs

### To Run
```
cd src
make
./suffixrank.sh INPUT_FOLDER [CHUNK_SIZE] [WORD_LENGTH]
```
CHUNK_SIZE:  positive power of 2 (default 16777216).
  Note that the algorithm will use 40 times this amount of RAM.

WORD_LENGTH: bytes per symbol, 1..4 (default 1)

Sample input folder can be downloaded from [here](https://drive.google.com/file/d/1B9muEMI97aF8-Zj_SCxHzA1tMCjtNCbR/view).

### Test Correctness

```
cd correctness_test
./test_correctness.sh <input_text_dir> <sa_output_dir> [word_length]
```
Note that the correctness test takes a long time to run.
