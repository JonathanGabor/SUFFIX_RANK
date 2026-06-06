#ifndef REDUCE_H
#define REDUCE_H

#include "utils.h"
#include "algorithm.h"

// Stream selector for the per-chunk RankRun readers.
#define STREAM_NEXTS    0
#define STREAM_CURRENTS 1

typedef struct heap_element {
	int64_t current_rank;   //ranks unpacked from int40 once on insert; int64 keeps the hot compare cheap
	int64_t next_rank;
	int count;
	int chunk_id;
} HeapElement;

typedef struct output_element {
	int64_t new_rank;   //transient resolved rank; stored to disk as int40 via the output buffer
	int count;          //local run length of the resolved group (carried to update)
	int chunk_id;
} OutputElement;

// Joint-walk cursor: the decoded "remaining" of the current currents-run and
// nexts-run for a chunk. Persists across buffer refills, so a run that straddles
// a refill is transparent. next_triplet emits min(cur_rem, next_rem) at a time.
typedef struct triplet_cursor {
	int64_t cur_rank;
	long    cur_rem;
	int64_t next_rank;
	long    next_rem;
} TripletCursor;

typedef struct merge_manager {
	long pair_count;
	long updated_rank; //the new rank obtained by adding count to the prev value of the updated_rank
	HeapElement *heap;  //keeps 1 from each buffer in top-down order - smallest on top (according to compare function)
	HeapElement last_transferred;             //last element transferred from heap to output buffer

	// NEXTS stream (nexts_<id>) -- ALSO the in-place output overlay target.
	// Resolved (updated_rank, count) RankRuns are written into the front over
	// already-consumed nexts slots (same 6B size); split overflow is drained to
	// global_<id> by the put_current flush-on-collision guard (see merge.c).
	FILE **nexts_fps;
	RankRun **nexts_buffers;
	int *nexts_pos;          //read cursor in nexts buffer; -1 => fully drained
	int *nexts_len;          //records currently in nexts buffer
	int *nexts_filepos;      //running read count; -1 once EOF reached (no more refills)
	int nexts_capacity;      //max RankRuns per nexts buffer (~2x currents capacity)

	int *out_pos;            //output write cursor (records) into the nexts buffer front
	FILE **out_fps;          //one persistent global_<id> output file pointer per chunk

	// CURRENTS stream (currents_<id>), read-only. Supplies current-rank runs that
	// re-split the nexts runs into (curr,next,count) triplets.
	FILE **cur_fps;
	RankRun **cur_buffers;
	int *cur_pos;            //read cursor; -1 => drained
	int *cur_len;
	int *cur_filepos;        //running read count; -1 once EOF reached
	int cur_capacity;

	TripletCursor *cursors;  //per-chunk joint-walk state

	int current_heap_size;
	int total_chunks;
	long mem_bytes;                                //merge RAM budget; buffer sizes derived directly from this
	char nexts_dir [MAX_PATH_LENGTH];              //where nexts_<id> live (and global_<id> are written)
	char out_dir [MAX_PATH_LENGTH];                //where global_<id> are written
	char currents_dir [MAX_PATH_LENGTH];           //where currents_<id> live
}Manager;

int reduce(char* nexts_dir, char* out_dir, char* currents_dir, int total_chunks, long mem_bytes);
void setup(Manager * manager);
void clean_up(Manager * manager);
void flush_output(Manager *manager, int chunk_id);
int refill_nexts(Manager * manager, int chunk_id);
int refill_currents(Manager * manager, int chunk_id);
void heap_to_output_last ( Manager *manager, HeapElement *current, OutputElement *result);
void heap_to_output ( Manager *manager, HeapElement *current, OutputElement *result);
int next_triplet (Manager * manager, int chunk_id, TripletCursor *t,
                  int64_t *curr, int64_t *next, long *count);
int get_next_run (Manager * manager, int chunk_id, HeapElement *out);
int insert_into_heap (Manager * manager, HeapElement *input);
void replace_top_heap_element (Manager * manager, HeapElement *input);
void pop_top_heap_element (Manager * manager);
int init_merge (Manager * manager);
int merge_runs (Manager * manager);
long compare_heap_elements (HeapElement *a, HeapElement *b);

#endif
