#include "merge.h"

//we are comparing 2 heap elements by current rank, then by next rank, if equal - by file_id
long compare_heap_elements (HeapElement *a, HeapElement *b) {
	if (a->current_rank == b->current_rank ) {
		if (a->next_rank == b->next_rank)
			return a->chunk_id - b->chunk_id;
		return ABSOLUTE(a->next_rank) - ABSOLUTE (b->next_rank);
	}

	return a->current_rank - b->current_rank;
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
		if(get_top_heap_element (manager, &smallest)!=SUCCESS)
			return FAILURE;
		if(manager->last_transferred.chunk_id == -1){
	    manager->updated_rank = smallest.current_rank;
	    manager->pair_count =0;
		}

		result = get_next_input_element (manager, smallest.chunk_id, &next);

		if (result==FAILURE)
			return FAILURE;

		if(result==SUCCESS) {        //next element exists
			if(insert_into_heap (manager,smallest.chunk_id, &next)!=SUCCESS)
				return FAILURE;
		}

		heap_to_output (manager, &smallest, &output_result);

		if (output_result.chunk_id >= 0) {          //app-specific
			chunk_id = output_result.chunk_id;
			manager->output_buffers[chunk_id][manager->output_buffer_positions[chunk_id]]=output_result.new_rank;
			manager->output_buffer_positions[chunk_id]++;

			//staying on the last slot of the output buffer - next will cause overflow
			if(manager->output_buffer_positions[chunk_id] == manager->output_buffer_capacity ) {
				flush_output_buffers(manager, chunk_id);
				manager->output_buffer_positions[chunk_id]=0;
			}
		}

		if (manager->current_heap_size == 0) {         //last heap element
			heap_to_output_last (manager, &smallest,  &output_result);

			chunk_id = output_result.chunk_id;
      manager->output_buffers[chunk_id][manager->output_buffer_positions[chunk_id]]=output_result.new_rank;
			manager->output_buffer_positions[chunk_id]++;
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

int get_top_heap_element (Manager * manager, HeapElement * result){
	HeapElement item;
	int child, parent;

	if(manager->current_heap_size == 0){
		printf( "UNEXPECTED ERROR: popping top element from an empty heap\n");
		return FAILURE;
	}

	*result=manager->heap[0];  //to be returned

	//now we need to reorganize heap - keep the smallest on top
	item = manager->heap [--manager->current_heap_size]; // to be reinserted

	parent =0;
	while ((child = (2 * parent) + 1) < manager->current_heap_size) {
		// if there are two children, compare them
		if (child + 1 < manager->current_heap_size &&
				(compare_heap_elements(&(manager->heap[child]),&(manager->heap[child + 1]))>0))
			++child;

		// compare item with the larger
		if (compare_heap_elements(&item, &(manager->heap[child]))>0) {
			manager->heap[parent] = manager->heap[child];
			parent = child;
		}
		else
			break;
	}
	manager->heap[parent] = item;

	return SUCCESS;
}

int insert_into_heap (Manager * manager, int chunk_id, RunRecord *input){

	HeapElement new_heap_element;
	int child, parent;

	new_heap_element.chunk_id = chunk_id;
	new_heap_element.current_rank = input->currentRank;
	new_heap_element.next_rank = input->nextRank;
	new_heap_element.count = input->count;

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
	result->chunk_id = current->chunk_id;
}


int refill_buffer (Manager * manager, int chunk_id) {

	int result;

	if(manager->input_file_positions[chunk_id] == -1) {
		manager->input_buffer_positions[chunk_id] = -1; //signifies no more elements
		return EMPTY; //run is complete - no more elements in the input file
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
	char file_name [MAX_PATH_LENGTH];
	FILE * outputFP;

	snprintf(file_name, sizeof(file_name), "%s/global_%d", manager->output_dir,chunk_id);

	OpenBinaryFileAppend (&(outputFP), file_name);
	Fwrite (manager->output_buffers[chunk_id], sizeof (long), manager->output_buffer_positions[chunk_id], outputFP);

	fclose (outputFP);
	outputFP = NULL;
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
	for (i=0; i<manager->total_chunks;i++)
		free(manager->output_buffers [i]);
	free(manager->output_buffers);
	free(manager->output_buffer_positions);
	free(manager->heap);
}

void setup(Manager * manager){
	int i;


	long mem_budget = MERGE_BUFFER_FACTOR * manager->working_chunk_size;

	manager->input_file_positions  = (int *) Calloc (manager->total_chunks * sizeof(int));
	//allocate input buffers
	manager->input_buffers = (RunRecord **) Calloc (manager->total_chunks * sizeof (RunRecord *));
	manager->input_buffer_capacity = mem_budget / (sizeof(RunRecord)*(manager->total_chunks));
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

	//allocate output buffers - multiple in this algorithm
	manager->output_buffer_capacity =  mem_budget / (sizeof(OutputElement)*(manager->total_chunks));
	if (manager->output_buffer_capacity < 1) manager->output_buffer_capacity = 1;
	manager->output_buffers = (long **) Calloc (manager->total_chunks * sizeof (long*));
	for (i=0; i<manager->total_chunks;i++)
		manager->output_buffers [i] = (long *) Calloc ((size_t)manager->output_buffer_capacity *sizeof(OutputElement));

	manager->output_buffer_positions  = (int *) Calloc (manager->total_chunks * sizeof(int));

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
