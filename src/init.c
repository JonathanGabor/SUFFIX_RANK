#include "utils.h"
#include "algorithm.h"
#include "input_stream.h"
#include "time.h"

// init.c reads input files directly (no separate input_to_binary stage).
// Each `word_length` consecutive bytes is packed big-endian into one symbol;
// real symbol values are shifted by +1 so value 0 is reserved as the
// per-file sentinel marker. Bucket sort uses a dense `counts[alphabet_size]`
// array, which is required to fit within the chunk budget:
// `alphabet_size = (1 << (8*word_length)) + 1 <= working_chunk_size`.

clock_t start, end;
double time_read;
double time_total, time_write = 0.0;
clock_t start_while, end_while;

// Bucket-sort input_buffer into sa_buffer using per-chunk symbol counts.
// counts[i] holds the frequency of symbol i in this chunk on entry; it is used
// as bucket positions and reset to 0 on exit.
static void init_sa(uint32_t *input_buffer, int *sa_buffer, long *counts, long alphabet_size, int total) {
	long i;
	long sum = 0, temp;
	for (i = 0; i < alphabet_size; i++) {
		temp = counts[i];
		counts[i] = sum;
		sum += temp;
	}
	for (i = 0; i < total; i++) {
		sa_buffer[counts[input_buffer[i]]] = i;
		counts[input_buffer[i]]++;
	}
	for (i = 0; i < alphabet_size; i++) counts[i] = 0;
}

// Flush the current chunk to disk: write ranks and initial SA.
// Resets pos_in_buffer to 0 and increments chunk_id.
static void flush_chunk(long *output_buffer, uint32_t *buffer, int *sa_buffer,
                        long *counts, long alphabet_size,
                        int *pos_in_buffer, int *chunk_id,
                        const char *output_directory,
                        FILE **outputFP, FILE **saFP) {
	char output_file_name[MAX_PATH_LENGTH];
	char sa_file_name[MAX_PATH_LENGTH];

	snprintf(output_file_name, sizeof output_file_name, "%s/ranks_%d", output_directory, *chunk_id);
	snprintf(sa_file_name, sizeof sa_file_name, "%s/sa_%d", output_directory, *chunk_id);
	OpenBinaryFileWrite(outputFP, output_file_name);
	OpenBinaryFileWrite(saFP, sa_file_name);

	if (TEST_PERFORMANCE) start = clock();

	Fwrite(output_buffer, sizeof(long), *pos_in_buffer, *outputFP);
	init_sa(buffer, sa_buffer, counts, alphabet_size, *pos_in_buffer);
	Fwrite(sa_buffer, sizeof(int), *pos_in_buffer, *saFP);

	if (TEST_PERFORMANCE) {
		end = clock();
		time_write += (double)(end - start) / CLOCKS_PER_SEC;
	}

	fclose(*outputFP);
	fclose(*saFP);
	(*chunk_id)++;
	*pos_in_buffer = 0;
}

// Pack `word_length` bytes (big-endian) into a uint32_t in [0, 2^(8*word_length)).
static inline uint32_t pack_be(const uint8_t *bytes, int word_length) {
	uint32_t v = 0;
	int i;
	for (i = 0; i < word_length; i++) {
		v = (v << 8) | bytes[i];
	}
	return v;
}

int count_characters(char *input_directory, char *output_directory,
                     long working_chunk_size, int word_length) {
	int i, j;
	long alphabet_size = (1L << (8 * word_length)) + 1L;  // +1 for sentinel value 0
	int n_files;

	char **files = collect_sorted_files(input_directory, &n_files);
	if (n_files == 0) {
		printf("No input files found in %s\n", input_directory);
		return FAILURE;
	}

	if (alphabet_size > working_chunk_size) {
		fprintf(stderr,
		        "alphabet_size %ld (word_length=%d) exceeds chunk_size %ld; "
		        "use a larger chunk size (must be >= alphabet_size)\n",
		        alphabet_size, word_length, working_chunk_size);
		free_sorted_files(files, n_files);
		return FAILURE;
	}

	printf("Current chunk size is set to %ld elements, word_length=%d, alphabet_size=%ld\n",
	       working_chunk_size, word_length, alphabet_size);

	long *counts = (long *) Calloc((size_t)alphabet_size * sizeof(long));
	long *ranks  = (long *) Calloc((size_t)alphabet_size * sizeof(long));
	FILE *outputFP = NULL;
	FILE *saFP = NULL;

	size_t read_buf_bytes = (size_t)working_chunk_size * sizeof(long);
	uint8_t *read_buf = (uint8_t *) Calloc(read_buf_bytes);

	if (TEST_PERFORMANCE) start = clock();

	// Pass 1: count symbol frequencies across all input files. Carry partial
	// (sub-word) trailing bytes across reads within a file; reject if EOF
	// leaves a partial word.
	for (j = 0; j < n_files; j++) {
		FILE *fp = NULL;
		OpenBinaryFileRead(&fp, files[j]);
		uint8_t carry[MAX_WORD_LENGTH];
		int carry_len = 0;
		size_t r;
		while ((r = fread(read_buf, 1, read_buf_bytes, fp)) > 0) {
			size_t cursor = 0;
			// If we have carry from a previous read, fill it up to a full word.
			if (carry_len > 0) {
				int need = word_length - carry_len;
				if ((size_t)need <= r) {
					memcpy(carry + carry_len, read_buf, need);
					counts[pack_be(carry, word_length) + 1]++;
					cursor = need;
					carry_len = 0;
				} else {
					memcpy(carry + carry_len, read_buf, r);
					carry_len += (int) r;
					continue;
				}
			}
			// Process complete words.
			size_t complete = ((r - cursor) / word_length) * word_length;
			size_t k;
			for (k = 0; k < complete; k += word_length) {
				counts[pack_be(read_buf + cursor + k, word_length) + 1]++;
			}
			cursor += complete;
			// Stash any remaining bytes for the next iteration.
			if (cursor < r) {
				carry_len = (int)(r - cursor);
				memcpy(carry, read_buf + cursor, carry_len);
			}
		}
		if (carry_len > 0) {
			fprintf(stderr,
			        "File %s has %d trailing byte(s) that don't fill a word_length=%d symbol\n",
			        files[j], carry_len, word_length);
			fclose(fp);
			free(counts); free(ranks); free(read_buf);
			free_sorted_files(files, n_files);
			return FAILURE;
		}
		fclose(fp);
	}
	counts[0] += n_files; // one sentinel per file

	if (TEST_PERFORMANCE) {
		end = clock();
		time_read = (double)(end - start) / CLOCKS_PER_SEC;
		printf("Global character count in %.4f\n", time_read);
	}

	// Compute initial rank for each symbol value (cumulative sum). All
	// sentinels share value 0 but will get unique negative ranks in pass 2.
	long rank = 0;
	for (i = 0; i < alphabet_size; i++) {
		ranks[i] = rank;
		rank += counts[i];
		counts[i] = 0; // reset for per-chunk bucket sort in pass 2
	}

	long     *output_buffer = (long *)     Calloc((size_t)working_chunk_size * sizeof(long));
	int      *sa_buffer     = (int *)      Calloc((size_t)working_chunk_size * sizeof(int));
	uint32_t *buffer        = (uint32_t *) Calloc((size_t)working_chunk_size * sizeof(uint32_t));

	int pos_in_buffer    = 0;
	int chunk_id         = 0;
	long current_sentinel = 0;

	if (TEST_PERFORMANCE) start_while = clock();

	// Pass 2: assign ranks and build initial per-chunk suffix arrays. Uses the
	// shared symbol iterator so verify.c sees the exact same stream.
	InputStream stream;
	if (input_stream_open(&stream, input_directory, word_length) != SUCCESS) {
		free_sorted_files(files, n_files);
		free(output_buffer); free(sa_buffer); free(buffer);
		free(read_buf); free(counts); free(ranks);
		return FAILURE;
	}
	uint32_t sym;
	int sent_id;
	int status;
	while ((status = input_stream_next(&stream, &sym, &sent_id)) == SUCCESS) {
		if (sent_id >= 0) {
			output_buffer[pos_in_buffer] = current_sentinel--;
			buffer[pos_in_buffer] = 0;
			counts[0]++;
		} else {
			output_buffer[pos_in_buffer] = ranks[sym];
			buffer[pos_in_buffer] = sym;
			counts[sym]++;
		}
		pos_in_buffer++;
		if (pos_in_buffer == working_chunk_size)
			flush_chunk(output_buffer, buffer, sa_buffer, counts, alphabet_size,
			            &pos_in_buffer, &chunk_id, output_directory,
			            &outputFP, &saFP);
	}
	input_stream_close(&stream);
	if (status == FAILURE) {
		free_sorted_files(files, n_files);
		free(output_buffer); free(sa_buffer); free(buffer);
		free(read_buf); free(counts); free(ranks);
		return FAILURE;
	}

	if (pos_in_buffer > 0)
		flush_chunk(output_buffer, buffer, sa_buffer, counts, alphabet_size,
		            &pos_in_buffer, &chunk_id, output_directory,
		            &outputFP, &saFP);

	if (TEST_PERFORMANCE) {
		end_while = clock();
		time_total = (double)(end_while - start_while) / CLOCKS_PER_SEC;
		printf("Init sa and rank in %.4f, write:%.4f\n", time_total, time_write);
	}

	free_sorted_files(files, n_files);
	free(output_buffer);
	free(sa_buffer);
	free(buffer);
	free(read_buf);
	free(counts);
	free(ranks);

	return SUCCESS;
}

int main(int argc, char **argv) {
	if (argc < 5) {
		puts("Run ./init <input_dir> <output_dir> <working_chunk_size> <word_length>");
		return FAILURE;
	}
	char *input_directory   = argv[1];
	char *output_directory  = argv[2];
	long working_chunk_size = parse_chunk_size(argv[3]);
	int word_length         = parse_word_length(argv[4]);
	return count_characters(input_directory, output_directory, working_chunk_size, word_length);
}
