#include "utils.h"
#include "algorithm.h"
#include <limits.h>

int update_local_ranks (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, long prefix_len,
                        long working_chunk_size,
                        int40 * buffer_current, int * sa_buffer, GlobalRecord * global_buf,
                        RankRun * currents_buf, int currents_capacity,
                        int * lo, int * hi){
	// Same EMPTY predicate refine uses, so the two agree on which chunks have runs.
	NextRankLoc loc = next_rank_loc(prefix_len, working_chunk_size, chunk_id, total_chunks);
	if (loc.is_empty) return EMPTY;

	char file_name[MAX_PATH_LENGTH];

	FILE * global_resolved_FP = NULL;
	FILE * current_FP = NULL;
	FILE * saFP = NULL;
	FILE * currents_FP = NULL;

	int result, total_resolved, sa_length, m, q;

	// This chunk's live window of active positions: only this slice of the int40
	// ranks file is loaded, mutated, and written back. Positions outside the
	// window keep their stored ranks (still valid next-ranks for other chunks).
	int win_lo = *lo;
	int win_hi = *hi;
	int new_lo = INT_MAX;   // recomputed window over the survivors
	int new_hi = -1;

	// read current ranks for this chunk's window (mutated in place, written back)
	snprintf(file_name, sizeof file_name, "%s/ranks_%d", rank_dir, chunk_id);
	OpenBinaryFileReadWrite (&current_FP, file_name);
	if (win_hi >= win_lo) {
		if (fseek(current_FP, (long) win_lo * (long) sizeof(int40), SEEK_SET)) {
			printf("update: fseek to position %d in ranks file failed\n", win_lo);
			return FAILURE;
		}
		fread (&buffer_current[win_lo], sizeof(int40), (size_t)(win_hi - win_lo + 1), current_FP);
	}

	// read local suffix array (sorted by curr,next for this iteration)
	snprintf(file_name, sizeof file_name, "%s/sa_%d", rank_dir, chunk_id);
	OpenBinaryFileRead (&saFP, file_name);
	sa_length = fread (sa_buffer, sizeof(int), working_chunk_size, saFP);
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

	// The surviving (count != 0) runs, in this same SA order, are exactly the
	// next iteration's currents stream: each is a distinct new-rank group of
	// size >= 2 over the compacted SA. Re-emit them as RankRuns to currents_<id>.
	snprintf(file_name, sizeof file_name, "%s/currents_%d", rank_dir, chunk_id);
	OpenBinaryFileWrite(&currents_FP, file_name);
	int cur_out = 0;

	// Apply each group's rank to the next <count> SA entries in order. A run
	// resolved this round is signaled by count==0 (it is always a global
	// singleton, so it covers exactly one SA entry); its rank is still stored
	// so it remains a valid next-rank for others, but it is dropped from the SA
	// by shifting later entries left over the gap (compaction via displacement).
	m = 0;
	int displacement = 0;
	for (q = 0; q < total_resolved; q++) {
		long rank = i40_load(&global_buf[q].rank);
		long cnt  = global_buf[q].count;
		if (cnt == COUNT_ESCAPE) {
			// Overflow: true count is carried in the next record's rank field.
			q++;
			cnt = i40_load(&global_buf[q].rank);
		}
		if (cnt == 0) {
			int pos = sa_buffer[m];
			i40_store(&buffer_current[pos], rank);
			displacement++;
			m++;
		} else {
			long j;
			for (j = 0; j < cnt; j++) {
				int pos = sa_buffer[m];
				i40_store(&buffer_current[pos], rank);
				if (pos < new_lo) new_lo = pos;   // survivor: tighten next window
				if (pos > new_hi) new_hi = pos;
				if (displacement) {
					sa_buffer[m-displacement] = sa_buffer[m];
				}
				m++;
			}
			// Survivor: carry it into the next iteration's currents stream.
			if (cur_out + 2 > currents_capacity) {
				Fwrite(currents_buf, sizeof(RankRun), (size_t) cur_out, currents_FP);
				cur_out = 0;
			}
			rankrun_emit(currents_buf, &cur_out, rank, cnt);
		}
	}
	if (m != sa_length) {
		printf ("update: counts (%d) do not cover local SA (%d) for chunk %d\n", m, sa_length, chunk_id);
		return FAILURE;
	}

	//write the updated ranks back (window slice only), then the compacted SA
	if (win_hi >= win_lo) {
		if (fseek(current_FP, (long) win_lo * (long) sizeof(int40), SEEK_SET)) {
			printf("update: fseek to position %d in ranks file failed\n", win_lo);
			return FAILURE;
		}
		Fwrite (&buffer_current[win_lo], sizeof(int40), (size_t)(win_hi - win_lo + 1), current_FP);
	}

	// Tighten the window to the survivors carried into the next iteration
	// (empty sentinel lo=0, hi=-1 when the chunk fully resolved this round).
	if (new_hi < new_lo) { new_lo = 0; new_hi = -1; }
	*lo = new_lo;
	*hi = new_hi;

	snprintf(file_name, sizeof file_name, "%s/sa_%d", rank_dir, chunk_id);
	OpenBinaryFileWrite (&saFP, file_name);
	Fwrite (sa_buffer, sizeof(int), sa_length-displacement, saFP);

	if (cur_out > 0)
		Fwrite(currents_buf, sizeof(RankRun), (size_t) cur_out, currents_FP);

	fclose (current_FP);
	fclose (saFP);
	fclose (currents_FP);

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
	// Headroom for byte-count overflow records: counts partition the local SA
	// (sum <= working_chunk_size) and each overflow run has count >= COUNT_ESCAPE,
	// so overflow records <= working_chunk_size/COUNT_ESCAPE. The whole-file fread
	// needs room for every physical record (runs + overflow records).
	size_t global_buf_capacity = (size_t)working_chunk_size
	                           + (size_t)working_chunk_size / COUNT_ESCAPE + 2;
	GlobalRecord * global_buf = (GlobalRecord *) Calloc (global_buf_capacity * sizeof(GlobalRecord));

	// Streaming output buffer for the next iteration's currents_<id> stream
	// (RankRuns). It is flushed whenever the next run + its escape pair would not
	// fit, so a fraction of a chunk is plenty.
	int currents_capacity = (int) (working_chunk_size / 3);
	if (currents_capacity < 2) currents_capacity = 2;
	RankRun * currents_buf = (RankRun *) Calloc ((size_t) currents_capacity * sizeof(RankRun));

	// Per-chunk active-position windows: read in, tightened to the survivors,
	// written back for the next iteration's refine/update to slice their loads.
	int * bounds_lo = (int *) Calloc ((size_t) total_chunks * sizeof(int));
	int * bounds_hi = (int *) Calloc ((size_t) total_chunks * sizeof(int));
	bounds_load(rank_dir, total_chunks, bounds_lo, bounds_hi);

	int more_runs = EMPTY;
    for (chunk_id=0; chunk_id<total_chunks; chunk_id++) {
  	   int result = update_local_ranks (rank_dir, temp_dir, total_chunks, chunk_id, prefix_len,
  	                                    working_chunk_size,
  	                                    buffer_current, sa_buffer, global_buf,
  	                                    currents_buf, currents_capacity,
  	                                    &bounds_lo[chunk_id], &bounds_hi[chunk_id]);
       if (result == FAILURE){
         return FAILURE;
       }
       if (result != EMPTY){
         more_runs = SUCCESS;
       }
    }

	// Persist the tightened windows for the next iteration.
	bounds_store(rank_dir, total_chunks, bounds_lo, bounds_hi);

	free (global_buf);
	free (currents_buf);
	free (buffer_current);
	free (sa_buffer);
	free (bounds_lo);
	free (bounds_hi);

	return more_runs;
}
