#include "utils.h"
#include "algorithm.h"
#include <limits.h>

int update_local_ranks (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, int h,
                        long working_chunk_size,
                        long * buffer_current, long * buffer_next,
                        int * sa_buffer, long * updated_ranks){
	int size_order = 0;
	while((working_chunk_size >> size_order) > 1) {size_order++;}
	long next_chunk_dist = h > size_order ? 1L<<(h-size_order) : 0;
	if ((next_chunk_dist + chunk_id) > total_chunks-1) return EMPTY;

	char file_name[MAX_PATH_LENGTH];

	FILE * global_resolved_FP = NULL;
	FILE * current_FP = NULL;
	FILE * next_FP = NULL;
	FILE * saFP = NULL;

	int result, total_resolved, total_ranks, sa_length, m, q, pos;

	// open specified input file with ranks updated
	// after previous iteration - with 2 file pointers
	snprintf(file_name, sizeof file_name, "%s/ranks_%d", rank_dir, chunk_id);
	OpenBinaryFileReadWrite (&current_FP, file_name);
	//memset(buffer_current, 0, (size_t)working_chunk_size * sizeof(long));
	total_ranks = fread (buffer_current, sizeof(long), working_chunk_size, current_FP);

	//handle reading next_rank
	if (next_chunk_dist) {
		snprintf(file_name, sizeof file_name, "%s/ranks_%d", rank_dir, (int)(chunk_id+next_chunk_dist));
		OpenBinaryFileRead (&next_FP, file_name);
		fread (buffer_next, sizeof (long), working_chunk_size, next_FP);
	} else{
		snprintf(file_name, sizeof file_name, "%s/ranks_%d", rank_dir, chunk_id);
		OpenBinaryFileRead (&next_FP, file_name);
		long offset = (1L << h) * (long)sizeof(long);
		if(fseek(next_FP, offset, SEEK_SET)) {
			printf ("Fseek failed trying to move to position %ld in ranks file\n", 1L << h);
			exit (1);
		}
		long r = (long) fread (buffer_next, sizeof (long), working_chunk_size, next_FP);
		// fclose(next_FP);
		if (chunk_id+1 < total_chunks) {
			fclose(next_FP);
			snprintf(file_name, sizeof file_name, "%s/ranks_%d", rank_dir, chunk_id+1);
			OpenBinaryFileRead (&next_FP, file_name);
			fread (buffer_next + r, sizeof (long), (size_t)(1L<<h), next_FP);
		}
	}

	// open local suffix array file where suffixes are sorted
	// according to local current,next for this iteration
	snprintf(file_name, sizeof file_name, "%s/sa_%d", rank_dir, chunk_id);
	OpenBinaryFileRead (&saFP, file_name);
	//read elements of sa
	sa_length = fread (sa_buffer, sizeof(int), total_ranks, saFP);
	fclose(saFP);
	//open file with global updates - if exists
	snprintf(file_name, sizeof file_name, "%s/global_%d", temp_dir, chunk_id);

	OpenBinaryFileRead(&global_resolved_FP, file_name);
	//read content of global resolved triples(curr, next, new) into buffer
	fseek (global_resolved_FP, 0, SEEK_END);
	total_resolved = ftell (global_resolved_FP)/sizeof(long);
	rewind(global_resolved_FP);
	if (total_resolved == 0) {
		fclose (current_FP);
		fclose (next_FP);
		fclose (global_resolved_FP);
		return EMPTY;
	}

	//read all global updates for this chunk
	// (sorted by curr,next rank) into an array updated_ranks
	result = fread (updated_ranks, sizeof (long), total_resolved, global_resolved_FP);
	if (result != total_resolved) {
		printf ("Error reading global resolved ranks file %s: wanted to read %d but fread returned %d\n", file_name, total_resolved,result);
		return FAILURE;
	}
	fclose (global_resolved_FP);

	//read specified number of total records in this interval
	//read as many elements of next array as are available

	m = 0; 	//position in local sa
	q = 0;
	long curr, next;
	int displacement = 0;
	while (q < total_resolved) {
		pos = sa_buffer [m];
		curr = buffer_current[pos];
		next = buffer_next[pos];
		while (m < sa_length && curr == buffer_current[pos] && next == buffer_next[pos])
		{
			buffer_current[pos] = updated_ranks[q];
			if (updated_ranks[q] <= 0) {
				displacement++;
			}
			else if (displacement) {
				sa_buffer[m-displacement] = sa_buffer[m];
			}
			m++;
			if (m < sa_length) {
				pos = sa_buffer[m];
			}
		}
		q++;
	}

	//return pointer to the beginning of the chunk
	rewind(current_FP);
	Fwrite (buffer_current, sizeof(long), total_ranks, current_FP);

	snprintf(file_name, sizeof file_name, "%s/sa_%d", rank_dir, chunk_id);
	OpenBinaryFileWrite (&saFP, file_name);
	Fwrite (sa_buffer, sizeof(int), sa_length-displacement, saFP);

	fclose (current_FP);
	fclose (next_FP);
	fclose (saFP);

	return SUCCESS;
}

int main (int argc, char **argv){
	char * rank_dir;
	char * temp_dir;
	int chunk_id, total_chunks, h;

	if (argc<6) {
		puts ("Run ./update <local_ranks_dir> <temp_dir> <total_chunks> <h> <working_chunk_size>");
		return FAILURE;
	}

	rank_dir = argv[1];
	temp_dir = argv[2];
	total_chunks = atoi(argv[3]);
	h = atoi(argv[4]);
	long working_chunk_size = parse_chunk_size(argv[5]);

	//allocate input buffers for current and next ranks, and for local suffix array
	long * buffer_current = (long *) Calloc ((size_t)working_chunk_size * sizeof(long));
	long * buffer_next = (long *) Calloc ((size_t)working_chunk_size * sizeof(long));
	int * sa_buffer = (int *) Calloc ((size_t)working_chunk_size * sizeof(int));
	long * updated_ranks = (long *) Calloc ((size_t)working_chunk_size * sizeof(long));

	int more_runs = EMPTY;
    for (chunk_id=0; chunk_id<total_chunks; chunk_id++) {
  	   int result = update_local_ranks (rank_dir, temp_dir, total_chunks, chunk_id, h,
  	                                    working_chunk_size,
  	                                    buffer_current, buffer_next, sa_buffer, updated_ranks);
       if (result == FAILURE){
         return FAILURE;
       }
       if (result != EMPTY){
         more_runs = SUCCESS;
       }
    }

	free (updated_ranks);
	free (buffer_current);
	free (buffer_next);
	free (sa_buffer);

	return more_runs;
}
