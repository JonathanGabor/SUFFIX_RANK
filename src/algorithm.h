#ifndef ALGORITHM_H
#define ALGORITHM_H

#include <stdint.h>
#include "utils.h"   // int40 + i40_load/i40_store

// Rank values are stored as int40 (5 bytes) on disk and in streaming buffers.
// This shrinks the runs_* footprint vs 8-byte long while staying valid for the
// large (100s-of-GB) inputs the algorithm must support, where int32 would
// overflow. count is a chunk-local run length (<= chunk_size <= 2^30), stored
// as int32p (4 bytes, alignment 1) so the struct packs to 14 bytes with no
// padding (a plain `int` would force alignment-4 padding to 16 bytes).
typedef struct run_triple {
	int40 currentRank;
	int40 nextRank;
	int32p count;
} RunRecord;

// One per chunk-local run, written by merge to global_<chunk> in run order
// (1:1 with refine's runs_<chunk>). Carries the resolved global rank plus the
// run's local count, so update can apply ranks by walking the local SA
// count-by-count without re-reading next-ranks. count is a chunk-local run
// length (<= chunk_size <= 2^30), stored as int32p (4 bytes) so the struct is
// 9 bytes rather than 10.
typedef struct global_record {
	int40 rank;
	int32p count;
} GlobalRecord;

// (position, final-rank) pair routed by rank bucket in create_pairs and
// consumed by invert. Both fields are < 2^39 (positions/ranks of a single
// string), so int40 halves the intermediate pairs_* footprint. The final
// suffixarray_* output stays `long`.
typedef struct rank_pos_pair {
	int40 index;
	int40 value;
} InverseRecord;

int generate_local_runs (char * rank_dir, char * runs_dir, int total_chunks, int chunk_id, int h,
                         long working_chunk_size,
                         int40 * current_ranks_buffer, int40 * next_ranks_buffer,
                         int * sa_buffer, RunRecord * runs_buffer);

int resolve_global_ranks (char *temp_dir );
int update_local_ranks (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, int h,
                        long working_chunk_size,
                        int40 * buffer_current,
                        int * sa_buffer, GlobalRecord * global_buf);

#endif
