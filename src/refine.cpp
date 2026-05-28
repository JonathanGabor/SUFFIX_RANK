// refine: paired with the divsufsort/SAscan-based init.
//
// Because init writes a true partial suffix array (not just a bucket sort),
// sa_buffer is already in local lex order; positions sharing the same
// current_rank are sub-sorted by what follows. So refine can produce RunRecords
// with a single linear scan over sa_buffer, emitting whenever (curr, next)
// changes. No grouping, no tsort, no write-back of sa_buffer.

extern "C" {
#include "utils.h"
#include "algorithm.h"
}

#include <cstdio>
#include <cstdlib>


static inline void emit_run(RunRecord rec, RunRecord *out_buf, int *out_count,
                            int capacity, FILE *runsFP) {
	if (*out_count == capacity) {
		Fwrite(out_buf, sizeof(RunRecord), (size_t) capacity, runsFP);
		*out_count = 0;
		if (TEST_PERFORMANCE) {
			fprintf(stderr, "refine: runs_buffer flushed mid-chunk (cap=%d)\n", capacity);
		}
	}
	out_buf[(*out_count)++] = rec;
}

static int generate_local_runs_fast(char *rank_dir, char *runs_dir, int total_chunks,
                                    int chunk_id, int h, long working_chunk_size,
                                    long *current_ranks_buffer,
                                    long *next_ranks_buffer,
                                    int *sa_buffer,
                                    RunRecord *runs_buffer) {
	char runs_file_name[MAX_PATH_LENGTH];
	FILE *runsFP = NULL;
	snprintf(runs_file_name, sizeof runs_file_name, "%s/runs_%d", runs_dir, chunk_id);
	OpenBinaryFileAppend(&runsFP, runs_file_name);

	int size_order = 0;
	while ((working_chunk_size >> size_order) > 1) size_order++;
	long next_chunk_dist = h > size_order ? 1L << (h - size_order) : 0;
	if ((next_chunk_dist + chunk_id) > total_chunks - 1) {
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

	// next_ranks loading mirrors update.c.
	if (next_chunk_dist) {
		snprintf(next_ranks_file_name, sizeof next_ranks_file_name,
		         "%s/ranks_%d", rank_dir, (int) (chunk_id + next_chunk_dist));
		OpenBinaryFileRead(&nextFP, next_ranks_file_name);
		fread(next_ranks_buffer, sizeof(long), (size_t) working_chunk_size, nextFP);
		fclose(nextFP);
	} else {
		snprintf(next_ranks_file_name, sizeof next_ranks_file_name,
		         "%s/ranks_%d", rank_dir, chunk_id);
		OpenBinaryFileRead(&nextFP, next_ranks_file_name);
		long offset = (1L << h) * (long) sizeof(long);
		if (fseek(nextFP, offset, SEEK_SET)) {
			printf("Fseek failed trying to move to position %ld in ranks file\n", 1L << h);
			exit(1);
		}
		long r = (long) fread(next_ranks_buffer, sizeof(long),
		                      (size_t) working_chunk_size, nextFP);
		fclose(nextFP);
		if (chunk_id + 1 < total_chunks) {
			snprintf(next_ranks_file_name, sizeof next_ranks_file_name,
			         "%s/ranks_%d", rank_dir, chunk_id + 1);
			OpenBinaryFileRead(&nextFP, next_ranks_file_name);
			fread(next_ranks_buffer + r, sizeof(long), (size_t) (1L << h), nextFP);
			fclose(nextFP);
		}
	}

	fread(current_ranks_buffer, sizeof(long), (size_t) working_chunk_size, currentFP);
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

	RunRecord cur;
	cur.currentRank = current_ranks_buffer[sa_buffer[0]];
	cur.nextRank    = next_ranks_buffer[sa_buffer[0]];
	cur.count       = 1;

	for (int i = 1; i < total_records; i++) {
		long c = current_ranks_buffer[sa_buffer[i]];
		long n = next_ranks_buffer[sa_buffer[i]];
		if (c == cur.currentRank && n == cur.nextRank) {
			cur.count++;
		} else {
			emit_run(cur, runs_buffer, &out_count, runs_capacity, runsFP);
			cur.currentRank = c;
			cur.nextRank    = n;
			cur.count       = 1;
		}
	}
	emit_run(cur, runs_buffer, &out_count, runs_capacity, runsFP);

	if (out_count > 0)
		Fwrite(runs_buffer, sizeof(RunRecord), (size_t) out_count, runsFP);
	fclose(runsFP);

	return SUCCESS;
}

int main(int argc, char **argv) {
	if (argc < 6) {
		puts("Run ./refine <rank_dir> <runs_dir> <total_chunks> <h> <working_chunk_size>");
		return FAILURE;
	}
	char *rank_dir = argv[1];
	char *runs_dir = argv[2];
	int total_chunks = atoi(argv[3]);
	int h = atoi(argv[4]);
	long working_chunk_size = parse_chunk_size(argv[5]);

	long *current_ranks_buffer = (long *) Calloc((size_t) working_chunk_size * sizeof(long));
	long *next_ranks_buffer    = (long *) Calloc((size_t) working_chunk_size * sizeof(long));
	int  *sa_buffer            = (int *)  Calloc((size_t) working_chunk_size * sizeof(int));
	RunRecord *runs_buffer     = (RunRecord *) Calloc((size_t) (working_chunk_size / 3) * sizeof(RunRecord));

	int more_runs = EMPTY;
	for (int chunk_id = 0; chunk_id < total_chunks; chunk_id++) {
		int result = generate_local_runs_fast(rank_dir, runs_dir, total_chunks, chunk_id, h,
		                                      working_chunk_size,
		                                      current_ranks_buffer, next_ranks_buffer,
		                                      sa_buffer, runs_buffer);
		if (result == FAILURE) return FAILURE;
		if (result != EMPTY) more_runs = SUCCESS;
	}

	free(current_ranks_buffer);
	free(next_ranks_buffer);
	free(sa_buffer);
	free(runs_buffer);
	return more_runs;
}
