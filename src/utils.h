#ifndef UTILS_H
#define UTILS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
//we need each chunk of the pre-SA file to be sorted by final rank in memory
//so we can have no more than say 1GB/size of (SARecord): 1073741824/16 = 67108864, so we divide this by 8, which means we will have
//in each chunk 134217728 about 134 MB of data to sort
#define NUM_THREADS 4
#ifndef WORKING_CHUNK_SIZE
  #define WORKING_CHUNK_SIZE 16777216 //22369620 //replace chunk size everywhere
#endif
//#define WORKING_CHUNK_SIZE (16777216/NUM_THREADS) //22369620 //replace chunk size everywhere
//#define DEFAULT_SA_CHUNK_SIZE 22369620

//#define DEFAULT_TRIPLE_BUFFER_SIZE 33554432
//(1024*1024*1024/32)*24 = 805306368 - that is 800 MB for each buffer at most
//about 33 MB of input fits into this buffer, and having 20 of these allows each chunk size go up to 660 MB - never the case really
//if only 1024 files are allowed per directory, as on some linux machines, we can have 1024/20 = about 50 input files if you write all temps into the same directory
//this will amount for the total input size of 50*660 MB = 33 GB and all this in memory < 2 GB
//for merge - at most 1024 input runs

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
void * Calloc (int num_bytes);
void tsort(int *sa, long *next_ranks, int n);

#endif
