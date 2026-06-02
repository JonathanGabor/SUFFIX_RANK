#include "merge.h"

//Output GlobalRecords (9B) are overlaid onto the shared per-chunk input buffer
//(RunRecords, 14B), written at the front over slots already copied into the heap.
//int40/int32p are alignment-1 so the unaligned overlay is well defined.
//
//The output cursor must never overrun into input not yet read into the heap, i.e.
//the write must satisfy (pos+1)*9 <= 14*input_position (whole buffer is free once
//the run is exhausted, input_position == -1). Steady state keeps the cursors in
//lockstep with the input read leading, but across a refill the reset output
//cursor can momentarily carry up to two pending resolutions while only one new
//input slot is free. We guard each write: if it would overrun, flush the
//accumulated output first and restart the cursor (the just-freed front always
//holds at least one record, so pos 0 is then safe). This self-corrects the
//boundary; in steady state the guard never fires.
static inline void write_output(Manager *manager, int chunk_id, int64_t rank, int count) {
	int pos = manager->output_buffer_positions[chunk_id];
	int p = manager->input_buffer_positions[chunk_id];
	long avail = (p == -1) ? (long)manager->input_buffer_capacity * (long)sizeof(RunRecord)
	                       : (long)p * (long)sizeof(RunRecord);
	if ((long)(pos + 1) * (long)sizeof(GlobalRecord) > avail) {
		flush_output_buffers(manager, chunk_id);
		pos = 0;
		manager->output_buffer_positions[chunk_id] = 0;
	}
	GlobalRecord *gr = (GlobalRecord *)((char *)manager->input_buffers[chunk_id]
			+ (size_t)pos * sizeof(GlobalRecord));
	i40_store(&gr->rank, rank);
	i32_store(&gr->count, count);
	manager->output_buffer_positions[chunk_id] = pos + 1;
}

//we are comparing 2 heap elements by current rank, then by next rank, if equal - by file_id
long compare_heap_elements (HeapElement *a, HeapElement *b) {
	if (a->current_rank == b->current_rank ) {
		if (a->next_rank == b->next_rank)
			return a->chunk_id - b->chunk_id;
		//cast to long: int32 ranks span up to +/-2^31, so the difference of two
		//absolute values can exceed INT_MAX and must be computed in long.
		return (long) ABSOLUTE(a->next_rank) - (long) ABSOLUTE (b->next_rank);
	}

	return (long) a->current_rank - (long) b->current_rank;
}

//manager fields should be already initialized in the caller
int merge_runs (Manager * manager){
	int result;      //stores SUCCESS/FAILURE returned at the end
	OutputElement output_result;
	int chunk_id;
	HeapElement smallest;
	RunRecord next;         //here next is of input_type
	//1. go in the loop through all input files and fill-in initial buffers
	if (init_merge (manager)!=SUCCESS)
		return FAILURE;


	output_result.chunk_id = -1;
	manager->last_transferred.chunk_id = -1;

	while (manager->current_heap_size > 0) {     //heap is not empty
		smallest = manager->heap[0];             //peek the current minimum
		if(manager->last_transferred.chunk_id == -1){
	    	manager->updated_rank = smallest.current_rank;
	    	manager->pair_count =0;
		}

		result = get_next_input_element (manager, smallest.chunk_id, &next);

		if (result==FAILURE)
			return FAILURE;

		//Fuse the pop+push into a single sift-down: if the popped chunk has a
		//next record, replace the root with it and sift down once; otherwise
		//pop the root (shrink heap) and sift the former-last element down.
		if(result==SUCCESS)        //next element exists
			replace_top_heap_element (manager, smallest.chunk_id, &next);
		else
			pop_top_heap_element (manager);

		heap_to_output (manager, &smallest, &output_result);

		if (output_result.chunk_id >= 0) {          //app-specific
			//overlay the resolved record onto the shared buffer (see write_output)
			write_output(manager, output_result.chunk_id, output_result.new_rank, output_result.count);
		}

		if (manager->current_heap_size == 0) {         //last heap element
			heap_to_output_last (manager, &smallest,  &output_result);
			write_output(manager, output_result.chunk_id, output_result.new_rank, output_result.count);
		}
		manager->last_transferred = smallest;
	}

	//flush what remains in output buffer
	for (chunk_id=0; chunk_id < manager->total_chunks; chunk_id++) {
		//if(manager->output_buffer_positions[chunk_id] > 0) {
			flush_output_buffers(manager, chunk_id);
		//}
	}

	if (DEBUG) printf("Merge complete.\n");
	clean_up(manager);
	return SUCCESS;
}

int init_merge (Manager * manager) {
	int i, ret;
	RunRecord first = {0};

	for(i=0;i<manager->total_chunks;i++) {
		if (refill_buffer(manager,i) == FAILURE){
			fprintf(stderr, "Failed to fill initial buffer %d\n",i);
			return FAILURE;
		}
	}

	for (i=0;i<manager->total_chunks;i++) {
		//get element from each buffer
		ret = get_next_input_element (manager,i, &first);
		if (ret==FAILURE) //at least 1 element should exist
			return FAILURE;

		//insert it into heap
		if (ret!=EMPTY){
			if(insert_into_heap (manager, i, &first)==FAILURE)
				return FAILURE;
		}
	}

	if (manager->current_heap_size == 0){
		printf("Initial heap fill failed - heap is empty - nothing to merge\n");
		return FAILURE;
	}

	return SUCCESS;
}

//Replace the root with a new record (from any chunk) and sift it down.
//Heap size is unchanged. Used on the hot path: after the minimum is consumed
//we feed in the next record from the same chunk, avoiding a separate pop+push.
void replace_top_heap_element (Manager * manager, int chunk_id, RunRecord *input){
	HeapElement item;
	int child, parent;

	item.chunk_id = chunk_id;
	item.current_rank = i40_load(&input->currentRank);
	item.next_rank = i40_load(&input->nextRank);
	item.count = i32_load(&input->count);

	parent = 0;
	while ((child = (2 * parent) + 1) < manager->current_heap_size) {
		if (child + 1 < manager->current_heap_size &&
				(compare_heap_elements(&(manager->heap[child]),&(manager->heap[child + 1]))>0))
			++child;

		if (compare_heap_elements(&item, &(manager->heap[child]))>0) {
			manager->heap[parent] = manager->heap[child];
			parent = child;
		}
		else
			break;
	}
	manager->heap[parent] = item;
}

//Pop the root (shrink the heap by one) and sift the former-last element down.
//Used when the chunk whose record was just consumed has no more records.
void pop_top_heap_element (Manager * manager){
	HeapElement item;
	int child, parent;

	item = manager->heap [--manager->current_heap_size];
	if (manager->current_heap_size == 0)
		return;

	parent = 0;
	while ((child = (2 * parent) + 1) < manager->current_heap_size) {
		if (child + 1 < manager->current_heap_size &&
				(compare_heap_elements(&(manager->heap[child]),&(manager->heap[child + 1]))>0))
			++child;

		if (compare_heap_elements(&item, &(manager->heap[child]))>0) {
			manager->heap[parent] = manager->heap[child];
			parent = child;
		}
		else
			break;
	}
	manager->heap[parent] = item;
}

int insert_into_heap (Manager * manager, int chunk_id, RunRecord *input){

	HeapElement new_heap_element;
	int child, parent;

	new_heap_element.chunk_id = chunk_id;
	new_heap_element.current_rank = i40_load(&input->currentRank);
	new_heap_element.next_rank = i40_load(&input->nextRank);
	new_heap_element.count = i32_load(&input->count);

	if (manager->current_heap_size == manager->total_chunks) {
		printf( "Unexpected ERROR: heap is full\n");
		return FAILURE;
	}

	child = manager->current_heap_size++; /* the next available slot in the heap */

	while (child > 0) {
		parent = (child - 1) / 2;
		if (compare_heap_elements(&(manager->heap[parent]),&new_heap_element)>0) {
			manager->heap[child] = manager->heap[parent];
			child = parent;
		}
		else
			break;
	}
	manager->heap[child]= new_heap_element;
	return SUCCESS;
}

int get_next_input_element (Manager * manager, int chunk_id, RunRecord *result){

	if(manager->input_buffer_positions[chunk_id] == -1) //run is complete
		return EMPTY;

	//there are still elements in the buffer
	if(manager->input_buffer_positions[chunk_id] < manager->input_buffer_lengths[chunk_id])
		*result = manager->input_buffers[chunk_id][manager->input_buffer_positions[chunk_id]++];
	else {
		int refill_result = refill_buffer (manager, chunk_id);
		if(refill_result==SUCCESS)
			return get_next_input_element (manager,  chunk_id, result);
		else
			return refill_result;
	}
	return SUCCESS;
}

//This takes 2 consecutive triples sorted by curr,next
//and decides on a new rank by comparing current with previous
//it is the previous tuple that is resolved, and is returned in the result
void heap_to_output ( Manager *manager, HeapElement *current, OutputElement *result) {
	result->chunk_id = -1;

	//this is the first triple during merge
	//we initialize global variable  counts_so_far to be total counts of this triple
	//and we return result with file_id = -1 - indicating that there is no need to add this result to output
	if (manager->last_transferred.chunk_id == -1) {
		manager->pair_count = current->count;
		return;
	}

	result->chunk_id = manager->last_transferred.chunk_id;
	result->count = manager->last_transferred.count;  //local run length of the group being resolved

	//both current rank and next rank are the same - these just came from 2 different files
	//in this case we first need to increment total count for this combination of current-next
	//and we output the previous into the output buffer
	if(current->current_rank == manager->last_transferred.current_rank){
		if (current->next_rank == manager->last_transferred.next_rank) {
			manager->pair_count += current->count;
			result->new_rank = manager->updated_rank;         //not resolved, because current is the same
		}
		else { //current rank is the same, but next ranks are different
			//if previous combination of current-next had count=1 then previous is resolved
			result->new_rank = manager->pair_count == 1 ? -manager->updated_rank : manager->updated_rank;
			//in any case base rank is increased by counts so far - adjusting future new rank for current
			manager->updated_rank += manager->pair_count;
			//reset counts so far to the current's count
			manager->pair_count = current->count;
		}
	}
	else { //current ranks of 2 tuples are different
		//if previous combination of current-next had count=1 then previous is resolved
		result->new_rank = manager->pair_count == 1 ? -manager->updated_rank : manager->updated_rank;
		manager->updated_rank = current->current_rank;

		//reset counts so far to the current's count
		manager->pair_count = current->count;
	}
}

void heap_to_output_last ( Manager *manager, HeapElement *current, OutputElement *result) {
	//if this is the only record - both first and last
	if (current->count == 1 && (manager->last_transferred.chunk_id == -1
			|| current->current_rank != manager->last_transferred.current_rank
			|| current->next_rank != manager->last_transferred.next_rank)) {
				result->new_rank = -1 * manager->updated_rank;
	}
	else {
		result->new_rank = manager->updated_rank;
	}
	result->count = current->count;
	result->chunk_id = current->chunk_id;
}


int refill_buffer (Manager * manager, int chunk_id) {

	int result;

	if(manager->input_file_positions[chunk_id] == -1) {
		manager->input_buffer_positions[chunk_id] = -1; //signifies no more elements
		return EMPTY; //run is complete - no more elements in the input file
	}

	//the input buffer is shared with output: before fread overwrites it, flush the
	//output records accumulated in the front (all input here is already consumed),
	//then restart the output cursor. Any outputs lagging past this window boundary
	//get written into the fresh buffer and flushed at the next refill/final flush.
	if (manager->output_buffer_positions[chunk_id] > 0) {
		flush_output_buffers(manager, chunk_id);
		manager->output_buffer_positions[chunk_id] = 0;
	}

	//the file pointer is left exactly where the previous read ended, so reads
	//are sequential and no fseek is required
	if ((result = fread (manager->input_buffers[chunk_id],
			sizeof (RunRecord), manager->input_buffer_capacity, manager->input_fps[chunk_id])) > 0) {
		manager->input_buffer_positions[chunk_id] = 0;
		manager->input_buffer_lengths [chunk_id] = result;
		manager->input_file_positions [chunk_id] += result;

		if (result < manager->input_buffer_capacity) //no more reads
			manager->input_file_positions [chunk_id] = -1;
		return SUCCESS;
	}
	//no more elements - we read exactly until the end of the file in the previous upload
	manager->input_file_positions [chunk_id] = -1;
	manager->input_buffer_positions[chunk_id] = -1;

	return EMPTY;
}

void flush_output_buffers (Manager *manager, int chunk_id) {
	//output lives in the front of the shared input buffer; writes are sequential
	//so no reopen/fseek is needed between flushes.
	Fwrite (manager->input_buffers[chunk_id], sizeof (GlobalRecord), manager->output_buffer_positions[chunk_id], manager->output_fps[chunk_id]);
}

void clean_up(Manager * manager){
	int i;
	for (i=0; i<manager->total_chunks;i++)
		free(manager->input_buffers [i]);
	free(manager->input_buffers);

	for (i=0; i<manager->total_chunks;i++)
		if (manager->input_fps[i]) fclose(manager->input_fps[i]);
	free(manager->input_fps);

	free(manager->input_file_positions);
	free(manager->input_buffer_positions);
	free(manager->input_buffer_lengths);
	free(manager->output_buffer_positions);
	for (i=0; i<manager->total_chunks;i++)
		if (manager->output_fps[i]) fclose(manager->output_fps[i]);
	free(manager->output_fps);
	free(manager->heap);
}

void setup(Manager * manager){
	int i;


	long mem_budget = MERGE_BUFFER_FACTOR * manager->working_chunk_size;

	manager->input_file_positions  = (int *) Calloc (manager->total_chunks * sizeof(int));
	//allocate the per-chunk shared input/output buffers. Input and output share
	//one buffer, so the full 2*mem_budget goes to input capacity (2x the old
	//input-only size) while total merge RAM stays the same as the previous
	//separate input+output buffers.
	manager->input_buffers = (RunRecord **) Calloc (manager->total_chunks * sizeof (RunRecord *));
	manager->input_buffer_capacity = (2 * mem_budget) / (sizeof(RunRecord)*(manager->total_chunks));
	if (manager->input_buffer_capacity < 1) manager->input_buffer_capacity = 1;
	for (i=0; i<manager->total_chunks;i++)
		manager->input_buffers [i] = (RunRecord *) Calloc ((size_t)manager->input_buffer_capacity *sizeof(RunRecord));

	//open one persistent file pointer per chunk; reads are sequential so we never
	//need to reopen or fseek during refills
	manager->input_fps = (FILE **) Calloc (manager->total_chunks * sizeof(FILE *));
	for (i=0; i<manager->total_chunks;i++) {
		char file_name[MAX_PATH_LENGTH];
		snprintf(file_name, sizeof(file_name), "%s/runs_%d", manager->input_dir, i);
		OpenBinaryFileRead(&(manager->input_fps[i]), file_name);
	}

	//allocate position pointers
	manager->input_buffer_positions  = (int *) Calloc (manager->total_chunks * sizeof(int));
	manager->input_buffer_lengths  = (int *) Calloc (manager->total_chunks * sizeof(int));

	//output records are overlaid onto input_buffers; only the per-chunk write
	//cursor is needed (records flushed to global_<chunk> at each refill).
	manager->output_buffer_positions  = (int *) Calloc (manager->total_chunks * sizeof(int));

	//open one persistent file pointer per chunk for sequential output writes;
	//global_* files start empty each merge invocation (tmp is cleared upstream).
	manager->output_fps = (FILE **) Calloc (manager->total_chunks * sizeof(FILE *));
	for (i=0; i<manager->total_chunks;i++) {
		char file_name[MAX_PATH_LENGTH];
		snprintf(file_name, sizeof(file_name), "%s/global_%d", manager->output_dir, i);
		OpenBinaryFileWrite(&(manager->output_fps[i]), file_name);
	}

	//allocate heap
	manager->heap = (HeapElement *) Calloc (manager->total_chunks * sizeof (HeapElement));
	manager->current_heap_size = 0;
}

int reduce(char* input_dir, char* temp_dir, int total_chunks, long working_chunk_size){
    Manager manager = {0};
    strcpy(manager.input_dir, input_dir);
    strcpy(manager.output_dir, temp_dir);
    manager.total_chunks = total_chunks;
    manager.working_chunk_size = working_chunk_size;
    setup(&manager);
    return merge_runs(&manager);
}

int main(int argc, char ** argv){
	char * input_dir;
	char * output_dir;
	int total_chunks;

	if (argc < 5){
		printf("run ./merge <input_dir> <output_dir> <total_chunks> <working_chunk_size>\n");
		return FAILURE;
	}
	input_dir = argv[1];
	output_dir = argv[2];
	total_chunks = atoi(argv[3]);
	long working_chunk_size = parse_chunk_size(argv[4]);

	return reduce(input_dir, output_dir, total_chunks, working_chunk_size);
}
