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
CHUNKS_DIR=chunks

SUCCESS=0
FAILURE=1
EMPTY=2

STATE=$SUCCESS
CHUNKS=0

TRUESTART=$($DATE)

RAM_DIVISOR=15
DEFAULT_MEM_BYTES=$(( RAM_DIVISOR * 16777216 ))

# Convert a human-readable size (e.g. 8G, 512M, 2048K, or plain bytes) to bytes.
to_bytes() {
    local s="$1" num unit
    if [[ "$s" =~ ^([0-9]+)([KkMmGgTt]?)([Bb]?)$ ]]; then
        num="${BASH_REMATCH[1]}"; unit="${BASH_REMATCH[2]}"
        case "$unit" in
            K|k) echo $(( num * 1024 )) ;;
            M|m) echo $(( num * 1024 * 1024 )) ;;
            G|g) echo $(( num * 1024 * 1024 * 1024 )) ;;
            T|t) echo $(( num * 1024 * 1024 * 1024 * 1024 )) ;;
            *)   echo "$num" ;;
        esac
    else
        return 1
    fi
}

# Pull out flags (may appear anywhere); remaining args stay positional.
RUN_VERIFY=0
CHUNK_SIZE_OVERRIDE=""
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --verify)  RUN_VERIFY=1; shift ;;
        # Bypass the OS page cache for all file I/O, to simulate a memory-saturated
        # large-input run (where scratch/input never gets cached) on an idle dev box.
        --nocache) export SUFFIXRANK_NOCACHE=1; shift ;;
        # Pin chunk_size (elements) directly, bypassing the memory-limit derivation.
        --chunk-size) CHUNK_SIZE_OVERRIDE="$2"; shift 2 ;;
        --chunk-size=*) CHUNK_SIZE_OVERRIDE="${1#*=}"; shift ;;
        *)         POSITIONAL+=("$1"); shift ;;
    esac
done
set -- "${POSITIONAL[@]}"

if [[ -z "$1" ]] || [[ ! -f "$1" ]]
then
    echo "Usage: $0 INPUT_FILE [MEMORY_LIMIT] [--chunk-size N] [--verify] [--nocache]"
    echo "  INPUT_FILE:     the single file to index (the string to build the suffix array for)"
    echo "  MEMORY_LIMIT:   working-RAM budget, e.g. 8G, 512M, or plain bytes (default ${DEFAULT_MEM_BYTES})"
    echo "                  chunk_size (elements) is derived as MEMORY_LIMIT / ${RAM_DIVISOR}"
    echo "  --chunk-size N: pin chunk_size (elements) directly, ignoring MEMORY_LIMIT"
    echo "  --verify:       run external-memory correctness checker after pipeline"
    echo "  --nocache:      bypass OS page cache for all I/O (simulate RAM-saturated run)"
    exit 1
fi

INPUT_FILE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"

cd "$(dirname "$0")"

if [[ -n "$CHUNK_SIZE_OVERRIDE" ]]; then
    CHUNK_SIZE="$CHUNK_SIZE_OVERRIDE"
    if ! [[ "$CHUNK_SIZE" =~ ^[0-9]+$ ]] || (( CHUNK_SIZE <= 0 )); then
        echo "Invalid --chunk-size '$CHUNK_SIZE': must be a positive integer"
        exit 1
    fi
    # Derive a merge RAM budget consistent with the pinned chunk size.
    MEM_BYTES=$(( CHUNK_SIZE * RAM_DIVISOR ))
    echo "Using chunk size: $CHUNK_SIZE elements (pinned via --chunk-size; byte alphabet, divsufsort path)"
else
    MEM_BYTES=$(to_bytes "${2:-$DEFAULT_MEM_BYTES}") || { echo "Invalid memory limit '$2'"; exit 1; }
    if (( MEM_BYTES <= 0 )); then echo "Invalid memory limit '$2'"; exit 1; fi
    CHUNK_SIZE=$(( MEM_BYTES / RAM_DIVISOR ))
    if (( CHUNK_SIZE <= 0 )); then
        echo "Memory limit ${MEM_BYTES} is too small (needs at least ${RAM_DIVISOR} bytes)"
        exit 1
    fi
    # Chunk size is kept within 32-bit int to avoid 64-bit-index complexity;
    # in practice it is never this large anyway.
    if (( CHUNK_SIZE > 2147483647 )); then
        CHUNK_SIZE=2147483647
        echo "Clamping chunk size to INT_MAX (${CHUNK_SIZE})"
    fi
    echo "Using memory limit: $MEM_BYTES bytes -> chunk size $CHUNK_SIZE elements (byte alphabet, divsufsort path)"
fi
if [[ -n "${SUFFIXRANK_NOCACHE:-}" && "${SUFFIXRANK_NOCACHE}" != "0" ]]; then
    echo "Page cache: BYPASSED (SUFFIXRANK_NOCACHE=${SUFFIXRANK_NOCACHE})"
fi

#prepare directory structure for processing
for dir in "$RANK_DIR" "$OUTPUT_DIR" "$TEMP_DIR" "$CHUNKS_DIR"; do
    if [[ -d "$dir" ]]; then
        rm -rf "${dir:?}"/*
    else
        mkdir "$dir"
    fi
done

#Part 1. Initial partial suffix sort: read the source file directly, write ranks_* and sa_* chunks.
START=$($DATE)
./init "$INPUT_FILE" "$RANK_DIR" "$CHUNKS_DIR" "$CHUNK_SIZE"
STATUS=$?

if [[ $STATUS -eq $FAILURE ]]
then
    exit 1
fi

CHUNKS=$(ls -1 "${RANK_DIR}"/sa_* 2>/dev/null | wc -l | tr -d ' ')

# merge keeps three FILEs open per chunk: nexts (read), currents (read), global (write).
OPEN_FILE_LIMIT=$(ulimit -n)
if [[ "$OPEN_FILE_LIMIT" != "unlimited" ]]; then
    REQUIRED_OPEN_FILES=$(( 3 * CHUNKS + 16 ))
    if (( OPEN_FILE_LIMIT < REQUIRED_OPEN_FILES )); then
        echo "Error: current open-file limit is too low for ${CHUNKS} chunks."
        echo "merge needs at least ${REQUIRED_OPEN_FILES} file descriptors, but ulimit -n is ${OPEN_FILE_LIMIT}."
        echo "Increase the limit, for example: ulimit -n ${REQUIRED_OPEN_FILES}"
        exit 1
    fi
fi

DUR=$(echo "$($DATE) - $START" | bc)
printf "Finished initializing in %.4f, total %d chunks\n" $DUR $CHUNKS


# Prefix length to start doubling from. init's k-mer bucket sort resolves the
# first L characters, so the loop starts at L = k (read from ranks/kmer_length)
# and doubles each iteration (k, 2k, 4k, ...). L need not be a power of two.
# Fall back to 1 (single-character init) if init wrote no kmer_length.
if [[ -r "${RANK_DIR}/kmer_length" ]]; then
    L=$(cat "${RANK_DIR}/kmer_length")
else
    L=1
fi
ITER=0

#Part 3. Perform O(log N) iterations of an algorithm
MORE_RUNS=1

while (( $MORE_RUNS == 1 ))
do
    MORE_RUNS=0;
    START=$($DATE)
    #clean temp directory for the next iteration
    rm -rf ${TEMP_DIR}/*

    ./refine ${RANK_DIR} ${TEMP_DIR} $CHUNKS $L $CHUNK_SIZE
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
    printf "Refined ranks for iteration %d (prefix_len %d) in %.4f seconds\n" $ITER $L $DUR

    if [[ $MORE_RUNS -eq 1 ]]
    then
        START=$($DATE)
        ./merge ${TEMP_DIR} ${TEMP_DIR} ${RANK_DIR} $CHUNKS $MEM_BYTES
        STATUS=$?

        if [[ $STATUS -eq $FAILURE ]]
        then
            exit 1
        fi

        DUR=$(echo "$($DATE) - $START" | bc)
        printf "Resolved global ranks for iteration %d (prefix_len %d) in %.4f seconds\n" $ITER $L $DUR

        if [[ $STATUS -ne $EMPTY ]]
        then
          START=$($DATE)
            ./update ${RANK_DIR} ${TEMP_DIR} $CHUNKS $L $CHUNK_SIZE
            STATUS=$?

            if [[ $STATUS -eq $FAILURE ]]
            then
                exit 1
            fi
          DUR=$(echo "$($DATE) - $START" | bc)
          printf "Updated ranks for iteration %d (prefix_len %d) in %.4f seconds\n" $ITER $L $DUR
        fi
    fi

    echo "Finished iteration $ITER (prefix_len $L)"


    echo
    (( L *= 2 ))
    (( ITER++ ))
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
    ./verify "$INPUT_FILE" "$RANK_DIR" "$OUTPUT_DIR" "$TEMP_DIR" $CHUNKS $CHUNK_SIZE
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
