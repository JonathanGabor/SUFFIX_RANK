#ifndef REDUCE_H
#define REDUCE_H

#include "utils.h"
#include "algorithm.h"

// Derived memory budget for merge buffers. Scales with the runtime chunk size.
// Each chunk gets working_chunk_size * MERGE_BUFFER_FACTOR bytes of input
// buffer (and the same for output buffers), divided across all total_chunks.
#define MERGE_BUFFER_FACTOR 10L

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

typedef struct merge_manager {
	long pair_count;
	long updated_rank; //the new rank obtained by adding count to the prev value of the updated_rank
	HeapElement *heap;  //keeps 1 from each buffer in top-down order - smallest on top (according to compare function)
	HeapElement last_transferred;             //last element transferred from heap to output buffer

	int *input_file_positions;             //current position in each file, -1 if the run is complete
	FILE **input_fps;                      //one open file pointer per chunk for sequential run reads

	RunRecord **input_buffers; //array of buffers to hold part of each run
	int *input_buffer_positions; //position in current input buffer, if no need to refill  - -1
	int input_buffer_capacity; //how many elements max can each hold
	int *input_buffer_lengths;  //number of actual elements currently in input buffer - can be less than max capacity

	GlobalRecord** output_buffers;     //per-chunk buffers of (resolved rank, local count) records, flushed to global_<chunk>
	int *output_buffer_positions;              //where to add next element in each output buffer
	int output_buffer_capacity;             //how many elements max each output buffer can hold
	FILE **output_fps;                 //one persistent output file pointer per chunk; writes are sequential so we never reopen on each flush

	int current_heap_size;
	int total_chunks;
	long working_chunk_size;                       //runtime chunk size; derived buffer sizes scale with this
	char input_dir [MAX_PATH_LENGTH];              //to generate input file based on all file_id - interval_id combination listed in inputFileNumbers
	char output_dir [MAX_PATH_LENGTH];             //where to write merged updates for each fileid-intervalid
}Manager;

int reduce(char* input_dir, char* temp_dir, int total_chunks, long working_chunk_size);
void setup(Manager * manager);
void clean_up(Manager * manager);
void flush_output_buffers (Manager *manager, int chunk_id);
int refill_buffer (Manager * manager, int chunk_id);
void heap_to_output_last ( Manager *manager, HeapElement *current, OutputElement *result);
void heap_to_output ( Manager *manager, HeapElement *current, OutputElement *result);
int get_next_input_element (Manager * manager, int chunk_id, RunRecord *result);
int insert_into_heap (Manager * manager, int chunk_id, RunRecord *input);
void replace_top_heap_element (Manager * manager, int chunk_id, RunRecord *input);
void pop_top_heap_element (Manager * manager);
int init_merge (Manager * manager);
int merge_runs (Manager * manager);
long compare_heap_elements (HeapElement *a, HeapElement *b);

#endif
