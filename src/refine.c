#include "utils.h"
#include "algorithm.h"

/**
   This sorts regions of SA with the same current_rank by their next_rank-
   the value a distance 2^h away.  It outputs runs of (curr, next, count)
 **/

// runs_buffer is sized at working_chunk_size/3 — flush to runsFP whenever it
// fills up.
static inline void emit_run(RunRecord rec, RunRecord *out_buf, int *out_count,
                            int capacity, FILE *runsFP) {
	if (*out_count == capacity) {
		Fwrite(out_buf, sizeof(RunRecord), capacity, runsFP);
		*out_count = 0;
		if (TEST_PERFORMANCE) {
			fprintf(stderr, "refine: runs_buffer flushed mid-chunk (cap=%d)\n", capacity);
		}
	}
	out_buf[(*out_count)++] = rec;
}

//create RunRecord triplet and sort
void sort_and_output_group(int * sa_buffer, long * next_ranks_buffer, long current_rank,
                           int start_interval, int end_interval,
                           RunRecord * out_buf, int * out_count,
                           int capacity, FILE *runsFP){

	int i;
	tsort(&sa_buffer[start_interval], next_ranks_buffer, end_interval-start_interval);

	RunRecord output;
	output.currentRank = current_rank;
	output.count = 1;
	output.nextRank = next_ranks_buffer[sa_buffer[start_interval]];

  //find runs and append them to output buffer
	for (i = start_interval+1; i < end_interval; i++) {
		if (next_ranks_buffer[sa_buffer[i]] != output.nextRank) {
			emit_run(output, out_buf, out_count, capacity, runsFP);
			output.count = 1;
			output.nextRank = next_ranks_buffer[sa_buffer[i]];
		}
		else {
			output.count++;
		}
	}
	emit_run(output, out_buf, out_count, capacity, runsFP);
}

int generate_local_runs (char * rank_dir, char * runs_dir, int total_chunks,
                         int chunk_id, int h, long working_chunk_size,
                         long * current_ranks_buffer,
                         long * next_ranks_buffer, int * sa_buffer,
                         RunRecord * runs_buffer) {

  char runs_file_name [MAX_PATH_LENGTH];
  FILE *runsFP = NULL;
 	snprintf(runs_file_name, sizeof runs_file_name, "%s/runs_%d", runs_dir, chunk_id);
 	OpenBinaryFileAppend(&runsFP, runs_file_name);

  //Determine which additional chunk must be loaded
  int size_order = 0;
  while((working_chunk_size >> size_order) > 1) {
    size_order++;
  }

  long next_chunk_dist = h > size_order ? 1L<<(h-size_order) : 0;
  if ((next_chunk_dist + chunk_id) > total_chunks-1) {
        fclose(runsFP);
        return EMPTY;
  }

	int i, total_records = 0;
	long r;

	FILE *currentFP = NULL;
	FILE *nextFP = NULL;
	FILE *saFP = NULL;

	char current_ranks_file_name [MAX_PATH_LENGTH];
	char next_ranks_file_name [MAX_PATH_LENGTH];
	char sa_file_name [MAX_PATH_LENGTH];

	snprintf(current_ranks_file_name, sizeof current_ranks_file_name, "%s/ranks_%d", rank_dir, chunk_id);
	snprintf(sa_file_name, sizeof sa_file_name, "%s/sa_%d", rank_dir, chunk_id);

	//open current rank and sa file
	OpenBinaryFileRead (&currentFP, current_ranks_file_name);
	OpenBinaryFileReadWrite (&saFP, sa_file_name);

	//handle reading next_rank
	if (next_chunk_dist) {
		snprintf(next_ranks_file_name, sizeof next_ranks_file_name, "%s/ranks_%d", rank_dir, (int)(chunk_id+next_chunk_dist));
		OpenBinaryFileRead (&nextFP, next_ranks_file_name);
		fread (next_ranks_buffer, sizeof (long), working_chunk_size, nextFP);
		fclose(nextFP);
	}
  	else {
		snprintf(next_ranks_file_name, sizeof next_ranks_file_name, "%s/ranks_%d", rank_dir, chunk_id);
		OpenBinaryFileRead (&nextFP, next_ranks_file_name);
		long offset = (1L << h) * (long)sizeof(long);
		if (fseek(nextFP, offset, SEEK_SET)) {
			printf ("Fseek failed trying to move to position %ld in ranks file\n", 1L << h);
			exit (1);
		}
		r = (long) fread (next_ranks_buffer, sizeof (long), working_chunk_size, nextFP);
		fclose(nextFP);
		if (chunk_id+1 < total_chunks) {
			snprintf(next_ranks_file_name, sizeof next_ranks_file_name, "%s/ranks_%d", rank_dir, chunk_id+1);
			OpenBinaryFileRead (&nextFP, next_ranks_file_name);
			fread (next_ranks_buffer + r, sizeof (long), (size_t)(1L<<h), nextFP);
            fclose (nextFP);
		}
	}

	//offset next rank by 2^h
	//read file by chunk, sort and generate triplet for each chunk
	//memset(current_ranks_buffer, 0, (size_t)working_chunk_size * sizeof(long));
	fread (current_ranks_buffer, sizeof (long), working_chunk_size, currentFP);
  fclose (currentFP);

	total_records = fread (sa_buffer, sizeof (int), working_chunk_size, saFP);
  	if (total_records==0) {
		fclose(runsFP);
		fclose(saFP);
    		return EMPTY;
	}

	int start_interval = 0;
	long previous_rank = current_ranks_buffer[sa_buffer[0]];
	long current_rank;
	int out_count = 0;
	int runs_capacity = (int)(working_chunk_size / 3);

  //Read through current_ranks_buffer until it changes.  Then sort based on next_rank.
	for (i=1; i < total_records; i++) {
		current_rank = current_ranks_buffer[sa_buffer[i]];
		if (current_rank != previous_rank) {
			//sort, generate runs
			sort_and_output_group(sa_buffer, next_ranks_buffer, previous_rank,
				                      start_interval, i, runs_buffer, &out_count,
				                      runs_capacity, runsFP);
			start_interval = i;
			previous_rank = current_rank;
		}
	}
	sort_and_output_group(sa_buffer, next_ranks_buffer, previous_rank,
		                      start_interval, total_records, runs_buffer, &out_count,
		                      runs_capacity, runsFP);

	Fwrite(runs_buffer, sizeof(RunRecord), out_count, runsFP);
	fclose(runsFP);
	runsFP = NULL;
	//return pointer to the beginning of the sa chunk
	fseek ( saFP, -(long)total_records * (long)sizeof(int), SEEK_CUR );
	Fwrite (sa_buffer, sizeof(int), total_records, saFP);
	fclose(saFP);

	return SUCCESS;
}

int main(int argc, char ** argv){
	char * rank_dir;
	char * runs_dir;
	int h, chunk_id, total_chunks;
	if (argc<6) {
		puts ("Run ./refine <rank_dir> <runs_dir> <total_chunks> <h> <working_chunk_size>");
		return FAILURE;
	}

  //Read inputs
	rank_dir = argv[1];
	runs_dir = argv[2];
	total_chunks = atoi(argv[3]);
	h = atoi(argv[4]);
	long working_chunk_size = parse_chunk_size(argv[5]);

  //allocate buffers
  long *current_ranks_buffer = (long *) Calloc ((size_t)working_chunk_size * sizeof (long));
  long *next_ranks_buffer = (long *) Calloc ((size_t)working_chunk_size * sizeof (long));
  int *sa_buffer = (int *) Calloc ((size_t)working_chunk_size * sizeof (int));
  RunRecord *runs_buffer = (RunRecord *) Calloc ((size_t)(working_chunk_size / 3) * sizeof (RunRecord));

  int more_runs = EMPTY;
  for (chunk_id=0; chunk_id<total_chunks; chunk_id++) {
	   int result = generate_local_runs (rank_dir, runs_dir, total_chunks, chunk_id, h,
	                                     working_chunk_size,
	                                     current_ranks_buffer, next_ranks_buffer,
	                                     sa_buffer, runs_buffer);
     if (result == FAILURE){
       return FAILURE;
     }
     if (result != EMPTY){
       more_runs = SUCCESS;
     }
  }

  free (sa_buffer);
  free (current_ranks_buffer);
  free (next_ranks_buffer);
  free (runs_buffer);

  return more_runs;
}
