#ifndef ALGORITHM_H
#define ALGORITHM_H

#include <stdint.h>
#include "utils.h"   // int40 + i40_load/i40_store

// Locate where the "next rank" (the rank of position i+prefix_len) lives, given
// the current prefix length. The global position of local index i in chunk_id is
// chunk_id*S + i, so its next-rank position is chunk_id*S + i + prefix_len, which
// falls in chunk (base_chunk = chunk_id + prefix_len/S) at local offset
// (i + prefix_len%S), spilling into base_chunk+1 once that offset reaches S.
// When base_chunk is past the last chunk, every position's next-rank is beyond the
// text end, so the chunk is fully resolved this round (EMPTY).
//
// prefix_len need NOT be a power of two and S need NOT be a power of two; this
// replaces the old log2/bit-shift scheme. refine and update MUST share this so
// they agree on which chunks are EMPTY (the runs_<chunk> <-> global_<chunk>
// correspondence depends on it).
typedef struct {
	long off;         // prefix_len % working_chunk_size
	int  base_chunk;  // chunk_id + prefix_len / working_chunk_size
	int  is_empty;    // base_chunk > total_chunks - 1
} NextRankLoc;

static inline NextRankLoc next_rank_loc(long prefix_len, long working_chunk_size,
                                        int chunk_id, int total_chunks) {
	NextRankLoc loc;
	long dist = prefix_len / working_chunk_size;
	loc.off = prefix_len % working_chunk_size;
	loc.base_chunk = chunk_id + (int) dist;
	loc.is_empty = loc.base_chunk > total_chunks - 1;
	return loc;
}

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

int generate_local_runs (char * rank_dir, char * runs_dir, int total_chunks, int chunk_id, long prefix_len,
                         long working_chunk_size,
                         int40 * current_ranks_buffer, int40 * next_ranks_buffer,
                         int * sa_buffer, RunRecord * runs_buffer);

int resolve_global_ranks (char *temp_dir );
int update_local_ranks (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, long prefix_len,
                        long working_chunk_size,
                        int40 * buffer_current,
                        int * sa_buffer, GlobalRecord * global_buf);

#endif
