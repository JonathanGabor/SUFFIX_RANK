#ifndef ALGORITHM_H
#define ALGORITHM_H

#include <stdint.h>

// Rank values fit in int32 (total symbol count < 2^31 for supported inputs).
// Storing ranks as int32 halves the on-disk runs_* footprint and the heap
// element size that the merge stage streams through.
typedef struct run_triple {
	int32_t currentRank;
	int32_t nextRank;
	int count;
} RunRecord;

typedef struct rank_pos_pair {
	long index;
	long value;
} InverseRecord;

int generate_local_runs (char * rank_dir, char * runs_dir, int total_chunks, int chunk_id, int h,
                         long working_chunk_size,
                         int32_t * current_ranks_buffer, int32_t * next_ranks_buffer,
                         int * sa_buffer, RunRecord * runs_buffer);

int resolve_global_ranks (char *temp_dir );
int update_local_ranks (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, int h,
                        long working_chunk_size,
                        int32_t * buffer_current, int32_t * buffer_next,
                        int * sa_buffer, long * updated_ranks);

#endif
