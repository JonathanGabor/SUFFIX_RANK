#include "utils.h"
#include "algorithm.h"

/**
   This sorts regions of SA with the same current_rank by their next_rank-
   the value a distance 2^h away.  It outputs runs of (curr, next, count)
 **/

//create RunRecord triplet and sort
void sort_and_output_group(int * sa_buffer, long * next_ranks_buffer, long current_rank,
                           int start_interval, int end_interval,
                           RunRecord * out_buf, int * out_count){

	int i;
	tsort(&sa_buffer[start_interval], next_ranks_buffer, end_interval-start_interval);

	RunRecord output;
	output.currentRank = current_rank;
	output.count = 1;
	output.nextRank = next_ranks_buffer[sa_buffer[start_interval]];

  //find runs and append them to output buffer
	for (i = start_interval+1; i < end_interval; i++) {
		if (next_ranks_buffer[sa_buffer[i]] != output.nextRank) {
			out_buf[(*out_count)++] = output;
			output.count = 1;
			output.nextRank = next_ranks_buffer[sa_buffer[i]];
		}
		else {
			output.count++;
		}
	}
	out_buf[(*out_count)++] = output;
}

int generate_local_runs (char * rank_dir, char * runs_dir, int total_chunks,
                         int chunk_id, int h, long * current_ranks_buffer,
                         long * next_ranks_buffer, int * sa_buffer,
                         RunRecord * runs_buffer) {

  char runs_file_name [MAX_PATH_LENGTH];
  FILE *runsFP = NULL;
 	sprintf (runs_file_name, "%s/runs_%d", runs_dir, chunk_id);
 	OpenBinaryFileAppend(&runsFP, runs_file_name);

  //Determine which additional chunk must be loaded
  int size_order = 0;
  while((WORKING_CHUNK_SIZE >> size_order) > 1) {
    size_order++;
  }

  int next_chunk_dist = h > size_order ? 1<<(h-size_order) : 0;
  if ((next_chunk_dist + chunk_id) > total_chunks-1) {
        fclose(runsFP);
        return EMPTY;
  }

	int i, r, total_records = 0;

	FILE *currentFP = NULL;
	FILE *nextFP = NULL;
	FILE *saFP = NULL;

	char current_ranks_file_name [MAX_PATH_LENGTH];
	char next_ranks_file_name [MAX_PATH_LENGTH];
	char sa_file_name [MAX_PATH_LENGTH];

	sprintf (current_ranks_file_name, "%s/ranks_%d", rank_dir, chunk_id);
	sprintf (sa_file_name, "%s/sa_%d", rank_dir, chunk_id);

	//open current rank and sa file
	OpenBinaryFileRead (&currentFP, current_ranks_file_name);
	OpenBinaryFileReadWrite (&saFP, sa_file_name);

	//handle reading next_rank
	if (next_chunk_dist) {
		sprintf (next_ranks_file_name, "%s/ranks_%d", rank_dir, chunk_id+next_chunk_dist);
		OpenBinaryFileRead (&nextFP, next_ranks_file_name);
		fread (next_ranks_buffer, sizeof (long), WORKING_CHUNK_SIZE, nextFP);
		fclose(nextFP);
	}
  else {
		sprintf (next_ranks_file_name, "%s/ranks_%d", rank_dir, chunk_id);
		OpenBinaryFileRead (&nextFP, next_ranks_file_name);
		if (fseek(nextFP, (1 << h)*sizeof(long), SEEK_SET)) {
			printf ("Fseek failed trying to move to position %d in ranks file\n", (1 << h));
			exit (1);
		}
		r = fread (next_ranks_buffer, sizeof (long), WORKING_CHUNK_SIZE, nextFP);
		fclose(nextFP);
		if (chunk_id+1 < total_chunks) {
			sprintf (next_ranks_file_name, "%s/ranks_%d", rank_dir, chunk_id+1);
			OpenBinaryFileRead (&nextFP, next_ranks_file_name);
			fread (next_ranks_buffer + r, sizeof (long), (1<<h), nextFP);
            fclose (nextFP);
		}
	}

	//offset next rank by 2^h
	//read file by chunk, sort and generate triplet for each chunk
	fread (current_ranks_buffer, sizeof (long), WORKING_CHUNK_SIZE, currentFP);
  fclose (currentFP);

	total_records = fread (sa_buffer, sizeof (int), WORKING_CHUNK_SIZE, saFP);
  	if (total_records==0) {
		fclose(runsFP);
		fclose(saFP);
    		return EMPTY;
	}

	int start_interval = 0;
	long previous_rank = current_ranks_buffer[sa_buffer[0]];
	long current_rank;
	int out_count = 0;

  //Read through current_ranks_buffer until it changes.  Then sort based on next_rank.
	for (i=1; i < total_records; i++) {
		current_rank = current_ranks_buffer[sa_buffer[i]];
		if (current_rank != previous_rank) {
			//sort, generate runs
			sort_and_output_group(sa_buffer, next_ranks_buffer, previous_rank,
				                      start_interval, i, runs_buffer, &out_count);
			start_interval = i;
			previous_rank = current_rank;
		}
	}
	sort_and_output_group(sa_buffer, next_ranks_buffer, previous_rank,
		                      start_interval, total_records, runs_buffer, &out_count);

	Fwrite(runs_buffer, sizeof(RunRecord), out_count, runsFP);
	fclose(runsFP);
	runsFP = NULL;
	//return pointer to the beginning of the sa chunk
	fseek ( saFP, -(total_records )*sizeof(int), SEEK_CUR );
	Fwrite (sa_buffer, sizeof(int), total_records, saFP);
	fclose(saFP);

	return SUCCESS;
}

int main(int argc, char ** argv){
	char * rank_dir;
	char * runs_dir;
	int h, chunk_id, total_chunks;
	if (argc<5) {
		puts ("Run ./generate_local_runs <rank_dir> <runs_dir> <chunk_id> <order>");
		return FAILURE;
	}

  //Read inputs
	rank_dir = argv[1];
	runs_dir = argv[2];
	total_chunks = atoi(argv[3]);
	h = atoi(argv[4]);

  //allocate buffers
  long *current_ranks_buffer = (long *) Calloc ((WORKING_CHUNK_SIZE) *sizeof (long));
  long *next_ranks_buffer = (long *) Calloc ((WORKING_CHUNK_SIZE) *sizeof (long));
  int *sa_buffer = (int *) Calloc ((WORKING_CHUNK_SIZE) *sizeof (int));
  RunRecord *runs_buffer = (RunRecord *) Calloc ((WORKING_CHUNK_SIZE) *sizeof (RunRecord));

  int more_runs = EMPTY;
  for (chunk_id=0; chunk_id<total_chunks; chunk_id++) {
	   int result = generate_local_runs (rank_dir, runs_dir, total_chunks, chunk_id, h, current_ranks_buffer, next_ranks_buffer, sa_buffer, runs_buffer);
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
