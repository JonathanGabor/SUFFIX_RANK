#ifndef ALGORITHM_H
#define ALGORITHM_H

#include <stdint.h>

typedef struct run_triple {
	long currentRank;
	long nextRank;
	int count;
} RunRecord;

typedef struct rank_pos_pair {
	long index;
	long value;
} InverseRecord;

typedef struct verify_record {
	long rank;          // ISA[p]; non-positive in this codebase's sign convention
	long position;      // global position p
	long next_rank;     // ranks_*[p+1], or LONG_MAX if p == N-1 (sorts shorter first)
	uint32_t char_val;  // T[p] in init.c's encoding: 0 = sentinel, packed_value+1 = real
	int sent_id;        // file index when char_val==0 (sentinel), else -1
} VerifyRecord;

int count_characters (char *input_directory, char *output_directory, long working_chunk_size, int word_length);
int generate_local_runs (char * rank_dir, char * runs_dir, int total_chunks, int chunk_id, int h,
                         long working_chunk_size,
                         long * current_ranks_buffer, long * next_ranks_buffer,
                         int * sa_buffer, RunRecord * runs_buffer);

int resolve_global_ranks (char *temp_dir );
int update_local_ranks (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, int h,
                        long working_chunk_size,
                        long * buffer_current, long * buffer_next,
                        int * sa_buffer, long * updated_ranks);

#endif
