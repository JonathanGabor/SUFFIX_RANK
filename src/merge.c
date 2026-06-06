#include "merge.h"

// merge reconstructs the (curr,next,count) triplet stream the heap consumes from
// TWO per-chunk RankRun streams: currents_<id> (current-rank runs) and nexts_<id>
// (next-rank runs), which are two run-length encodings of the same local SA. The
// joint walk (next_triplet) emits min(cur_rem,next_rem) at each step, re-splitting
// a nexts run that straddles a current-rank boundary. So #triplets >= #nexts runs.
//
// Output: resolved (updated_rank, count) RankRuns (same 6B size as a nexts record)
// are overlaid onto the front of the nexts buffer, over slots already read. Splits
// mean output can outrun the nexts records consumed; the guard below flushes the
// accumulated output to global_<id> and resets the write cursor to 0 whenever a
// write would reach the unread-nexts frontier. The just-consumed front (slots
// [0, nexts_pos)) is always free, and output is produced in SA order, so the
// flushed prefix and the records written after it stay correctly ordered on disk.
// No separate output buffer and no memmove are needed; the nexts buffer is sized
// ~2x the currents buffer so the guard fires rarely.
static inline void put_current(Manager *manager, int chunk_id, int64_t rank_field, uint8_t count_byte) {
	int wpos = manager->out_pos[chunk_id];
	int rp   = manager->nexts_pos[chunk_id];
	// Free (already-consumed) record slots at the front: the whole buffer once the
	// nexts stream is drained (rp == -1), otherwise the rp records read so far.
	long avail = (rp == -1) ? (long)manager->nexts_capacity : (long)rp;
	if ((long)(wpos + 1) > avail) {
		flush_output(manager, chunk_id);
		wpos = 0;
		manager->out_pos[chunk_id] = 0;
	}
	RankRun *o = &manager->nexts_buffers[chunk_id][wpos];
	i40_store(&o->rank, rank_field);
	o->count = count_byte;
	manager->out_pos[chunk_id] = wpos + 1;
}

// Encode count as a byte, escaping large counts with an overflow record (true
// count in the overflow record's rank field). Each record goes through the
// guarded put_current, so the overlay invariant holds even for the pair.
static inline void write_output(Manager *manager, int chunk_id, int64_t rank, int count) {
	if (count < COUNT_ESCAPE) {
		put_current(manager, chunk_id, rank, (uint8_t) count);
	} else {
		put_current(manager, chunk_id, rank, COUNT_ESCAPE);
		put_current(manager, chunk_id, (int64_t) count, 0);
	}
}

//we are comparing 2 heap elements by current rank, then by next rank, if equal - by file_id
long compare_heap_elements (HeapElement *a, HeapElement *b) {
	if (a->current_rank == b->current_rank ) {
		if (a->next_rank == b->next_rank)
			return a->chunk_id - b->chunk_id;
		return a->next_rank - b->next_rank;
	}
	return a->current_rank - b->current_rank;
}

//manager fields should be already initialized in the caller
int merge_runs (Manager * manager){
	int result;      //stores SUCCESS/FAILURE returned at the end
	OutputElement output_result;
	int chunk_id;
	HeapElement smallest;
	HeapElement next;       //decoded next triplet from the popped chunk
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

		result = get_next_run (manager, smallest.chunk_id, &next);

		if (result==FAILURE)
			return FAILURE;

		//Fuse the pop+push into a single sift-down: if the popped chunk has a
		//next triplet, replace the root with it and sift down once; otherwise
		//pop the root (shrink heap) and sift the former-last element down.
		if(result==SUCCESS)        //next element exists
			replace_top_heap_element (manager, &next);
		else
			pop_top_heap_element (manager);

		heap_to_output (manager, &smallest, &output_result);

		if (output_result.chunk_id >= 0) {          //app-specific
			//overlay the resolved record onto the nexts buffer (see write_output)
			write_output(manager, output_result.chunk_id, output_result.new_rank, output_result.count);
		}

		if (manager->current_heap_size == 0) {         //last heap element
			heap_to_output_last (manager, &smallest,  &output_result);
			write_output(manager, output_result.chunk_id, output_result.new_rank, output_result.count);
		}
		manager->last_transferred = smallest;
	}

	//flush what remains in each chunk's output overlay
	for (chunk_id=0; chunk_id < manager->total_chunks; chunk_id++) {
		flush_output(manager, chunk_id);
	}

	if (DEBUG) printf("Merge complete.\n");
	clean_up(manager);
	return SUCCESS;
}

int init_merge (Manager * manager) {
	int i, ret;
	HeapElement first;

	for(i=0;i<manager->total_chunks;i++) {
		refill_nexts(manager,i);
		refill_currents(manager,i);
	}

	for (i=0;i<manager->total_chunks;i++) {
		//get the first triplet from each chunk (joint walk of its two streams)
		ret = get_next_run (manager,i, &first);
		if (ret==FAILURE) //corruption (e.g. currents drained while nexts remain)
			return FAILURE;

		//insert it into heap
		if (ret!=EMPTY){
			if(insert_into_heap (manager, &first)==FAILURE)
				return FAILURE;
		}
	}

	if (manager->current_heap_size == 0){
		printf("Initial heap fill failed - heap is empty - nothing to merge\n");
		return FAILURE;
	}

	return SUCCESS;
}

//Replace the root with an already-decoded run (from any chunk) and sift it down.
//Heap size is unchanged. Used on the hot path: after the minimum is consumed
//we feed in the next record from the same chunk, avoiding a separate pop+push.
void replace_top_heap_element (Manager * manager, HeapElement *input){
	HeapElement item = *input;
	int child, parent;

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

int insert_into_heap (Manager * manager, HeapElement *input){

	HeapElement new_heap_element = *input;
	int child, parent;

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

// Read one physical RankRun from a chunk's stream (NEXTS or CURRENTS), refilling
// the buffer from disk as needed. Returns SUCCESS / EMPTY (stream drained).
static int get_phys_rankrun (Manager * manager, int chunk_id, int which, RankRun *out){
	int pos = (which == STREAM_NEXTS) ? manager->nexts_pos[chunk_id]
	                                  : manager->cur_pos[chunk_id];
	if (pos == -1) return EMPTY;          //stream fully drained

	int len = (which == STREAM_NEXTS) ? manager->nexts_len[chunk_id]
	                                  : manager->cur_len[chunk_id];
	if (pos < len) {
		if (which == STREAM_NEXTS) {
			*out = manager->nexts_buffers[chunk_id][pos];
			manager->nexts_pos[chunk_id] = pos + 1;
		} else {
			*out = manager->cur_buffers[chunk_id][pos];
			manager->cur_pos[chunk_id] = pos + 1;
		}
		return SUCCESS;
	}

	int rr = (which == STREAM_NEXTS) ? refill_nexts(manager, chunk_id)
	                                 : refill_currents(manager, chunk_id);
	if (rr == SUCCESS)
		return get_phys_rankrun(manager, chunk_id, which, out);
	return rr;   //EMPTY
}

// Decode one logical (rank, count) run from a chunk's stream, resolving the
// byte-count escape: a record with count==COUNT_ESCAPE is followed by an
// overflow record whose rank field holds the true count. Records may straddle a
// buffer boundary; get_phys_rankrun refills as needed.
static int decode_rankrun (Manager * manager, int chunk_id, int which,
                           int64_t *rank, long *count){
	RankRun raw;
	int r = get_phys_rankrun(manager, chunk_id, which, &raw);
	if (r != SUCCESS) return r;   //EMPTY

	*rank = i40_load(&raw.rank);
	if (raw.count != COUNT_ESCAPE) {
		*count = raw.count;
	} else {
		RankRun ov;
		if (get_phys_rankrun(manager, chunk_id, which, &ov) != SUCCESS) {
			printf("merge: missing overflow record after count escape in chunk %d\n", chunk_id);
			return FAILURE;
		}
		*count = i40_load(&ov.rank);
	}
	return SUCCESS;
}

// Joint walk: yield one (curr, next, count) triplet, count = overlap of the
// current currents-run and nexts-run. Advances whichever run(s) the overlap
// exhausts. Decodes the NEXTS run first so an empty chunk (resolved this round:
// no nexts records) returns EMPTY without ever touching its currents stream.
int next_triplet (Manager * manager, int chunk_id, TripletCursor *t,
                  int64_t *curr, int64_t *next, long *count){
	if (t->next_rem == 0) {
		long cnt; int64_t rk;
		int r = decode_rankrun(manager, chunk_id, STREAM_NEXTS, &rk, &cnt);
		if (r != SUCCESS) return r;       //EMPTY (chunk done) or FAILURE
		t->next_rank = rk;
		t->next_rem  = cnt;
	}
	if (t->cur_rem == 0) {
		long cnt; int64_t rk;
		int r = decode_rankrun(manager, chunk_id, STREAM_CURRENTS, &rk, &cnt);
		if (r == EMPTY) {
			//currents drained while nexts still has records: streams disagree
			printf("merge: currents stream shorter than nexts in chunk %d\n", chunk_id);
			return FAILURE;
		}
		if (r != SUCCESS) return FAILURE;
		t->cur_rank = rk;
		t->cur_rem  = cnt;
	}

	long take = (t->cur_rem < t->next_rem) ? t->cur_rem : t->next_rem;
	*curr  = t->cur_rank;
	*next  = t->next_rank;
	*count = take;
	t->cur_rem  -= take;
	t->next_rem -= take;
	return SUCCESS;
}

// Produce the next (curr,next,count) triplet for a chunk as a HeapElement.
int get_next_run (Manager * manager, int chunk_id, HeapElement *out){
	int64_t c, n; long cnt;
	int r = next_triplet(manager, chunk_id, &manager->cursors[chunk_id], &c, &n, &cnt);
	if (r != SUCCESS) return r;   //EMPTY or FAILURE
	out->chunk_id = chunk_id;
	out->current_rank = c;
	out->next_rank = n;
	out->count = (int) cnt;       //count <= chunk_size <= 2^31, fits int
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
			//the rank itself is always the (non-negative) global rank; if the previous
			//combination of current-next had count=1 then previous is resolved, signaled
			//to update by writing count=0 (a resolved run always covers exactly 1 SA entry)
			result->new_rank = manager->updated_rank;
			if (manager->pair_count == 1) result->count = 0;
			//in any case base rank is increased by counts so far - adjusting future new rank for current
			manager->updated_rank += manager->pair_count;
			//reset counts so far to the current's count
			manager->pair_count = current->count;
		}
	}
	else { //current ranks of 2 tuples are different
		//if previous combination of current-next had count=1 then previous is resolved (count=0)
		result->new_rank = manager->updated_rank;
		if (manager->pair_count == 1) result->count = 0;
		manager->updated_rank = current->current_rank;

		//reset counts so far to the current's count
		manager->pair_count = current->count;
	}
}

void heap_to_output_last ( Manager *manager, HeapElement *current, OutputElement *result) {
	result->new_rank = manager->updated_rank;
	//if this is the only record - both first and last - it is a resolved
	//singleton, signaled to update with count=0 (rank stays the global rank)
	if (current->count == 1 && (manager->last_transferred.chunk_id == -1
			|| current->current_rank != manager->last_transferred.current_rank
			|| current->next_rank != manager->last_transferred.next_rank)) {
		result->count = 0;
	}
	else {
		result->count = current->count;
	}
	result->chunk_id = current->chunk_id;
}


// Refill a chunk's nexts buffer. Because the nexts buffer doubles as the output
// overlay, flush any accumulated output (all input here is already consumed)
// before overwriting it, then restart the output cursor. Sequential reads: the
// file pointer is left where the previous read ended, so no fseek is needed.
int refill_nexts (Manager * manager, int chunk_id) {
	int result;

	if(manager->nexts_filepos[chunk_id] == -1) {
		manager->nexts_pos[chunk_id] = -1; //no more elements ever
		return EMPTY;
	}

	if (manager->out_pos[chunk_id] > 0) {
		flush_output(manager, chunk_id);
		manager->out_pos[chunk_id] = 0;
	}

	if ((result = fread (manager->nexts_buffers[chunk_id],
			sizeof (RankRun), manager->nexts_capacity, manager->nexts_fps[chunk_id])) > 0) {
		manager->nexts_pos[chunk_id] = 0;
		manager->nexts_len [chunk_id] = result;
		manager->nexts_filepos [chunk_id] += result;

		if (result < manager->nexts_capacity) //read hit EOF
			manager->nexts_filepos [chunk_id] = -1;
		return SUCCESS;
	}
	manager->nexts_filepos [chunk_id] = -1;
	manager->nexts_pos[chunk_id] = -1;
	return EMPTY;
}

// Refill a chunk's currents buffer (read-only stream, no output coupling).
int refill_currents (Manager * manager, int chunk_id) {
	int result;

	if(manager->cur_filepos[chunk_id] == -1) {
		manager->cur_pos[chunk_id] = -1;
		return EMPTY;
	}

	if ((result = fread (manager->cur_buffers[chunk_id],
			sizeof (RankRun), manager->cur_capacity, manager->cur_fps[chunk_id])) > 0) {
		manager->cur_pos[chunk_id] = 0;
		manager->cur_len [chunk_id] = result;
		manager->cur_filepos [chunk_id] += result;

		if (result < manager->cur_capacity)
			manager->cur_filepos [chunk_id] = -1;
		return SUCCESS;
	}
	manager->cur_filepos [chunk_id] = -1;
	manager->cur_pos[chunk_id] = -1;
	return EMPTY;
}

void flush_output (Manager *manager, int chunk_id) {
	//output lives in the front of the nexts buffer; writes are sequential so no
	//reopen/fseek is needed between flushes.
	if (manager->out_pos[chunk_id] > 0)
		Fwrite (manager->nexts_buffers[chunk_id], sizeof (RankRun),
		        manager->out_pos[chunk_id], manager->out_fps[chunk_id]);
}

void clean_up(Manager * manager){
	int i;
	for (i=0; i<manager->total_chunks;i++) {
		free(manager->nexts_buffers[i]);
		free(manager->cur_buffers[i]);
	}
	free(manager->nexts_buffers);
	free(manager->cur_buffers);

	for (i=0; i<manager->total_chunks;i++) {
		if (manager->nexts_fps[i]) fclose(manager->nexts_fps[i]);
		if (manager->cur_fps[i])   fclose(manager->cur_fps[i]);
		if (manager->out_fps[i])   fclose(manager->out_fps[i]);
	}
	free(manager->nexts_fps);
	free(manager->cur_fps);
	free(manager->out_fps);

	free(manager->nexts_pos);
	free(manager->nexts_len);
	free(manager->nexts_filepos);
	free(manager->out_pos);
	free(manager->cur_pos);
	free(manager->cur_len);
	free(manager->cur_filepos);
	free(manager->cursors);
	free(manager->heap);
}

void setup(Manager * manager){
	int i;

	// Total merge RAM is the mem_bytes budget, split across chunks as (1 currents +
	// 2 nexts) RankRun units each: the currents buffer (read-only) and the nexts
	// buffer (read + in-place output overlay), the latter sized ~2x for split
	// headroom. Solve cur_capacity from that; nexts_capacity = 2x.
	manager->cur_capacity = (int)(manager->mem_bytes
	                              / ((long)sizeof(RankRun) * 3 * manager->total_chunks));
	if (manager->cur_capacity < 1) manager->cur_capacity = 1;
	manager->nexts_capacity = 2 * manager->cur_capacity;

	manager->nexts_buffers   = (RankRun **) Calloc (manager->total_chunks * sizeof (RankRun *));
	manager->cur_buffers     = (RankRun **) Calloc (manager->total_chunks * sizeof (RankRun *));
	manager->nexts_fps       = (FILE **) Calloc (manager->total_chunks * sizeof(FILE *));
	manager->cur_fps         = (FILE **) Calloc (manager->total_chunks * sizeof(FILE *));
	manager->out_fps         = (FILE **) Calloc (manager->total_chunks * sizeof(FILE *));
	manager->nexts_pos       = (int *) Calloc (manager->total_chunks * sizeof(int));
	manager->nexts_len       = (int *) Calloc (manager->total_chunks * sizeof(int));
	manager->nexts_filepos   = (int *) Calloc (manager->total_chunks * sizeof(int));
	manager->out_pos         = (int *) Calloc (manager->total_chunks * sizeof(int));
	manager->cur_pos         = (int *) Calloc (manager->total_chunks * sizeof(int));
	manager->cur_len         = (int *) Calloc (manager->total_chunks * sizeof(int));
	manager->cur_filepos     = (int *) Calloc (manager->total_chunks * sizeof(int));
	manager->cursors         = (TripletCursor *) Calloc (manager->total_chunks * sizeof(TripletCursor));

	for (i=0; i<manager->total_chunks;i++) {
		char file_name[MAX_PATH_LENGTH];

		manager->nexts_buffers[i] = (RankRun *) Calloc ((size_t)manager->nexts_capacity * sizeof(RankRun));
		manager->cur_buffers[i]   = (RankRun *) Calloc ((size_t)manager->cur_capacity * sizeof(RankRun));

		// Cursors start "buffer empty" (pos==len==0, filepos==0) so the first
		// refill in init_merge loads from disk; cursors zero-initialized by Calloc.
		snprintf(file_name, sizeof(file_name), "%s/nexts_%d", manager->nexts_dir, i);
		OpenBinaryFileRead(&(manager->nexts_fps[i]), file_name);

		snprintf(file_name, sizeof(file_name), "%s/currents_%d", manager->currents_dir, i);
		OpenBinaryFileRead(&(manager->cur_fps[i]), file_name);

		//global_* files start empty each merge invocation (tmp is cleared upstream)
		snprintf(file_name, sizeof(file_name), "%s/global_%d", manager->out_dir, i);
		OpenBinaryFileWrite(&(manager->out_fps[i]), file_name);
	}

	manager->heap = (HeapElement *) Calloc (manager->total_chunks * sizeof (HeapElement));
	manager->current_heap_size = 0;
}

int reduce(char* nexts_dir, char* out_dir, char* currents_dir, int total_chunks, long mem_bytes){
    Manager manager = {0};
    strcpy(manager.nexts_dir, nexts_dir);
    strcpy(manager.out_dir, out_dir);
    strcpy(manager.currents_dir, currents_dir);
    manager.total_chunks = total_chunks;
    manager.mem_bytes = mem_bytes;
    setup(&manager);
    return merge_runs(&manager);
}

int main(int argc, char ** argv){
	if (argc < 6){
		printf("run ./merge <nexts_dir> <out_dir> <currents_dir> <total_chunks> <mem_bytes>\n");
		return FAILURE;
	}
	char * nexts_dir = argv[1];
	char * out_dir = argv[2];
	char * currents_dir = argv[3];
	int total_chunks = atoi(argv[4]);
	long mem_bytes = parse_mem_bytes(argv[5]);

	return reduce(nexts_dir, out_dir, currents_dir, total_chunks, mem_bytes);
}
