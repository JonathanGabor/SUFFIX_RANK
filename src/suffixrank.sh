#!/bin/bash


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

SUCCESS=0
FAILURE=1
EMPTY=2

STATE=$SUCCESS
CHUNKS=0

TRUESTART=$($DATE)

if [[ -z "$1" ]] || [[ ! -d "$1" ]]
then
    echo "Usage: $0 INPUT_FOLDER [CHUNK_SIZE] [WORD_LENGTH]"
    echo "  CHUNK_SIZE:  positive power of 2 (default 16777216)"
    echo "  WORD_LENGTH: bytes per symbol, 1..4 (default 1)"
    exit 1
fi

INPUT_DIR=$(cd "$1" && pwd)

# Run relative to the script's own directory so ./init, ranks/, tmp/, output/
# resolve correctly regardless of the caller's working directory.
cd "$(dirname "$0")"

# Parse optional chunk size (must be a positive power of 2).
CHUNK_SIZE="${2:-16777216}"
if ! [[ "$CHUNK_SIZE" =~ ^[0-9]+$ ]] || (( CHUNK_SIZE <= 0 )); then
    echo "Invalid chunk size '$CHUNK_SIZE': must be a positive integer"
    exit 1
fi
if (( (CHUNK_SIZE & (CHUNK_SIZE - 1)) != 0 )); then
    echo "Invalid chunk size $CHUNK_SIZE: must be a power of 2"
    exit 1
fi

# Parse optional word length (1..4).
WORD_LENGTH="${3:-1}"
if ! [[ "$WORD_LENGTH" =~ ^[0-9]+$ ]] || (( WORD_LENGTH < 1 || WORD_LENGTH > 4 )); then
    echo "Invalid word_length '$WORD_LENGTH': must be an integer in [1, 4]"
    exit 1
fi
echo "Using chunk size: $CHUNK_SIZE, word_length: $WORD_LENGTH"

#prepare directory structure for processing
for dir in "$RANK_DIR" "$OUTPUT_DIR" "$TEMP_DIR"; do
    if [[ -d "$dir" ]]; then
        rm -rf "${dir:?}"/*
    else
        mkdir "$dir"
    fi
done

#Part 1. Initial bucket sort: read source files directly, write ranks_* and sa_* chunks.
START=$($DATE)
./init "$INPUT_DIR" "$RANK_DIR" "$CHUNK_SIZE" "$WORD_LENGTH"
STATUS=$?

if [[ $STATUS -eq $FAILURE ]]
then
    exit 1
fi

CHUNKS=$(ls -1 "${RANK_DIR}"/sa_* 2>/dev/null | wc -l | tr -d ' ')

DUR=$(echo "$($DATE) - $START" | bc)
printf "Finished initializing in %.4f, total %d chunks\n" $DUR $CHUNKS


#set prefix length to 2^H
H=0

#Part 3. Perform O(log N) iterations of an algorithm
# main loop
MORE_RUNS=1

while (( $MORE_RUNS == 1 ))
do
    MORE_RUNS=0;
    START=$($DATE)
    #clean temp directory for the next iteration
    rm -rf ${TEMP_DIR}/*

    #generate sorted runs with counts and local rank pairs grouped by file_id and interval_id
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
    #only if there are ranks to be resolved - continue
    if [[ $MORE_RUNS -eq 1 ]]
    then
        START=$($DATE)
        #merge local ranks into global ranks - from all the chunks
        ./merge ${TEMP_DIR} ${TEMP_DIR} $CHUNKS $CHUNK_SIZE
        STATUS=$?

        if [[ $STATUS -eq $FAILURE ]]
        then
            exit 1
        fi

        DUR=$(echo "$($DATE) - $START" | bc)
        printf "Resolved global ranks for iteration %d in %.4f seconds\n" $H $DUR

        #at least something was resolved
        if [[ $STATUS -ne $EMPTY ]]
        then
          START=$($DATE)
            #update local ranks with resolved global ranks
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
#clean temp directory
rm -rf ${TEMP_DIR}/*

exit 0
