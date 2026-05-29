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
