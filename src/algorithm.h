#ifndef ALGORITHM_H
#define ALGORITHM_H

#include <stdint.h>
#include "utils.h"   // int40 + i40_load/i40_store

// Rank values are stored as int40 (5 bytes) on disk and in streaming buffers.
// This shrinks the runs_* footprint vs 8-byte long while staying valid for the
// large (100s-of-GB) inputs the algorithm must support, where int32 would
// overflow. count is a chunk-local run length (<= chunk_size <= 2^30).
typedef struct run_triple {
	int40 currentRank;
	int40 nextRank;
	int count;
} RunRecord;

typedef struct rank_pos_pair {
	long index;
	long value;
} InverseRecord;

int generate_local_runs (char * rank_dir, char * runs_dir, int total_chunks, int chunk_id, int h,
                         long working_chunk_size,
                         int40 * current_ranks_buffer, int40 * next_ranks_buffer,
                         int * sa_buffer, RunRecord * runs_buffer);

int resolve_global_ranks (char *temp_dir );
int update_local_ranks (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, int h,
                        long working_chunk_size,
                        int40 * buffer_current, int40 * buffer_next,
                        int * sa_buffer, int40 * updated_ranks);

#endif
