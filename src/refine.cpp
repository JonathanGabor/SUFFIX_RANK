// Because init writes a true partial suffix array,
// sa_buffer is already in local lex order; positions sharing the same
// current_rank are sub-sorted by what follows. So refine can produce RunRecords
// with a single linear scan over sa_buffer, emitting whenever (curr, next)
// changes.

extern "C" {
#include "utils.h"
#include "algorithm.h"
}

#include <cstdio>
#include <cstdlib>

// Distribution of local run lengths (RunRecord.count) emitted this iteration,
// gated by TEST_PERFORMANCE. refine is a fresh process per iteration, so this
// zero-initialized histogram accumulates exactly one iteration's runs and is
// printed at process exit (see main). Buckets: 1, 2, 3, 4, 5..2^8, 2^8..2^16,
// 2^16..2^24, >2^24 (inclusive upper bound; each range starts one past the
// previous bound).
#define NUM_COUNT_BUCKETS 8
static long g_count_hist[NUM_COUNT_BUCKETS];
static long g_mid_chunk_flushes;

static inline int count_bucket(int count) {
	if (count <= 4) return count - 1;          // 1->0, 2->1, 3->2, 4->3
	if (count <= (1 << 8))  return 4;          // 5 .. 256
	if (count <= (1 << 16)) return 5;          // 257 .. 65536
	if (count <= (1 << 24)) return 6;          // 65537 .. 16777216
	return 7;                                  // > 2^24
}

static const char *const COUNT_BUCKET_LABELS[NUM_COUNT_BUCKETS] = {
	"1", "2", "3", "4", "5..2^8", "2^8..2^16", "2^16..2^24", ">2^24"
};

static inline void emit_run(int64_t curr, int64_t next, int count,
                            RunRecord *out_buf, int *out_count,
                            int capacity, FILE *runsFP) {
	if (TEST_PERFORMANCE) g_count_hist[count_bucket(count)]++;
	// Reserve room for an escape+overflow pair so it never splits across a flush.
	if (*out_count + 2 > capacity) {
		Fwrite(out_buf, sizeof(RunRecord), (size_t) *out_count, runsFP);
		*out_count = 0;
		if (TEST_PERFORMANCE) g_mid_chunk_flushes++;
	}
	RunRecord *r = &out_buf[(*out_count)++];
	i40_store(&r->currentRank, curr);
	i40_store(&r->nextRank, next);
	if (count < COUNT_ESCAPE) {
		r->count = (uint8_t) count;
	} else {
		// Count doesn't fit a byte: write the sentinel, then an overflow record
		// carrying the true count in currentRank (its own count byte is ignored).
		r->count = COUNT_ESCAPE;
		RunRecord *ov = &out_buf[(*out_count)++];
		i40_store(&ov->currentRank, (int64_t) count);
		i40_store(&ov->nextRank, 0);
		ov->count = 0;
	}
}

static int generate_local_runs_fast(char *rank_dir, char *runs_dir, int total_chunks,
                                    int chunk_id, long prefix_len, long working_chunk_size,
                                    int40 *current_ranks_buffer,
                                    int40 *next_ranks_buffer,
                                    int *sa_buffer,
                                    RunRecord *runs_buffer) {
	char runs_file_name[MAX_PATH_LENGTH];
	FILE *runsFP = NULL;
	snprintf(runs_file_name, sizeof runs_file_name, "%s/runs_%d", runs_dir, chunk_id);
	OpenBinaryFileAppend(&runsFP, runs_file_name);

	// Where the rank of position i+prefix_len lives (arbitrary prefix_len, see
	// next_rank_loc). update.c uses the identical predicate for its EMPTY check.
	NextRankLoc loc = next_rank_loc(prefix_len, working_chunk_size, chunk_id, total_chunks);
	if (loc.is_empty) {
		fclose(runsFP);
		return EMPTY;
	}

	FILE *currentFP = NULL, *nextFP = NULL, *saFP = NULL;
	char current_ranks_file_name[MAX_PATH_LENGTH];
	char next_ranks_file_name[MAX_PATH_LENGTH];
	char sa_file_name[MAX_PATH_LENGTH];

	snprintf(current_ranks_file_name, sizeof current_ranks_file_name,
	         "%s/ranks_%d", rank_dir, chunk_id);
	snprintf(sa_file_name, sizeof sa_file_name,
	         "%s/sa_%d", rank_dir, chunk_id);

	OpenBinaryFileRead(&currentFP, current_ranks_file_name);
	OpenBinaryFileRead(&saFP, sa_file_name);

	// Fill next_ranks_buffer[local] = rank of (chunk_id*S + local + prefix_len).
	// Aligned case: the window starts at the head of base_chunk. Otherwise it
	// starts off entries into base_chunk and spills off entries into base_chunk+1.
	if (loc.off == 0) {
		snprintf(next_ranks_file_name, sizeof next_ranks_file_name,
		         "%s/ranks_%d", rank_dir, loc.base_chunk);
		OpenBinaryFileRead(&nextFP, next_ranks_file_name);
		fread(next_ranks_buffer, sizeof(int40), (size_t) working_chunk_size, nextFP);
		fclose(nextFP);
	} else {
		snprintf(next_ranks_file_name, sizeof next_ranks_file_name,
		         "%s/ranks_%d", rank_dir, loc.base_chunk);
		OpenBinaryFileRead(&nextFP, next_ranks_file_name);
		long offset = loc.off * (long) sizeof(int40);
		if (fseek(nextFP, offset, SEEK_SET)) {
			printf("Fseek failed trying to move to position %ld in ranks file\n", loc.off);
			exit(1);
		}
		long r = (long) fread(next_ranks_buffer, sizeof(int40),
		                      (size_t) working_chunk_size, nextFP);
		fclose(nextFP);
		if (loc.base_chunk + 1 < total_chunks) {
			snprintf(next_ranks_file_name, sizeof next_ranks_file_name,
			         "%s/ranks_%d", rank_dir, loc.base_chunk + 1);
			OpenBinaryFileRead(&nextFP, next_ranks_file_name);
			fread(next_ranks_buffer + r, sizeof(int40), (size_t) loc.off, nextFP);
			fclose(nextFP);
		}
	}

	fread(current_ranks_buffer, sizeof(int40), (size_t) working_chunk_size, currentFP);
	fclose(currentFP);

	int total_records = (int) fread(sa_buffer, sizeof(int),
	                                (size_t) working_chunk_size, saFP);
	fclose(saFP);
	if (total_records == 0) {
		fclose(runsFP);
		return EMPTY;
	}

	// Single linear scan: emit a RunRecord every time (curr, next) changes.
	// sa_buffer is in true lex order, so consecutive positions sharing
	// (curr, next) are contiguous.
	int out_count = 0;
	int runs_capacity = (int) (working_chunk_size / 3);

	int64_t cur_c = i40_load(&current_ranks_buffer[sa_buffer[0]]);
	int64_t cur_n = i40_load(&next_ranks_buffer[sa_buffer[0]]);
	int cur_count = 1;

	for (int i = 1; i < total_records; i++) {
		int64_t c = i40_load(&current_ranks_buffer[sa_buffer[i]]);
		int64_t n = i40_load(&next_ranks_buffer[sa_buffer[i]]);
		if (c == cur_c && n == cur_n) {
			cur_count++;
		} else {
			emit_run(cur_c, cur_n, cur_count, runs_buffer, &out_count, runs_capacity, runsFP);
			cur_c = c;
			cur_n = n;
			cur_count = 1;
		}
	}
	emit_run(cur_c, cur_n, cur_count, runs_buffer, &out_count, runs_capacity, runsFP);

	if (out_count > 0)
		Fwrite(runs_buffer, sizeof(RunRecord), (size_t) out_count, runsFP);
	fclose(runsFP);

	return SUCCESS;
}

int main(int argc, char **argv) {
	if (argc < 6) {
		puts("Run ./refine <rank_dir> <runs_dir> <total_chunks> <prefix_len> <working_chunk_size>");
		return FAILURE;
	}
	char *rank_dir = argv[1];
	char *runs_dir = argv[2];
	int total_chunks = atoi(argv[3]);
	long prefix_len = atol(argv[4]);
	long working_chunk_size = parse_chunk_size(argv[5]);

	int40 *current_ranks_buffer = (int40 *) Calloc((size_t) working_chunk_size * sizeof(int40));
	int40 *next_ranks_buffer    = (int40 *) Calloc((size_t) working_chunk_size * sizeof(int40));
	int  *sa_buffer            = (int *)  Calloc((size_t) working_chunk_size * sizeof(int));
	RunRecord *runs_buffer     = (RunRecord *) Calloc((size_t) (working_chunk_size / 3) * sizeof(RunRecord));

	int more_runs = EMPTY;
	for (int chunk_id = 0; chunk_id < total_chunks; chunk_id++) {
		int result = generate_local_runs_fast(rank_dir, runs_dir, total_chunks, chunk_id, prefix_len,
		                                      working_chunk_size,
		                                      current_ranks_buffer, next_ranks_buffer,
		                                      sa_buffer, runs_buffer);
		if (result == FAILURE) return FAILURE;
		if (result != EMPTY) more_runs = SUCCESS;
	}

	if (TEST_PERFORMANCE) {
		long total = 0;
		for (int b = 0; b < NUM_COUNT_BUCKETS; b++) total += g_count_hist[b];
		fprintf(stderr, "refine: run-length count distribution for prefix_len %ld (total %ld runs)\n",
		        prefix_len, total);
		fprintf(stderr, "refine: runs_buffer flushed mid-chunk %ld times\n",
		        g_mid_chunk_flushes);
		for (int b = 0; b < NUM_COUNT_BUCKETS; b++) {
			double pct = total > 0 ? 100.0 * (double) g_count_hist[b] / (double) total : 0.0;
			fprintf(stderr, "  count %-10s : %12ld  (%6.2f%%)\n",
			        COUNT_BUCKET_LABELS[b], g_count_hist[b], pct);
		}
	}

	free(current_ranks_buffer);
	free(next_ranks_buffer);
	free(sa_buffer);
	free(runs_buffer);
	return more_runs;
}
