#!/bin/bash

# Drives the default divsufsort/SAscan-based pipeline.

if date +%s.%N &>/dev/null; then
    timestamp() { date +%s.%N; }
else
    timestamp() { perl -MTime::HiRes=time -e 'printf "%.9f\n", time'; }
fi
DATE=timestamp

#constants
TEMP_DIR=tmp
RANK_DIR=ranks
OUTPUT_DIR=output
CHUNKS_DIR=chunks

SUCCESS=0
FAILURE=1
EMPTY=2

STATE=$SUCCESS
CHUNKS=0

TRUESTART=$($DATE)

# Pull out --verify (may appear anywhere); remaining args stay positional.
RUN_VERIFY=0
POSITIONAL=()
for arg in "$@"; do
    if [[ "$arg" == "--verify" ]]; then
        RUN_VERIFY=1
    else
        POSITIONAL+=("$arg")
    fi
done
set -- "${POSITIONAL[@]}"

if [[ -z "$1" ]] || [[ ! -d "$1" ]]
then
    echo "Usage: $0 INPUT_FOLDER [CHUNK_SIZE] [--verify]"
    echo "  CHUNK_SIZE:  positive power of 2 (default 1048576)"
    echo "  --verify:    run external-memory correctness checker after pipeline"
    exit 1
fi

INPUT_DIR=$(cd "$1" && pwd)

cd "$(dirname "$0")"

CHUNK_SIZE="${2:-1048576}"
if ! [[ "$CHUNK_SIZE" =~ ^[0-9]+$ ]] || (( CHUNK_SIZE <= 0 )); then
    echo "Invalid chunk size '$CHUNK_SIZE': must be a positive integer"
    exit 1
fi
if (( (CHUNK_SIZE & (CHUNK_SIZE - 1)) != 0 )); then
    echo "Invalid chunk size $CHUNK_SIZE: must be a power of 2"
    exit 1
fi

echo "Using chunk size: $CHUNK_SIZE (byte alphabet, divsufsort path)"

#prepare directory structure for processing
for dir in "$RANK_DIR" "$OUTPUT_DIR" "$TEMP_DIR" "$CHUNKS_DIR"; do
    if [[ -d "$dir" ]]; then
        rm -rf "${dir:?}"/*
    else
        mkdir "$dir"
    fi
done

#Part 1. Initial partial suffix sort: read source files directly, write ranks_* and sa_* chunks.
START=$($DATE)
./init "$INPUT_DIR" "$RANK_DIR" "$CHUNKS_DIR" "$CHUNK_SIZE"
STATUS=$?

if [[ $STATUS -eq $FAILURE ]]
then
    exit 1
fi

CHUNKS=$(ls -1 "${RANK_DIR}"/sa_* 2>/dev/null | wc -l | tr -d ' ')

# merge currently keeps one input and one output FILE open per chunk.
OPEN_FILE_LIMIT=$(ulimit -n)
if [[ "$OPEN_FILE_LIMIT" != "unlimited" ]]; then
    REQUIRED_OPEN_FILES=$(( 2 * CHUNKS + 16 ))
    if (( OPEN_FILE_LIMIT < REQUIRED_OPEN_FILES )); then
        echo "Error: current open-file limit is too low for ${CHUNKS} chunks."
        echo "merge needs at least ${REQUIRED_OPEN_FILES} file descriptors, but ulimit -n is ${OPEN_FILE_LIMIT}."
        echo "Increase the limit, for example: ulimit -n ${REQUIRED_OPEN_FILES}"
        exit 1
    fi
fi

DUR=$(echo "$($DATE) - $START" | bc)
printf "Finished initializing in %.4f, total %d chunks\n" $DUR $CHUNKS


#set prefix length to 2^H
H=0

#Part 3. Perform O(log N) iterations of an algorithm
MORE_RUNS=1

while (( $MORE_RUNS == 1 ))
do
    MORE_RUNS=0;
    START=$($DATE)
    #clean temp directory for the next iteration
    rm -rf ${TEMP_DIR}/*

    ./refine ${RANK_DIR} ${TEMP_DIR} $CHUNKS $H $CHUNK_SIZE
    STATUS=$?

    if [[ $STATUS -ne $EMPTY ]]
    then
        MORE_RUNS=1
    fi

    if [[ $STATUS -eq $FAILURE ]]
    then
        exit 1
    fi
    DUR=$(echo "$($DATE) - $START" | bc)
    printf "Refined ranks for iteration %d in %.4f seconds\n" $H $DUR

    if [[ $MORE_RUNS -eq 1 ]]
    then
        START=$($DATE)
        ./merge ${TEMP_DIR} ${TEMP_DIR} $CHUNKS $CHUNK_SIZE
        STATUS=$?

        if [[ $STATUS -eq $FAILURE ]]
        then
            exit 1
        fi

        DUR=$(echo "$($DATE) - $START" | bc)
        printf "Resolved global ranks for iteration %d in %.4f seconds\n" $H $DUR

        if [[ $STATUS -ne $EMPTY ]]
        then
          START=$($DATE)
            ./update ${RANK_DIR} ${TEMP_DIR} $CHUNKS $H $CHUNK_SIZE
            STATUS=$?

            if [[ $STATUS -eq $FAILURE ]]
            then
                exit 1
            fi
          DUR=$(echo "$($DATE) - $START" | bc)
          printf "Updated ranks for iteration %d in %.4f seconds\n" $H $DUR
        fi
    fi

    echo "Finished iteration $H"


    echo
    (( H++ ))
done

#clean temp directory
rm -rf ${TEMP_DIR}/*

START=$($DATE)
./create_pairs ${RANK_DIR} ${TEMP_DIR} $CHUNKS $CHUNK_SIZE
STATUS=$?

if [[ $STATUS -eq $FAILURE ]]
then
    exit 1
fi
DUR=$(echo "$($DATE) - $START" | bc)
printf "Created in %.4f seconds\n" $DUR

START=$($DATE)


./invert ${TEMP_DIR} ${OUTPUT_DIR} $CHUNKS $CHUNK_SIZE
STATUS=$?

if [[ $STATUS -eq $FAILURE ]]
then
    exit 1
fi
DUR=$(echo "$($DATE) - $START" | bc)
printf "Inverted in %.4f seconds\n" $DUR

DUR=$(echo "$($DATE) - $TRUESTART" | bc)
printf "Total time: %.4f seconds\n\n" $DUR

if (( RUN_VERIFY == 1 )); then
    rm -rf ${TEMP_DIR}/*
    START=$($DATE)
    ./verify "$INPUT_DIR" "$RANK_DIR" "$OUTPUT_DIR" "$TEMP_DIR" $CHUNKS $CHUNK_SIZE
    STATUS=$?
    DUR=$(echo "$($DATE) - $START" | bc)
    printf "Verified in %.4f seconds\n" $DUR
    if [[ $STATUS -ne $SUCCESS ]]; then
        rm -rf ${TEMP_DIR}/*
        exit 1
    fi
fi

#clean temp directory
rm -rf ${TEMP_DIR}/*
rm -rf ${CHUNKS_DIR}/*

exit 0
