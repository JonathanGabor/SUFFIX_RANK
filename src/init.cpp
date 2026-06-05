// init: divsufsort-based initialization.
//
// Produces ranks_<id> files and a true partial suffix array sa_<id> per chunk
// via libdivsufsort and a SAscan-style reverse-block + gt_head pass.
//
// Restrictions: byte alphabet only. Input bytes must leave room for one
// divsufsort byte code per file sentinel plus the Z-transform increment.
//
// The partial suffix sorting pass uses ideas and small helper code adapted from
// SAscan by Juha Karkkainen and Dominik Kempa, MIT licensed. See README.md for
// the citation and license acknowledgement.

extern "C" {
#include "utils.h"
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


// ---------- k-mer initial bucket sort ----------------------------------------
// Single input file (one trailing sentinel). The initial ranks encode the first
// k characters (not just one), so the prefix-doubling loop can start at prefix
// length k. We:
//   * scan the input to find which byte values occur (sigma_real distinct),
//   * pick the largest k whose k-mer histogram (B^k entries, B = sigma_real + 1)
//     fits the RAM budget,
//   * bucket every real position by its first-k-character key K and assign
//     rank = 1 + (#real positions with a smaller key).  (Slot 0 is the sentinel.)
//
// Symbol-code alphabet (used ONLY for the key K, never for the divsufsort X
// stream which keeps its raw 1+byte encoding):
//   sentinel -> 0                          (smallest)
//   real byte b -> 1 + densecode(b)        in [1, B)
//
// The key window stops at the trailing sentinel and pads with 0 afterwards. The
// sentinel is unique (one per string) and smaller than every real symbol, so any
// window touching it yields a unique K: that suffix is a singleton resolved
// immediately, and the pad value past the sentinel never affects ordering.

#define INIT_READ_BUF_BYTES (4L * 1024 * 1024)

// Sliding k-mer-key window. Feeding code e (the symbol at stream index p) rolls
// the base-B key to window [p-k+1, p] and reports the real position it completes:
// emit_pos = p-(k-1), valid when emit_pos >= 0 (and, by construction, < length).
// Codes before index 0 are treated as 0.
typedef struct {
	long *ring;   // k slots: codes of the last k fed positions
	long  key;    // base-B value of the current window
	long  topw;   // B^(k-1)
	long  B;
	int   k;
	long  p;      // feed index
} KWin;

static inline void kwin_reset(KWin *w) {
	w->key = 0;
	w->p = 0;
	for (int i = 0; i < w->k; i++) w->ring[i] = 0;
}

static inline long kwin_feed(KWin *w, long e, long *outK) {
	long idx = w->p % w->k;
	long old = w->ring[idx];
	w->ring[idx] = e;
	w->key = (w->key - old * w->topw) * w->B + e;
	long emit_pos = w->p - (w->k - 1);
	w->p++;
	*outK = w->key;
	return emit_pos;
}

// Pass B: write the divsufsort byte chunks chunks/X_<id> (raw 1+byte encoding,
// sentinel byte 0 -- unchanged) and accumulate the k-mer histogram counts[K].
// Returns the chunk count and last chunk size (which pass C reproduces
// identically: same positions, same order, same chunk size).
static int pass_build_histogram_and_X(const char *input_file,
                                      const long *code_of_byte,
                                      long B, int k, long topw,
                                      const char *chunks_directory,
                                      long working_chunk_size,
                                      long *counts, long *ring,
                                      uint8_t *read_buf,
                                      int *n_chunks_out, long *last_chunk_size_out) {
	uint8_t *encoded_buffer = (uint8_t *) Calloc((size_t) working_chunk_size * sizeof(uint8_t));
	KWin w; w.ring = ring; w.B = B; w.k = k; w.topw = topw;
	kwin_reset(&w);

	int pos_in_buffer = 0;
	int chunk_id = 0;
	FILE *xfp = NULL;
	char path[MAX_PATH_LENGTH];

	#define FLUSH_X_IF_FULL() do { \
		if (pos_in_buffer == working_chunk_size) { \
			chunk_X_path(path, sizeof path, chunks_directory, chunk_id); \
			OpenBinaryFileWrite(&xfp, path); \
			Fwrite(encoded_buffer, sizeof(uint8_t), (size_t) pos_in_buffer, xfp); \
			fclose(xfp); \
			chunk_id++; pos_in_buffer = 0; \
		} \
	} while (0)

	FILE *fp = NULL;
	OpenBinaryFileRead(&fp, (char *) input_file);
	size_t r;
	long K;
	while ((r = fread(read_buf, 1, INIT_READ_BUF_BYTES, fp)) > 0) {
		for (size_t cursor = 0; cursor < r; cursor++) {
			uint32_t v = read_buf[cursor];
			encoded_buffer[pos_in_buffer++] = (uint8_t) (1 + v);
			FLUSH_X_IF_FULL();
			long emit_pos = kwin_feed(&w, code_of_byte[v], &K);
			if (emit_pos >= 0) counts[K]++;
		}
	}
	fclose(fp);

	// Flush the window for the last k-1 real positions: the sentinel (code 0)
	// and pads are both 0, so feed k-1 zeros (counts only).
	for (int extra = 0; extra < k - 1; extra++) {
		long K2;
		long emit_pos = kwin_feed(&w, 0, &K2);
		if (emit_pos >= 0) counts[K2]++;
	}

	// Trailing sentinel byte (code 0) in the X stream.
	encoded_buffer[pos_in_buffer++] = 0;
	FLUSH_X_IF_FULL();

	long last_chunk_size = 0;
	if (pos_in_buffer > 0) {
		chunk_X_path(path, sizeof path, chunks_directory, chunk_id);
		OpenBinaryFileWrite(&xfp, path);
		Fwrite(encoded_buffer, sizeof(uint8_t), (size_t) pos_in_buffer, xfp);
		fclose(xfp);
		last_chunk_size = pos_in_buffer;
		chunk_id++;
	} else if (chunk_id > 0) {
		last_chunk_size = working_chunk_size;
	}
	#undef FLUSH_X_IF_FULL

	*n_chunks_out = chunk_id;
	*last_chunk_size_out = last_chunk_size;
	free(encoded_buffer);
	return SUCCESS;
}

// Pass C: write ranks_<id>. Real position -> 1 + bucketstart[K(i)]; the trailing
// sentinel -> 0 (the unique smallest, "already resolved"). Ranks are appended in
// global position order even though K(i) is only known once k-1 later symbols have
// been read (the window emits real positions in order; the sentinel rank is
// appended last).
static int pass_write_ranks(const char *input_file,
                            const long *code_of_byte,
                            long B, int k, long topw,
                            const char *rank_directory,
                            long working_chunk_size,
                            const long *bucketstart, long *ring,
                            uint8_t *read_buf) {
	int40 *output_buffer = (int40 *) Calloc((size_t) working_chunk_size * sizeof(int40));
	KWin w; w.ring = ring; w.B = B; w.k = k; w.topw = topw;
	kwin_reset(&w);

	int pos_in_buffer = 0;
	int chunk_id = 0;
	FILE *ranks_fp = NULL;
	char path[MAX_PATH_LENGTH];

	#define APPEND_RANK(val) do { \
		i40_store(&output_buffer[pos_in_buffer++], (val)); \
		if (pos_in_buffer == working_chunk_size) { \
			ranks_path(path, sizeof path, rank_directory, chunk_id); \
			OpenBinaryFileWrite(&ranks_fp, path); \
			Fwrite(output_buffer, sizeof(int40), (size_t) pos_in_buffer, ranks_fp); \
			fclose(ranks_fp); \
			chunk_id++; pos_in_buffer = 0; \
		} \
	} while (0)

	FILE *fp = NULL;
	OpenBinaryFileRead(&fp, (char *) input_file);
	size_t r;
	long K;
	while ((r = fread(read_buf, 1, INIT_READ_BUF_BYTES, fp)) > 0) {
		for (size_t cursor = 0; cursor < r; cursor++) {
			uint32_t v = read_buf[cursor];
			long emit_pos = kwin_feed(&w, code_of_byte[v], &K);
			if (emit_pos >= 0) APPEND_RANK(1 + bucketstart[K]);
		}
	}
	fclose(fp);

	for (int extra = 0; extra < k - 1; extra++) {
		long K2;
		long emit_pos = kwin_feed(&w, 0, &K2);
		if (emit_pos >= 0) APPEND_RANK(1 + bucketstart[K2]);
	}

	// Trailing sentinel rank (unique, == 0).
	APPEND_RANK(0);

	if (pos_in_buffer > 0) {
		ranks_path(path, sizeof path, rank_directory, chunk_id);
		OpenBinaryFileWrite(&ranks_fp, path);
		Fwrite(output_buffer, sizeof(int40), (size_t) pos_in_buffer, ranks_fp);
		fclose(ranks_fp);
	}
	#undef APPEND_RANK

	free(output_buffer);
	return SUCCESS;
}

// Drive the count scan, k selection, histogram, and the two writing passes.
static int run_passes_1_and_2(const char *input_file,
                              const char *rank_directory,
                              const char *chunks_directory,
                              long working_chunk_size,
                              int *n_chunks_out,
                              long *last_chunk_size_out,
                              int *k_out) {
	long alphabet_size = BYTE_ALPHABET_SIZE + 1L;
	long *counts = (long *) Calloc((size_t) alphabet_size * sizeof(long));
	uint8_t *read_buf = (uint8_t *) Calloc((size_t) INIT_READ_BUF_BYTES);

	// Pass 1: count byte frequencies (counts[byte+1]).
	{
		FILE *fp = NULL;
		OpenBinaryFileRead(&fp, (char *) input_file);
		size_t r;
		while ((r = fread(read_buf, 1, (size_t) INIT_READ_BUF_BYTES, fp)) > 0) {
			for (size_t c = 0; c < r; c++)
				counts[(uint32_t) read_buf[c] + 1]++;
		}
		fclose(fp);
	}

	// Divsufsort sees a byte string: reserve byte code 0 for the sentinel, shift
	// real bytes up by 1, and leave code 255 for the SAscan Z-transform's +1.
	// So real input bytes must be <= 253.
	long max_allowed_byte = 253L;
	for (long sym = max_allowed_byte + 2; sym < alphabet_size; sym++) {
		if (counts[sym] != 0) {
			fprintf(stderr,
			        "init: input byte %ld occurs %ld time(s), but the divsufsort "
			        "path only supports bytes <= %ld\n",
			        sym - 1, counts[sym], max_allowed_byte);
			free(counts); free(read_buf);
			return FAILURE;
		}
	}

	// Dense, order-preserving code per used byte; code_of_byte = 1 + dense
	// (slot 0 is the sentinel).
	long *code_of_byte = (long *) Calloc((size_t) BYTE_ALPHABET_SIZE * sizeof(long));
	long sigma_real = 0;
	for (long b = 0; b < BYTE_ALPHABET_SIZE; b++) {
		if (counts[b + 1] != 0) code_of_byte[b] = 1 + sigma_real++;
	}
	long B = sigma_real + 1;

	// Largest k >= 1 with B^k <= budget (entries). budget = working_chunk_size
	// keeps the 8-byte histogram within ~8*chunk_size of the 20*chunk_size model.
	long budget = working_chunk_size;
	int k = 1;
	long hist_size = B;                 // B^1
	while (hist_size <= budget / B) {   // would B^(k+1) still fit?
		hist_size *= B;
		k++;
	}
	long topw = hist_size / B;          // B^(k-1)

	long *counts_kmer = (long *) Calloc((size_t) hist_size * sizeof(long));
	long *ring = (long *) Calloc((size_t) k * sizeof(long));

	printf("init: alphabet sigma=%ld, base B=%ld, k=%d (histogram %ld entries)\n",
	       sigma_real, B, k, hist_size);

	// Pass B: X chunks + k-mer histogram.
	int n_chunks = 0;
	long last_chunk_size = 0;
	int s = pass_build_histogram_and_X(input_file, code_of_byte, B, k, topw,
	                                   chunks_directory, working_chunk_size,
	                                   counts_kmer, ring, read_buf,
	                                   &n_chunks, &last_chunk_size);
	if (s != SUCCESS) {
		free(counts); free(read_buf); free(code_of_byte);
		free(counts_kmer); free(ring);
		return s;
	}

	// Cumulative sum -> bucketstart[K] = #real positions with a smaller key.
	long acc = 0;
	for (long i = 0; i < hist_size; i++) {
		long c = counts_kmer[i];
		counts_kmer[i] = acc;
		acc += c;
	}

	// Pass C: ranks chunks.
	s = pass_write_ranks(input_file, code_of_byte, B, k, topw,
	                     rank_directory, working_chunk_size,
	                     counts_kmer, ring, read_buf);

	*n_chunks_out = n_chunks;
	*last_chunk_size_out = last_chunk_size;
	*k_out = k;

	free(counts);
	free(read_buf);
	free(code_of_byte);
	free(counts_kmer);
	free(ring);
	return s;
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
                      int n_sentinel_codes,   // reserved low byte codes (1: the sentinel)
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

			// Z transform. Sentinel codes [0, n_sentinel_codes) stay fixed.
			const uint8_t last = X[n_X - 1];
			for (long j = 0; j < n_X; j++) {
				if (X[j] >= n_sentinel_codes &&
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
			        "init: chunk size %ld exceeds 32-bit divsufsort limit\n",
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

static int init_suffixrank(const char *input_file,
                           const char *rank_directory,
                           const char *chunks_directory,
                           long working_chunk_size) {
	int n_chunks = 0;
	long last_chunk_size = 0;
	int k = 1;

	clock_t t0 = TEST_PERFORMANCE ? clock() : 0;
	int s = run_passes_1_and_2(input_file, rank_directory, chunks_directory,
	                           working_chunk_size,
	                           &n_chunks, &last_chunk_size, &k);
	if (TEST_PERFORMANCE) {
		clock_t t1 = clock();
		printf("init passes 1+2 in %.4f\n",
		       (double) (t1 - t0) / CLOCKS_PER_SEC);
	}
	if (s != SUCCESS) return s;

	// The doubling loop starts at prefix length k (see suffixrank.sh).
	char path[MAX_PATH_LENGTH];
	snprintf(path, sizeof path, "%s/kmer_length", rank_directory);
	FILE *kfp = NULL;
	OpenFileWrite(&kfp, path);
	fprintf(kfp, "%d\n", k);
	fclose(kfp);

	clock_t t2 = TEST_PERFORMANCE ? clock() : 0;
	s = run_pass_3(rank_directory, chunks_directory, working_chunk_size,
	               n_chunks, 1 /* one reserved sentinel code */, last_chunk_size);
	if (TEST_PERFORMANCE) {
		clock_t t3 = clock();
		printf("init pass 3 (divsufsort + gt) in %.4f\n",
		       (double) (t3 - t2) / CLOCKS_PER_SEC);
	}

	printf("init: %d chunk(s), chunk_size=%ld, k=%d, byte alphabet\n",
	       n_chunks, working_chunk_size, k);
	return s;
}

int main(int argc, char **argv) {
	if (argc < 5) {
		puts("Run ./init <input_file> <rank_dir> <chunks_dir> <working_chunk_size>");
		return FAILURE;
	}
	const char *input_file        = argv[1];
	const char *rank_directory    = argv[2];
	const char *chunks_directory  = argv[3];
	long working_chunk_size       = parse_chunk_size(argv[4]);
	return init_suffixrank(input_file, rank_directory, chunks_directory,
	                       working_chunk_size);
}
