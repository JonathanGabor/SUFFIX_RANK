#include "utils.h"
#include "algorithm.h"
#include <limits.h>

int update_local_ranks (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, long prefix_len,
                        long working_chunk_size,
                        int40 * buffer_current, int * sa_buffer, GlobalRecord * global_buf){
	// Same EMPTY predicate refine uses, so the two agree on which chunks have runs.
	NextRankLoc loc = next_rank_loc(prefix_len, working_chunk_size, chunk_id, total_chunks);
	if (loc.is_empty) return EMPTY;

	char file_name[MAX_PATH_LENGTH];

	FILE * global_resolved_FP = NULL;
	FILE * current_FP = NULL;
	FILE * saFP = NULL;

	int result, total_resolved, total_ranks, sa_length, m, q;

	// read current ranks for this chunk (mutated in place, then written back)
	snprintf(file_name, sizeof file_name, "%s/ranks_%d", rank_dir, chunk_id);
	OpenBinaryFileReadWrite (&current_FP, file_name);
	total_ranks = fread (buffer_current, sizeof(int40), working_chunk_size, current_FP);

	// read local suffix array (sorted by curr,next for this iteration)
	snprintf(file_name, sizeof file_name, "%s/sa_%d", rank_dir, chunk_id);
	OpenBinaryFileRead (&saFP, file_name);
	sa_length = fread (sa_buffer, sizeof(int), total_ranks, saFP);
	fclose(saFP);

	// read this chunk's global (rank, count) records, in run order. They are
	// 1:1 with refine's runs, so their counts partition the local SA exactly.
	snprintf(file_name, sizeof file_name, "%s/global_%d", temp_dir, chunk_id);
	OpenBinaryFileRead(&global_resolved_FP, file_name);
	fseek (global_resolved_FP, 0, SEEK_END);
	total_resolved = ftell (global_resolved_FP)/sizeof(GlobalRecord);
	rewind(global_resolved_FP);
	if (total_resolved == 0) {
		fclose (current_FP);
		fclose (global_resolved_FP);
		return EMPTY;
	}
	result = fread (global_buf, sizeof (GlobalRecord), total_resolved, global_resolved_FP);
	if (result != total_resolved) {
		printf ("Error reading global resolved ranks file %s: wanted to read %d but fread returned %d\n", file_name, total_resolved,result);
		return FAILURE;
	}
	fclose (global_resolved_FP);

	// Apply each group's resolved rank to the next <count> SA entries in order.
	// Suffixes resolved this round (rank <= 0) are dropped from the SA by
	// shifting later entries left over the gap (compaction via displacement).
	m = 0;
	int displacement = 0;
	for (q = 0; q < total_resolved; q++) {
		long rank = i40_load(&global_buf[q].rank);
		long cnt  = i32_load(&global_buf[q].count);
		long j;
		for (j = 0; j < cnt; j++) {
			int pos = sa_buffer[m];
			i40_store(&buffer_current[pos], rank);
			if (rank <= 0) {
				displacement++;
			} else if (displacement) {
				sa_buffer[m-displacement] = sa_buffer[m];
			}
			m++;
		}
	}
	if (m != sa_length) {
		printf ("update: counts (%d) do not cover local SA (%d) for chunk %d\n", m, sa_length, chunk_id);
		return FAILURE;
	}

	//write the updated ranks back, then the compacted suffix array
	rewind(current_FP);
	Fwrite (buffer_current, sizeof(int40), total_ranks, current_FP);

	snprintf(file_name, sizeof file_name, "%s/sa_%d", rank_dir, chunk_id);
	OpenBinaryFileWrite (&saFP, file_name);
	Fwrite (sa_buffer, sizeof(int), sa_length-displacement, saFP);

	fclose (current_FP);
	fclose (saFP);

	return SUCCESS;
}

int main (int argc, char **argv){
	char * rank_dir;
	char * temp_dir;
	int chunk_id, total_chunks;
	long prefix_len;

	if (argc<6) {
		puts ("Run ./update <local_ranks_dir> <temp_dir> <total_chunks> <prefix_len> <working_chunk_size>");
		return FAILURE;
	}

	rank_dir = argv[1];
	temp_dir = argv[2];
	total_chunks = atoi(argv[3]);
	prefix_len = atol(argv[4]);
	long working_chunk_size = parse_chunk_size(argv[5]);

	//allocate buffers: current ranks (mutated in place), local SA, and the
	//per-chunk global (rank,count) records read back from merge.
	int40 * buffer_current = (int40 *) Calloc ((size_t)working_chunk_size * sizeof(int40));
	int * sa_buffer = (int *) Calloc ((size_t)working_chunk_size * sizeof(int));
	GlobalRecord * global_buf = (GlobalRecord *) Calloc ((size_t)working_chunk_size * sizeof(GlobalRecord));

	int more_runs = EMPTY;
    for (chunk_id=0; chunk_id<total_chunks; chunk_id++) {
  	   int result = update_local_ranks (rank_dir, temp_dir, total_chunks, chunk_id, prefix_len,
  	                                    working_chunk_size,
  	                                    buffer_current, sa_buffer, global_buf);
       if (result == FAILURE){
         return FAILURE;
       }
       if (result != EMPTY){
         more_runs = SUCCESS;
       }
    }

	free (global_buf);
	free (buffer_current);
	free (sa_buffer);

	return more_runs;
}
