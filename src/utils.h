#ifndef UTILS_H
#define UTILS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>

#define DEBUG 0
#define DEBUG_SMALL 0
#define TEST_CORRECTNESS 0
#define TEST_PERFORMANCE 0
#define DEBUG_ORDER 0
#define MAX_LINE 10000
#define MAX_CHAR 255
#define DEFAULT_CHAR 35
#define MAX_PATH_LENGTH 1024

#define ABSOLUTE(a) (((a) > (0)) ? (a) : ((0)-(a)))
#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))
#define SWAP(p, q)      (tmp=*(p), *(p)=*(q), *(q)=tmp)
#define MED3(a, b, c)   (a<b) ?                        \
        ((b<c) ? (b) : ((a<c) ? (c) : (a)))       \
        : ((b>c) ? (b) : ((a>c) ? (c) : (a)))
#define KEY(a) ABSOLUTE(next_ranks[sa[(a)]])

// WORKING_CHUNK_SIZE is now a runtime parameter passed via the CLI to each
// binary (see suffixrank.sh). Must be a positive power of 2.
#define DEFAULT_WORKING_CHUNK_SIZE 16777216L
#define ALPHABET_SIZE 65536  // input symbols are uint16_t

#define SUCCESS 0
#define FAILURE 1
#define EMPTY 2

typedef struct tuple_count {
	unsigned int key;
    long count;
}Tuple;

void OpenFileRead (FILE ** fp, char * file_name);
void OpenBinaryFileRead (FILE ** fp, char * file_name);
void OpenBinaryFileWrite (FILE ** fp, char * file_name);
void OpenBinaryFileAppend(FILE ** fp, char * file_name);
void OpenBinaryFileReadWrite (FILE ** fp, char * file_name);
void OpenFileWrite (FILE ** fp, char * file_name);
void Fwrite (const void *buffer, size_t elem_size, size_t num_elements, FILE *fp );
void * Calloc (size_t num_bytes);
void tsort(int *sa, long *next_ranks, int n);
long parse_chunk_size (const char *arg);

#endif
