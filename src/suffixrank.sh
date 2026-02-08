#!/bin/bash


if date +%s.%N &>/dev/null; then
    timestamp() { date +%s.%N; }
else
    timestamp() { perl -MTime::HiRes=time -e 'printf "%.9f\n", time'; }
fi
DATE=timestamp

#constants
TEMP_DIR=tmp
BINARY_INPUT_DIR=input
RANK_DIR=ranks
OUTPUT_DIR=output

SUCCESS=0
FAILURE=1
EMPTY=2

STATE=$SUCCESS
CHUNKS=0

TRUESTART=$($DATE)
#Part 0. Preprocessing - convering text files to binary with sentinels
#need directory input to store binary files with sentinels
if [[ -d $BINARY_INPUT_DIR ]]
then
    rm -rf ${BINARY_INPUT_DIR}/*
else
    mkdir ${BINARY_INPUT_DIR}
fi

#convert text files to binary input
TOTALFILES=0

if [[ -d $1 ]]
then
    for FILE in $1/*
    do
        if [[ -f "$FILE" ]]
        then
            (( TOTALFILES++ ))
            ./input_to_binary $FILE $BINARY_INPUT_DIR
            echo "processed file $FILE with exit code $?"
        fi
    done
else
    echo "No such directory $1"
    exit 1
fi

echo "Total $TOTALFILES files to process"

#prepare directory structure for processing
#need output dir
if [[ -d $RANK_DIR ]]
then
    rm -rf ${RANK_DIR}/*
else
    mkdir ${RANK_DIR}
fi

if [[ -d $OUTPUT_DIR ]]
then
    rm -rf ${OUTPUT_DIR}/*
else
    mkdir ${OUTPUT_DIR}
fi

START=$($DATE)
#Part 1. Count totals of characters in all input files
./init ${BINARY_INPUT_DIR} ${RANK_DIR}
STATUS=$?

if [[ $STATUS -eq $FAILURE ]]
then
    exit 1
fi

CHUNKS=$(($(ls -l ${RANK_DIR}/* | wc -l)/2))

DUR=$(echo "$($DATE) - $START" | bc)
printf "Finished inizializing in %.4f, total %d chunks\n" $DUR $CHUNKS


#set prefix length to 2^H
H=0

#need tmp dir
if [[ -d $TEMP_DIR ]]
then
    rm -rf ${TEMP_DIR}/*
else
    mkdir ${TEMP_DIR}
fi

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
    #valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes

    ./refine ${RANK_DIR} ${TEMP_DIR} $CHUNKS $H
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
        ./merge ${TEMP_DIR} ${TEMP_DIR} $CHUNKS
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
            #valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./update ${RANK_DIR} ${TEMP_DIR} $H
            ./update ${RANK_DIR} ${TEMP_DIR} $CHUNKS $H
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
./create_pairs ${RANK_DIR} ${TEMP_DIR} $CHUNKS
STATUS=$?

if [[ $STATUS -eq $FAILURE ]]
then
    exit 1
fi
DUR=$(echo "$($DATE) - $START" | bc)
printf "Created in %.4f seconds\n" $DUR

START=$($DATE)


./invert ${TEMP_DIR} ${OUTPUT_DIR} $CHUNKS
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
