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
#define SWAP(p, q)      (tmp=sa[(p)], sa[(p)]=sa[(q)], sa[(q)]=tmp)
#define MED3(a, b, c)   (a<b) ?                        \
        ((b<c) ? (b) : ((a<c) ? (c) : (a)))       \
        : ((b>c) ? (b) : ((a>c) ? (c) : (a)))
#define KEY(a) ABSOLUTE(next_ranks[sa[(a)]])

// WORKING_CHUNK_SIZE is a runtime parameter passed via the CLI to each binary
// that needs it (see suffixrank.sh). chunk_size must be a positive power of 2.
// The public pipeline is byte-alphabet only.
#define DEFAULT_WORKING_CHUNK_SIZE 16777216L
#define BYTE_ALPHABET_SIZE 256L

#define SUCCESS 0
#define FAILURE 1
#define EMPTY 2

// 40-bit signed integer, stored as 5 little-endian bytes. Rank values are
// narrowed to this on disk and in the large streaming/rank buffers: 8 bytes
// (long) is wasteful, but 32 bits is too small for the 100s-of-GB inputs the
// algorithm must remain viable for. 40 bits covers +/-2^39 (~5.5e11) ranks.
// Hot in-memory comparisons unpack to int64 once, so the per-access cost stays
// off the heap's inner loop.
typedef struct { uint8_t b[5]; } int40;

static inline int64_t i40_load(const int40 *p) {
	uint32_t lo;
	memcpy(&lo, p->b, 4);                 // low 32 bits (unaligned-safe)
	int8_t hi = (int8_t) p->b[4];         // top 8 bits, sign-extended
	return (int64_t) lo | ((int64_t) hi << 32);
}

static inline void i40_store(int40 *p, int64_t v) {
	uint32_t lo = (uint32_t) v;
	memcpy(p->b, &lo, 4);
	p->b[4] = (uint8_t) ((uint64_t) v >> 32);
}

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
