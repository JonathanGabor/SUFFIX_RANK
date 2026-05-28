#include "utils.h"
#include "algorithm.h"
#include "input_stream.h"
#include <limits.h>

// External-memory SA correctness checker.
//
// Phase A: stream input symbols + ranks_* in position order. For each position
// p emit a VerifyRecord (rank, position, next_rank, char, sent_id), routed
// into the destination rank-chunk file tmp/verify_<(-rank)/W>. Mirrors
// create_pairs.c's partition-by-rank-bucket pattern.
//
// Phase B: per rank-chunk, counting-sort records into rank order using
// slot = (-rank) - k*W. This simultaneously verifies that ranks_* forms a
// permutation of [-N+1, 0]. Then cross-check sorted_recs[i].position equals
// suffixarray_<k>[i] (SA inverts ISA), and adjacent-compare suffixes via
// (char, next_rank) with sentinel tie-breaking on sent_id.

#define WRITE_BUF_RECORDS 1024
#define READ_BATCH 4096

typedef struct {
	VerifyRecord *buf;
	int len;
	FILE *fp;
	char path[MAX_PATH_LENGTH];
} PartitionWriter;

static void pw_init(PartitionWriter *w, const char *tmp_dir, int k) {
	snprintf(w->path, MAX_PATH_LENGTH, "%s/verify_%d", tmp_dir, k);
	w->buf = (VerifyRecord *) Calloc(WRITE_BUF_RECORDS * sizeof(VerifyRecord));
	w->len = 0;
	OpenBinaryFileWrite(&w->fp, w->path);
}

static inline void pw_emit(PartitionWriter *w, const VerifyRecord *rec) {
	w->buf[w->len++] = *rec;
	if (w->len == WRITE_BUF_RECORDS) {
		Fwrite(w->buf, sizeof(VerifyRecord), (size_t) w->len, w->fp);
		w->len = 0;
	}
}

static void pw_close(PartitionWriter *w) {
	if (w->len > 0) Fwrite(w->buf, sizeof(VerifyRecord), (size_t) w->len, w->fp);
	fclose(w->fp);
	free(w->buf);
}

static int verify_partition(const char *input_dir, const char *ranks_dir,
                            const char *tmp_dir, int total_chunks,
                            long W) {
	InputStream stream;
	if (input_stream_open(&stream, input_dir) != SUCCESS) {
		return FAILURE;
	}

	PartitionWriter *writers = (PartitionWriter *) Calloc((size_t) total_chunks * sizeof(PartitionWriter));
	int k;
	for (k = 0; k < total_chunks; k++) pw_init(&writers[k], tmp_dir, k);

	long *R = (long *) Calloc((size_t) W * sizeof(long));
	char ranks_path[MAX_PATH_LENGTH];
	char peek_path[MAX_PATH_LENGTH];

	int chunk_id;
	for (chunk_id = 0; chunk_id < total_chunks; chunk_id++) {
		snprintf(ranks_path, sizeof ranks_path, "%s/ranks_%d", ranks_dir, chunk_id);
		FILE *ranksFP;
		OpenBinaryFileRead(&ranksFP, ranks_path);
		long count = (long) fread(R, sizeof(long), (size_t) W, ranksFP);
		fclose(ranksFP);

		long next_first;
		int have_next_first = 0;
		if (chunk_id + 1 < total_chunks) {
			snprintf(peek_path, sizeof peek_path, "%s/ranks_%d", ranks_dir, chunk_id + 1);
			FILE *peekFP;
			OpenBinaryFileRead(&peekFP, peek_path);
			if (fread(&next_first, sizeof(long), 1, peekFP) != 1) {
				fprintf(stderr, "Failed to peek %s for cross-chunk next_rank\n", peek_path);
				fclose(peekFP);
				return FAILURE;
			}
			fclose(peekFP);
			have_next_first = 1;
		}

		long i;
		for (i = 0; i < count; i++) {
			uint32_t cv;
			int sid;
			int rc = input_stream_next(&stream, &cv, &sid);
			if (rc != SUCCESS) {
				fprintf(stderr,
				        "Input stream ran out at position %ld but ranks_%d expects more\n",
				        (long) chunk_id * W + i, chunk_id);
				return FAILURE;
			}

			long cur_rank = R[i];
			long next_rank;
			if (i + 1 < count) {
				next_rank = R[i + 1];
			} else if (have_next_first) {
				next_rank = next_first;
			} else {
				next_rank = LONG_MAX;  // p == N-1: shorter suffix sorts first
			}

			long target = (-cur_rank) / W;
			if (target < 0 || target >= total_chunks) {
				fprintf(stderr,
				        "Rank %ld at position %ld maps to bucket %ld (out of [0, %d))\n",
				        cur_rank, (long) chunk_id * W + i, target, total_chunks);
				return FAILURE;
			}

			VerifyRecord rec;
			rec.rank = cur_rank;
			rec.position = (long) chunk_id * W + i;
			rec.next_rank = next_rank;
			rec.char_val = cv;
			rec.sent_id = sid;
			pw_emit(&writers[target], &rec);
		}
	}

	uint32_t cv;
	int sid;
	if (input_stream_next(&stream, &cv, &sid) != EMPTY) {
		fprintf(stderr, "Input stream has extra symbols beyond ranks_*\n");
		return FAILURE;
	}
	input_stream_close(&stream);

	for (k = 0; k < total_chunks; k++) pw_close(&writers[k]);
	free(writers);
	free(R);
	return SUCCESS;
}

// Strict lex-less on the (char, next_rank) annotation, with sentinel handling
// matching test_sa.c. Equal pairs count as violations (SA entries must be
// strictly ordered).
static int lex_less_strict(const VerifyRecord *p, const VerifyRecord *c) {
	int p_sent = (p->char_val == 0);
	int c_sent = (c->char_val == 0);
	if (p_sent && c_sent) {
		return p->sent_id < c->sent_id;
	}
	if (p_sent) return 1;
	if (c_sent) return 0;
	if (p->char_val != c->char_val) return p->char_val < c->char_val;
	// Tie on T[p]: lex-smaller iff next_rank is lex-smaller. Rank 0 is the
	// lex-smallest in this codebase's sign convention, so lex_smaller(a,b)
	// numerically is a > b.
	return p->next_rank > c->next_rank;
}

static int verify_check(const char *sa_dir, const char *tmp_dir,
                        int total_chunks, long W) {
	VerifyRecord *sorted_recs = (VerifyRecord *) Calloc((size_t) W * sizeof(VerifyRecord));
	char *seen = (char *) Calloc((size_t) W);
	VerifyRecord *read_buf = (VerifyRecord *) Calloc(READ_BATCH * sizeof(VerifyRecord));
	long *sa_buf = (long *) Calloc((size_t) W * sizeof(long));

	VerifyRecord carry;
	memset(&carry, 0, sizeof carry);
	int have_carry = 0;
	long total_verified = 0;

	char tmp_path[MAX_PATH_LENGTH];
	char sa_path[MAX_PATH_LENGTH];

	int k;
	for (k = 0; k < total_chunks; k++) {
		snprintf(tmp_path, sizeof tmp_path, "%s/verify_%d", tmp_dir, k);
		snprintf(sa_path, sizeof sa_path, "%s/suffixarray_%d", sa_dir, k);

		FILE *saFP;
		OpenBinaryFileRead(&saFP, sa_path);
		long sa_count = (long) fread(sa_buf, sizeof(long), (size_t) W, saFP);
		fclose(saFP);

		memset(seen, 0, (size_t) sa_count);

		FILE *tmpFP;
		OpenBinaryFileRead(&tmpFP, tmp_path);
		long placed = 0;
		size_t got;
		while ((got = fread(read_buf, sizeof(VerifyRecord), READ_BATCH, tmpFP)) > 0) {
			size_t j;
			for (j = 0; j < got; j++) {
				long slot = (-read_buf[j].rank) - (long) k * W;
				if (slot < 0 || slot >= sa_count) {
					fprintf(stderr,
					        "verify_%d: rank %ld (position %ld) maps to slot %ld outside [0, %ld)\n",
					        k, read_buf[j].rank, read_buf[j].position, slot, sa_count);
					fclose(tmpFP);
					return FAILURE;
				}
				if (seen[slot]) {
					fprintf(stderr,
					        "verify_%d: duplicate rank %ld at slot %ld\n",
					        k, read_buf[j].rank, slot);
					fclose(tmpFP);
					return FAILURE;
				}
				sorted_recs[slot] = read_buf[j];
				seen[slot] = 1;
				placed++;
			}
		}
		fclose(tmpFP);

		if (placed != sa_count) {
			fprintf(stderr,
			        "verify_%d: got %ld records but suffixarray_%d has %ld entries\n",
			        k, placed, k, sa_count);
			return FAILURE;
		}

		long i;
		for (i = 0; i < sa_count; i++) {
			if (sorted_recs[i].position != sa_buf[i]) {
				fprintf(stderr,
				        "SA mismatch at rank %ld: ranks_* says position %ld, suffixarray_%d[%ld] = %ld\n",
				        -((long) k * W + i), sorted_recs[i].position, k, i, sa_buf[i]);
				return FAILURE;
			}
			const VerifyRecord *prev = NULL;
			if (i > 0) prev = &sorted_recs[i - 1];
			else if (have_carry) prev = &carry;

			if (prev != NULL && !lex_less_strict(prev, &sorted_recs[i])) {
				fprintf(stderr,
				        "TEST FAILED at rank %ld: suffix at pos %ld not lex-less than suffix at pos %ld\n"
				        "  prev: char=%u sent_id=%d next_rank=%ld\n"
				        "  curr: char=%u sent_id=%d next_rank=%ld\n",
				        -((long) k * W + i),
				        prev->position, sorted_recs[i].position,
				        prev->char_val, prev->sent_id, prev->next_rank,
				        sorted_recs[i].char_val, sorted_recs[i].sent_id, sorted_recs[i].next_rank);
				return FAILURE;
			}
			total_verified++;
			if (total_verified % 10000000 == 0)
				printf("Verified %ld positions. Correct so far.\n", total_verified);
		}

		if (sa_count > 0) {
			carry = sorted_recs[sa_count - 1];
			have_carry = 1;
		}
	}

	printf("Test passed. Verified %ld SA positions.\n", total_verified);

	free(sorted_recs);
	free(seen);
	free(read_buf);
	free(sa_buf);
	return SUCCESS;
}

int main(int argc, char **argv) {
	if (argc < 7) {
		puts("Run ./verify <input_dir> <ranks_dir> <sa_dir> <tmp_dir> <total_chunks> <chunk_size>");
		return FAILURE;
	}
	const char *input_dir = argv[1];
	const char *ranks_dir = argv[2];
	const char *sa_dir = argv[3];
	const char *tmp_dir = argv[4];
	int total_chunks = atoi(argv[5]);
	long W = parse_chunk_size(argv[6]);

	if (total_chunks <= 0) {
		fprintf(stderr, "total_chunks must be positive (got %d)\n", total_chunks);
		return FAILURE;
	}

	if (verify_partition(input_dir, ranks_dir, tmp_dir, total_chunks, W) != SUCCESS)
		return FAILURE;
	if (verify_check(sa_dir, tmp_dir, total_chunks, W) != SUCCESS)
		return FAILURE;
	return SUCCESS;
}
