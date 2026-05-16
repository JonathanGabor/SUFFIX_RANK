#!/bin/bash
# Usage: ./test_correctness.sh <input_text_dir> <sa_output_dir> [word_length]
set -e
INPUT_FOLDER=$1
SA_FOLDER=$2
WORD_LENGTH="${3:-1}"

if [[ -z "$INPUT_FOLDER" || -z "$SA_FOLDER" ]]; then
    echo "Usage: $0 <input_text_dir> <sa_output_dir> [word_length]"
    exit 1
fi

gcc -O3 -Wall -o test_sa utils.c test_sa.c
cat $(find "${SA_FOLDER}" -name "suffixarray*" | sort -V) > final_sa
./test_sa "${INPUT_FOLDER}" final_sa "${WORD_LENGTH}"
