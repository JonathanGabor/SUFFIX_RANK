// init_dss: divsufsort-based initialization.
//
// Drop-in replacement for `init`. Produces the same ranks_<id> files as init.c
// but a *true partial suffix array* sa_<id> per chunk (via libdivsufsort and a
// sascan-style reverse-block + gt_head pass) instead of init.c's bucket-sorted
// initial SA. This lets refine_dss skip tsort and emit RunRecords with a single
// linear scan.
//
// Restrictions: word_length == 1 and input bytes must leave room for one
// divsufsort byte code per file sentinel plus the Z-transform increment.

extern "C" {
#include "utils.h"
#include "input_stream.h"
}

#include <divsufsort.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "bitvector.hpp"
#include "srank.hpp"


// ---------- small helpers ----------------------------------------------------

static long file_size_or_die(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) {
		fprintf(stderr, "stat(%s) failed\n", path);
		exit(1);
	}
	return (long) st.st_size;
}

static void chunk_X_path(char *out, size_t cap, const char *dir, int id) {
	snprintf(out, cap, "%s/X_%d", dir, id);
}

static void chunk_gt_path(char *out, size_t cap, const char *dir, int id) {
	snprintf(out, cap, "%s/gt_head_%d", dir, id);
}

static void ranks_path(char *out, size_t cap, const char *dir, int id) {
	snprintf(out, cap, "%s/ranks_%d", dir, id);
}

static void sa_path(char *out, size_t cap, const char *dir, int id) {
	snprintf(out, cap, "%s/sa_%d", dir, id);
}

static void read_all(const char *path, uint8_t *buf, long n) {
	FILE *fp = NULL;
	OpenBinaryFileRead(&fp, (char *) path);
	if ((long) fread(buf, 1, (size_t) n, fp) != n) {
		fprintf(stderr, "short read on %s\n", path);
		exit(1);
	}
	fclose(fp);
}


// ---------- pass 1 + pass 2 --------------------------------------------------
// Pass 1: count symbol frequencies.
// Pass 2: re-read, assign ranks via cumulative-sum of counts, write
//   ranks_<id> and chunks/X_<id>. No bucket sort, no sa_<id>; those are
//   produced by pass 3.

static int write_chunk_pass2(FILE *ranks_fp, FILE *xfp,
                             long *output_buffer, uint8_t *encoded_buffer,
                             int pos_in_buffer) {
	Fwrite(output_buffer, sizeof(long), (size_t) pos_in_buffer, ranks_fp);
	Fwrite(encoded_buffer, sizeof(uint8_t), (size_t) pos_in_buffer, xfp);
	return 0;
}

static int run_passes_1_and_2(const char *input_directory,
                              const char *rank_directory,
                              const char *chunks_directory,
                              long working_chunk_size,
                              int word_length,
                              long *counts,
                              int *n_files_out,
                              int *n_chunks_out,
                              long *last_chunk_size_out) {
	int n_files = 0;
	char **files = collect_sorted_files(input_directory, &n_files);
	if (n_files == 0) {
		printf("No input files found in %s\n", input_directory);
		free_sorted_files(files, n_files);
		return FAILURE;
	}

	long alphabet_size = (1L << (8 * word_length)) + 1L;
	if (alphabet_size > working_chunk_size) {
		fprintf(stderr,
		        "alphabet_size %ld (word_length=%d) exceeds chunk_size %ld\n",
		        alphabet_size, word_length, working_chunk_size);
		free_sorted_files(files, n_files);
		return FAILURE;
	}

	long *ranks = (long *) Calloc((size_t) alphabet_size * sizeof(long));
	size_t read_buf_bytes = (size_t) working_chunk_size * sizeof(long);
	uint8_t *read_buf = (uint8_t *) Calloc(read_buf_bytes);

	// Pass 1: count symbol frequencies.
	for (int j = 0; j < n_files; j++) {
		FILE *fp = NULL;
		OpenBinaryFileRead(&fp, files[j]);
		uint8_t carry[MAX_WORD_LENGTH];
		int carry_len = 0;
		size_t r;
		while ((r = fread(read_buf, 1, read_buf_bytes, fp)) > 0) {
			size_t cursor = 0;
			if (carry_len > 0) {
				int need = word_length - carry_len;
				if ((size_t) need <= r) {
					memcpy(carry + carry_len, read_buf, (size_t) need);
					uint32_t v = 0;
					for (int k = 0; k < word_length; k++) v = (v << 8) | carry[k];
					counts[v + 1]++;
					cursor = (size_t) need;
					carry_len = 0;
				} else {
					memcpy(carry + carry_len, read_buf, r);
					carry_len += (int) r;
					continue;
				}
			}
			size_t complete = ((r - cursor) / (size_t) word_length) * (size_t) word_length;
			for (size_t k = 0; k < complete; k += (size_t) word_length) {
				uint32_t v = 0;
				for (int b = 0; b < word_length; b++)
					v = (v << 8) | read_buf[cursor + k + b];
				counts[v + 1]++;
			}
			cursor += complete;
			if (cursor < r) {
				carry_len = (int) (r - cursor);
				memcpy(carry, read_buf + cursor, (size_t) carry_len);
			}
		}
		if (carry_len > 0) {
			fprintf(stderr,
			        "File %s has %d trailing byte(s) that don't fill a word_length=%d symbol\n",
			        files[j], carry_len, word_length);
			fclose(fp);
			free(ranks); free(read_buf); free_sorted_files(files, n_files);
			return FAILURE;
		}
		fclose(fp);
	}
	counts[0] += n_files; // one sentinel per file

	// Divsufsort sees a byte string, so multi-file sentinels must be distinct
	// in that byte string too. Reserve codes [0, n_files) for file sentinels,
	// shift real bytes above them, and leave one extra code for Z += 1.
	if (word_length == 1) {
		if (n_files > 254) {
			fprintf(stderr,
			        "init_dss: %d input files require too many distinct sentinel "
			        "byte codes for the divsufsort path\n",
			        n_files);
			free(ranks); free(read_buf); free_sorted_files(files, n_files);
			return FAILURE;
		}
		long max_allowed_byte = 254L - n_files;
		for (long sym = max_allowed_byte + 2; sym < alphabet_size; sym++) {
			if (counts[sym] != 0) {
				fprintf(stderr,
				        "init_dss: input byte %ld occurs %ld time(s), but with "
				        "%d file sentinel(s) the divsufsort path only supports "
				        "bytes <= %ld\n",
				        sym - 1, counts[sym], n_files, max_allowed_byte);
				free(ranks); free(read_buf); free_sorted_files(files, n_files);
				return FAILURE;
			}
		}
	}

	// Cumulative-sum counts → ranks[symbol] gives the lowest rank for that symbol.
	long rank = 0;
	for (long i = 0; i < alphabet_size; i++) {
		ranks[i] = rank;
		rank += counts[i];
		// counts[] is intentionally NOT reset; no bucket-sort step needs it.
	}

	long *output_buffer = (long *) Calloc((size_t) working_chunk_size * sizeof(long));
	uint8_t *encoded_buffer = (uint8_t *) Calloc((size_t) working_chunk_size * sizeof(uint8_t));

	int pos_in_buffer = 0;
	int chunk_id = 0;
	long current_sentinel = 0;

	FILE *ranks_fp = NULL;
	FILE *xfp = NULL;
	char path[MAX_PATH_LENGTH];

	for (int j = 0; j < n_files; j++) {
		FILE *fp = NULL;
		OpenBinaryFileRead(&fp, files[j]);
		uint8_t carry[MAX_WORD_LENGTH];
		int carry_len = 0;
		size_t r;
		while ((r = fread(read_buf, 1, read_buf_bytes, fp)) > 0) {
			size_t cursor = 0;
			if (carry_len > 0) {
				int need = word_length - carry_len;
				if ((size_t) need <= r) {
					memcpy(carry + carry_len, read_buf, (size_t) need);
					uint32_t v = 0;
					for (int k = 0; k < word_length; k++) v = (v << 8) | carry[k];
					uint32_t sym = v + 1;
					output_buffer[pos_in_buffer] = ranks[sym];
					encoded_buffer[pos_in_buffer] = (uint8_t) (n_files + v);
					pos_in_buffer++;
					if (pos_in_buffer == working_chunk_size) {
						ranks_path(path, sizeof path, rank_directory, chunk_id);
						OpenBinaryFileWrite(&ranks_fp, path);
						chunk_X_path(path, sizeof path, chunks_directory, chunk_id);
						OpenBinaryFileWrite(&xfp, path);
						write_chunk_pass2(ranks_fp, xfp, output_buffer, encoded_buffer, pos_in_buffer);
						fclose(ranks_fp); fclose(xfp);
						chunk_id++;
						pos_in_buffer = 0;
					}
					cursor = (size_t) need;
					carry_len = 0;
				} else {
					memcpy(carry + carry_len, read_buf, r);
					carry_len += (int) r;
					continue;
				}
			}
			while (cursor + (size_t) word_length <= r) {
				uint32_t v = 0;
				for (int b = 0; b < word_length; b++)
					v = (v << 8) | read_buf[cursor + b];
				uint32_t sym = v + 1;
				output_buffer[pos_in_buffer] = ranks[sym];
				encoded_buffer[pos_in_buffer] = (uint8_t) (n_files + v);
				pos_in_buffer++;
				cursor += (size_t) word_length;
				if (pos_in_buffer == working_chunk_size) {
					ranks_path(path, sizeof path, rank_directory, chunk_id);
					OpenBinaryFileWrite(&ranks_fp, path);
					chunk_X_path(path, sizeof path, chunks_directory, chunk_id);
					OpenBinaryFileWrite(&xfp, path);
					write_chunk_pass2(ranks_fp, xfp, output_buffer, encoded_buffer, pos_in_buffer);
					fclose(ranks_fp); fclose(xfp);
					chunk_id++;
					pos_in_buffer = 0;
				}
			}
			if (cursor < r) {
				carry_len = (int) (r - cursor);
				memcpy(carry, read_buf + cursor, (size_t) carry_len);
			}
		}
		fclose(fp);

		// Per-file sentinel.
		output_buffer[pos_in_buffer] = current_sentinel--;
		encoded_buffer[pos_in_buffer] = (uint8_t) j;
		pos_in_buffer++;
		if (pos_in_buffer == working_chunk_size) {
			ranks_path(path, sizeof path, rank_directory, chunk_id);
			OpenBinaryFileWrite(&ranks_fp, path);
			chunk_X_path(path, sizeof path, chunks_directory, chunk_id);
			OpenBinaryFileWrite(&xfp, path);
			write_chunk_pass2(ranks_fp, xfp, output_buffer, encoded_buffer, pos_in_buffer);
			fclose(ranks_fp); fclose(xfp);
			chunk_id++;
			pos_in_buffer = 0;
		}
	}

	long last_chunk_size = 0;
	if (pos_in_buffer > 0) {
		ranks_path(path, sizeof path, rank_directory, chunk_id);
		OpenBinaryFileWrite(&ranks_fp, path);
		chunk_X_path(path, sizeof path, chunks_directory, chunk_id);
		OpenBinaryFileWrite(&xfp, path);
		write_chunk_pass2(ranks_fp, xfp, output_buffer, encoded_buffer, pos_in_buffer);
		fclose(ranks_fp); fclose(xfp);
		last_chunk_size = pos_in_buffer;
		chunk_id++;
		pos_in_buffer = 0;
	} else if (chunk_id > 0) {
		last_chunk_size = working_chunk_size;
	}

	*n_chunks_out = chunk_id;
	*n_files_out = n_files;
	*last_chunk_size_out = last_chunk_size;

	free_sorted_files(files, n_files);
	free(output_buffer);
	free(encoded_buffer);
	free(read_buf);
	free(ranks);

	return SUCCESS;
}


// ---------- pass 3 -----------------------------------------------------------
// Reverse-chunk pass: for each chunk_id (from n_chunks-1 down to 0):
//   * Load X from chunks/X_<id>.
//   * If not the last chunk:
//       - Load Y from chunks/X_<id+1>, gt_head from chunks/gt_head_<id+1>.
//       - Compute gt_eof_bv(Y, X, gt_head, gt_eof).
//       - Apply the SAscan Z transform; sentinel byte codes are exempt.
//   * If n_chunks > 1: compute new_gt_head from current X (Z-transformed for
//     non-last chunks; raw for the last) and save as chunks/gt_head_<id>.
//   * divsufsort(X, sa_buffer, n_X); write sa_<id>.

static int run_pass_3(const char *rank_directory,
                      const char *chunks_directory,
                      long working_chunk_size,
                      int n_chunks,
                      int n_files,
                      long last_chunk_size) {
	if (n_chunks == 0) return SUCCESS;

	// X holds the current chunk (size up to working_chunk_size).
	// A holds the next chunk Y.
	uint8_t *X = (uint8_t *) Calloc((size_t) working_chunk_size * sizeof(uint8_t));
	uint8_t *A = (uint8_t *) Calloc((size_t) working_chunk_size * sizeof(uint8_t));
	int    *sa = (int *)    Calloc((size_t) working_chunk_size * sizeof(int));

	char path[MAX_PATH_LENGTH];

	for (int chunk_id = n_chunks - 1; chunk_id >= 0; chunk_id--) {
		chunk_X_path(path, sizeof path, chunks_directory, chunk_id);
		long n_X = file_size_or_die(path);
		read_all(path, X, n_X);

		if (chunk_id < n_chunks - 1) {
			// Build A = Y, the next chunk. This gives gtX:Y, used by
			// the Z transform from SAscan Lemma 1.
			chunk_X_path(path, sizeof path, chunks_directory, chunk_id + 1);
			long n_next = file_size_or_die(path);
			read_all(path, A, n_next);
			long A_length = n_next;

			chunk_gt_path(path, sizeof path, chunks_directory, chunk_id + 1);
			bitvector *gt_head_bv = new bitvector(std::string(path));
			bitvector *gt_eof_bv  = new bitvector(n_X);

			compute_gt_eof_bv(A, A_length, X, n_X, gt_head_bv, gt_eof_bv);

			// Z transform. Sentinel codes [0, n_files) stay fixed.
			const uint8_t last = X[n_X - 1];
			for (long j = 0; j < n_X; j++) {
				if (X[j] >= n_files &&
				    (j == n_X - 1 || X[j] > last ||
				     (X[j] == last && gt_eof_bv->get(j + 1)))) {
					X[j] = (uint8_t) (X[j] + 1);
				}
			}

			delete gt_eof_bv;
			delete gt_head_bv;

			// Consumed: next chunk's X and gt_head are no longer needed.
			chunk_X_path(path, sizeof path, chunks_directory, chunk_id + 1);
			unlink(path);
			chunk_gt_path(path, sizeof path, chunks_directory, chunk_id + 1);
			unlink(path);
		}

		if (n_chunks > 1) {
			bitvector *new_gt_head_bv = new bitvector(n_X);
			compute_new_gt_head_bv(X, n_X, new_gt_head_bv);
			chunk_gt_path(path, sizeof path, chunks_directory, chunk_id);
			new_gt_head_bv->save(std::string(path));
			delete new_gt_head_bv;
		}

		if (n_X > (long) 0x7fffffffL) {
			fprintf(stderr,
			        "init_dss: chunk size %ld exceeds 32-bit divsufsort limit\n",
			        n_X);
			free(X); free(A); free(sa);
			return FAILURE;
		}
		divsufsort((const unsigned char *) X, sa, (int32_t) n_X);

		sa_path(path, sizeof path, rank_directory, chunk_id);
		FILE *sa_fp = NULL;
		OpenBinaryFileWrite(&sa_fp, path);
		Fwrite(sa, sizeof(int), (size_t) n_X, sa_fp);
		fclose(sa_fp);
	}

	// Clean up the leftovers for chunk 0 (its X and gt_head are never consumed).
	chunk_X_path(path, sizeof path, chunks_directory, 0);
	unlink(path);
	chunk_gt_path(path, sizeof path, chunks_directory, 0);
	unlink(path);

	free(X);
	free(A);
	free(sa);
	(void) last_chunk_size; // unused; sizes come from fstat per chunk
	return SUCCESS;
}


// ---------- driver -----------------------------------------------------------

static int init_dss(const char *input_directory,
                    const char *rank_directory,
                    const char *chunks_directory,
                    long working_chunk_size,
                    int word_length) {
	if (word_length != 1) {
		fprintf(stderr, "init_dss: only word_length=1 is supported (got %d)\n",
		        word_length);
		return FAILURE;
	}

	long alphabet_size = (1L << (8 * word_length)) + 1L;
	long *counts = (long *) Calloc((size_t) alphabet_size * sizeof(long));
	int n_chunks = 0;
	int n_files = 0;
	long last_chunk_size = 0;

	clock_t t0 = TEST_PERFORMANCE ? clock() : 0;
	int s = run_passes_1_and_2(input_directory, rank_directory, chunks_directory,
	                           working_chunk_size, word_length,
	                           counts, &n_files, &n_chunks, &last_chunk_size);
	if (TEST_PERFORMANCE) {
		clock_t t1 = clock();
		printf("init_dss passes 1+2 in %.4f\n",
		       (double) (t1 - t0) / CLOCKS_PER_SEC);
	}
	free(counts);
	if (s != SUCCESS) return s;

	clock_t t2 = TEST_PERFORMANCE ? clock() : 0;
	s = run_pass_3(rank_directory, chunks_directory, working_chunk_size,
	               n_chunks, n_files, last_chunk_size);
	if (TEST_PERFORMANCE) {
		clock_t t3 = clock();
		printf("init_dss pass 3 (divsufsort + gt) in %.4f\n",
		       (double) (t3 - t2) / CLOCKS_PER_SEC);
	}

	printf("init_dss: %d chunk(s), chunk_size=%ld, word_length=%d\n",
	       n_chunks, working_chunk_size, word_length);
	return s;
}

int main(int argc, char **argv) {
	if (argc < 6) {
		puts("Run ./init_dss <input_dir> <rank_dir> <chunks_dir> <working_chunk_size> <word_length>");
		return FAILURE;
	}
	const char *input_directory   = argv[1];
	const char *rank_directory    = argv[2];
	const char *chunks_directory  = argv[3];
	long working_chunk_size       = parse_chunk_size(argv[4]);
	int word_length               = parse_word_length(argv[5]);
	return init_dss(input_directory, rank_directory, chunks_directory,
	                working_chunk_size, word_length);
}
