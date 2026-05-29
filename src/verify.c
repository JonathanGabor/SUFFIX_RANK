#include "utils.h"
#include "algorithm.h"
#include "input_stream.h"
#include <limits.h>

// External-memory SA correctness checker.
//
// Phase A: stream input symbols + ranks_* in position order. For each position
// p emit a compact VerifyRecord (local rank slot, position, next_rank,
// first_key), routed into the destination rank-chunk file tmp/verify_<(-rank)/W>.
// Mirrors create_pairs.c's partition-by-rank-bucket pattern.
//
// Phase B: per rank-chunk, counting-sort records into rank order using
// slot recorded in each temp record. This simultaneously verifies that ranks_*
// forms a permutation of [-N+1, 0]. Then stream-check positions against
// suffixarray_<k>[i] (SA inverts ISA), and adjacent-compare suffixes via
// (first_key, next_rank).

#define WRITE_BUF_RECORDS 1024
#define READ_BATCH 4096
#define SA_READ_BATCH 4096

typedef struct verify_record {
	long position;       // global position p
	long next_rank;      // ranks_*[p+1], or LONG_MAX if p == N-1
	uint32_t slot;       // local rank slot in the destination rank chunk
	uint32_t first_key;  // sentinels first by file id, then real symbols
} VerifyRecord;

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

static uint32_t encode_first_key(const InputStream *stream, uint32_t char_val, int sent_id) {
	if (char_val == 0) return (uint32_t) sent_id;
	return (uint32_t) stream->n_files + char_val;
}

static int verify_partition(const char *input_dir, const char *ranks_dir,
                            const char *tmp_dir, int total_chunks,
                            long W) {
	InputStream stream;
	if (input_stream_open(&stream, input_dir) != SUCCESS) {
		return FAILURE;
	}
	if (W > (long) UINT32_MAX) {
		fprintf(stderr, "verify requires chunk_size <= UINT32_MAX for local rank slots\n");
		input_stream_close(&stream);
		return FAILURE;
	}

	PartitionWriter *writers = (PartitionWriter *) Calloc((size_t) total_chunks * sizeof(PartitionWriter));
	int k;
	for (k = 0; k < total_chunks; k++) pw_init(&writers[k], tmp_dir, k);

	int32_t *R = (int32_t *) Calloc((size_t) W * sizeof(int32_t));
	char ranks_path[MAX_PATH_LENGTH];
	char peek_path[MAX_PATH_LENGTH];

	int chunk_id;
	for (chunk_id = 0; chunk_id < total_chunks; chunk_id++) {
		snprintf(ranks_path, sizeof ranks_path, "%s/ranks_%d", ranks_dir, chunk_id);
		FILE *ranksFP;
		OpenBinaryFileRead(&ranksFP, ranks_path);
		long count = (long) fread(R, sizeof(int32_t), (size_t) W, ranksFP);
		fclose(ranksFP);

		int32_t next_first;
		int have_next_first = 0;
		if (chunk_id + 1 < total_chunks) {
			snprintf(peek_path, sizeof peek_path, "%s/ranks_%d", ranks_dir, chunk_id + 1);
			FILE *peekFP;
			OpenBinaryFileRead(&peekFP, peek_path);
			if (fread(&next_first, sizeof(int32_t), 1, peekFP) != 1) {
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
			long slot = (-cur_rank) - target * W;
			if (target < 0 || target >= total_chunks) {
				fprintf(stderr,
				        "Rank %ld at position %ld maps to bucket %ld (out of [0, %d))\n",
				        cur_rank, (long) chunk_id * W + i, target, total_chunks);
				return FAILURE;
			}
			if (slot < 0 || slot > (long) UINT32_MAX) {
				fprintf(stderr,
				        "Rank %ld at position %ld maps to local slot %ld outside uint32 range\n",
				        cur_rank, (long) chunk_id * W + i, slot);
				return FAILURE;
			}

			VerifyRecord rec;
			rec.position = (long) chunk_id * W + i;
			rec.next_rank = next_rank;
			rec.slot = (uint32_t) slot;
			rec.first_key = encode_first_key(&stream, cv, sid);
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

static int bitset_get(const uint8_t *bits, long i) {
	return (bits[(size_t) i >> 3] >> (i & 7)) & 1U;
}

static void bitset_set(uint8_t *bits, long i) {
	bits[(size_t) i >> 3] |= (uint8_t) (1U << (i & 7));
}

static int file_record_count(FILE *fp, const char *path, long *count_out) {
	if (fseeko(fp, 0, SEEK_END) != 0) {
		fprintf(stderr, "Failed to seek %s\n", path);
		return FAILURE;
	}
	off_t bytes = ftello(fp);
	if (bytes < 0) {
		fprintf(stderr, "Failed to tell %s\n", path);
		return FAILURE;
	}
	if ((uintmax_t) bytes % sizeof(long) != 0) {
		fprintf(stderr, "%s size is not a whole number of suffix array entries\n", path);
		return FAILURE;
	}
	if ((uintmax_t) bytes / sizeof(long) > (uintmax_t) LONG_MAX) {
		fprintf(stderr, "%s has too many suffix array entries for this platform\n", path);
		return FAILURE;
	}
	*count_out = (long) ((uintmax_t) bytes / sizeof(long));
	if (fseeko(fp, 0, SEEK_SET) != 0) {
		fprintf(stderr, "Failed to rewind %s\n", path);
		return FAILURE;
	}
	return SUCCESS;
}

// Strict lex-less on the encoded (first_key, next_rank) annotation. Equal
// pairs count as violations because SA entries must be strictly ordered.
static int lex_less_strict(uint32_t p_first_key, long p_next_rank,
                           uint32_t c_first_key, long c_next_rank) {
	if (p_first_key != c_first_key) return p_first_key < c_first_key;
	// Tie on T[p]: lex-smaller iff next_rank is lex-smaller. Rank 0 is the
	// lex-smallest in this codebase's sign convention, so lex_smaller(a,b)
	// numerically is a > b.
	return p_next_rank > c_next_rank;
}

static int verify_check(const char *sa_dir, const char *tmp_dir,
                        int total_chunks, long W) {
	long *positions = (long *) Calloc((size_t) W * sizeof(long));
	long *next_ranks = (long *) Calloc((size_t) W * sizeof(long));
	uint32_t *first_keys = (uint32_t *) Calloc((size_t) W * sizeof(uint32_t));
	uint8_t *seen = (uint8_t *) Calloc(((size_t) W + 7U) / 8U);
	VerifyRecord *read_buf = (VerifyRecord *) Calloc(READ_BATCH * sizeof(VerifyRecord));
	long *sa_read_buf = (long *) Calloc(SA_READ_BATCH * sizeof(long));

	long carry_position = 0;
	long carry_next_rank = 0;
	uint32_t carry_first_key = 0;
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
		long sa_count;
		if (file_record_count(saFP, sa_path, &sa_count) != SUCCESS) {
			fclose(saFP);
			return FAILURE;
		}
		if (sa_count > W) {
			fprintf(stderr,
			        "suffixarray_%d has %ld entries, exceeding chunk size %ld\n",
			        k, sa_count, W);
			fclose(saFP);
			return FAILURE;
		}

		memset(seen, 0, ((size_t) sa_count + 7U) / 8U);

		FILE *tmpFP;
		OpenBinaryFileRead(&tmpFP, tmp_path);
		long placed = 0;
		size_t got;
		while ((got = fread(read_buf, sizeof(VerifyRecord), READ_BATCH, tmpFP)) > 0) {
			size_t j;
			for (j = 0; j < got; j++) {
				long slot = (long) read_buf[j].slot;
				if (slot < 0 || slot >= sa_count) {
					fprintf(stderr,
					        "verify_%d: position %ld maps to slot %ld outside [0, %ld)\n",
					        k, read_buf[j].position, slot, sa_count);
					fclose(tmpFP);
					fclose(saFP);
					return FAILURE;
				}
				if (bitset_get(seen, slot)) {
					fprintf(stderr,
					        "verify_%d: duplicate record for slot %ld\n", k, slot);
					fclose(tmpFP);
					fclose(saFP);
					return FAILURE;
				}
				positions[slot] = read_buf[j].position;
				next_ranks[slot] = read_buf[j].next_rank;
				first_keys[slot] = read_buf[j].first_key;
				bitset_set(seen, slot);
				placed++;
			}
		}
		fclose(tmpFP);

		if (placed != sa_count) {
			fprintf(stderr,
			        "verify_%d: got %ld records but suffixarray_%d has %ld entries\n",
			        k, placed, k, sa_count);
			fclose(saFP);
			return FAILURE;
		}

		long i = 0;
		size_t sa_got;
		while ((sa_got = fread(sa_read_buf, sizeof(long), SA_READ_BATCH, saFP)) > 0) {
			size_t j;
			for (j = 0; j < sa_got; j++, i++) {
				if (i >= sa_count) {
					fprintf(stderr, "suffixarray_%d has more entries than expected\n", k);
					fclose(saFP);
					return FAILURE;
				}
				if (positions[i] != sa_read_buf[j]) {
					fprintf(stderr,
					        "SA mismatch at rank %ld: ranks_* says position %ld, suffixarray_%d[%ld] = %ld\n",
					        -((long) k * W + i), positions[i], k, i, sa_read_buf[j]);
					fclose(saFP);
					return FAILURE;
				}

				int has_prev = 0;
				long prev_position = 0;
				long prev_next_rank = 0;
				uint32_t prev_first_key = 0;
				if (i > 0) {
					has_prev = 1;
					prev_position = positions[i - 1];
					prev_next_rank = next_ranks[i - 1];
					prev_first_key = first_keys[i - 1];
				} else if (have_carry) {
					has_prev = 1;
					prev_position = carry_position;
					prev_next_rank = carry_next_rank;
					prev_first_key = carry_first_key;
				}

				if (has_prev &&
				    !lex_less_strict(prev_first_key, prev_next_rank, first_keys[i], next_ranks[i])) {
					fprintf(stderr,
					        "TEST FAILED at rank %ld: suffix at pos %ld not lex-less than suffix at pos %ld\n"
					        "  prev: first_key=%u next_rank=%ld\n"
					        "  curr: first_key=%u next_rank=%ld\n",
					        -((long) k * W + i),
					        prev_position, positions[i],
					        prev_first_key, prev_next_rank,
					        first_keys[i], next_ranks[i]);
					fclose(saFP);
					return FAILURE;
				}
				total_verified++;
				if (total_verified % 10000000 == 0)
					printf("Verified %ld positions. Correct so far.\n", total_verified);
			}
		}
		fclose(saFP);
		if (i != sa_count) {
			fprintf(stderr,
			        "suffixarray_%d: expected %ld entries but read %ld\n",
			        k, sa_count, i);
			return FAILURE;
		}

		if (sa_count > 0) {
			carry_position = positions[sa_count - 1];
			carry_next_rank = next_ranks[sa_count - 1];
			carry_first_key = first_keys[sa_count - 1];
			have_carry = 1;
		}
	}

	printf("Test passed. Verified %ld SA positions.\n", total_verified);

	free(positions);
	free(next_ranks);
	free(first_keys);
	free(seen);
	free(read_buf);
	free(sa_read_buf);
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
