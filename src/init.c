#include "utils.h"
#include "time.h"

clock_t start, end;
double time_read;
double time_total, time_write = 0.0;
clock_t start_while, end_while;

// Bucket-sort input_buffer into sa_buffer using per-chunk symbol counts.
// counts[i] holds the frequency of symbol i in this chunk on entry; it is used
// as bucket positions and reset to 0 on exit.
static void init_sa(uint16_t *input_buffer, int *sa_buffer, long *counts, int total) {
	int i;
	long sum = 0, temp;
	for (i = 0; i < ALPHABET_SIZE; i++) {
		temp = counts[i];
		counts[i] = sum;
		sum += temp;
	}
	for (i = 0; i < total; i++) {
		sa_buffer[counts[input_buffer[i]]] = i;
		counts[input_buffer[i]]++;
	}
	for (i = 0; i < ALPHABET_SIZE; i++) counts[i] = 0;
}

// Flush the current chunk to disk: write ranks and initial SA.
// Resets pos_in_buffer to 0 and increments chunk_id.
static void flush_chunk(long *output_buffer, uint16_t *buffer, int *sa_buffer,
                        long *counts, int *pos_in_buffer, int *chunk_id,
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
	init_sa(buffer, sa_buffer, counts, *pos_in_buffer);
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

int count_characters(char *input_directory, char *output_directory, long working_chunk_size) {
	int i;
	long *counts = (long *) Calloc(ALPHABET_SIZE * sizeof(long));
	long *ranks  = (long *) Calloc(ALPHABET_SIZE * sizeof(long));
	FILE *outputFP = NULL;
	FILE *saFP = NULL;

	char input_file_name[MAX_PATH_LENGTH];
	snprintf(input_file_name, sizeof input_file_name, "%s/binary_input", input_directory);

	printf("Current chunk size is set to %ld elements\n", working_chunk_size);

	if (TEST_PERFORMANCE) {
		start = clock();
	}

	// Pass 1: count symbol frequencies across the binary stream.
	// A read buffer of 64K uint16_t is plenty for streaming.
	const long count_buf_elems = 65536;
	uint16_t *count_buf = (uint16_t *) Calloc(count_buf_elems * sizeof(uint16_t));
	{
		FILE *fp = NULL;
		OpenBinaryFileRead(&fp, input_file_name);
		size_t r;
		while ((r = fread(count_buf, sizeof(uint16_t), count_buf_elems, fp)) > 0) {
			for (size_t j = 0; j < r; j++) {
				counts[count_buf[j]]++;
			}
		}
		fclose(fp);
	}
	long total_sentinels = counts[0];
	if (total_sentinels == 0) {
		printf("No sentinels found in %s/binary_input (empty input?)\n", input_directory);
		free(counts); free(ranks); free(count_buf);
		return FAILURE;
	}

	if (TEST_PERFORMANCE) {
		end = clock();
		time_read = (double)(end - start) / CLOCKS_PER_SEC;
		printf("Global character count in %.4f\n", time_read);
	}

	// Compute initial rank for each symbol value (cumulative sum). All sentinels
	// share input value 0 but will be assigned unique negative ranks in pass 2.
	long rank = 0;
	for (i = 0; i < ALPHABET_SIZE; i++) {
		ranks[i] = rank;
		rank += counts[i];
		counts[i] = 0; // reset for per-chunk bucket sort in pass 2
	}

	long    *output_buffer = (long *)     Calloc(working_chunk_size * sizeof(long));
	int     *sa_buffer     = (int *)      Calloc(working_chunk_size * sizeof(int));
	uint16_t *buffer       = (uint16_t *) Calloc(working_chunk_size * sizeof(uint16_t));

	int pos_in_buffer = 0;
	int chunk_id      = 0;
	long current_sentinel = 0;

	if (TEST_PERFORMANCE) {
		start_while = clock();
	}

	// Pass 2: assign ranks and build initial per-chunk suffix arrays.
	{
		FILE *fp = NULL;
		OpenBinaryFileRead(&fp, input_file_name);
		size_t r;
		while ((r = fread(count_buf, sizeof(uint16_t), count_buf_elems, fp)) > 0) {
			for (size_t j = 0; j < r; j++) {
				uint16_t val = count_buf[j];
				if (val == 0) {
					// Per-file sentinel: gets a unique non-positive rank.
					output_buffer[pos_in_buffer] = current_sentinel--;
				} else {
					output_buffer[pos_in_buffer] = ranks[val];
				}
				buffer[pos_in_buffer] = val;
				counts[val]++;
				pos_in_buffer++;
				if (pos_in_buffer == working_chunk_size)
					flush_chunk(output_buffer, buffer, sa_buffer, counts,
					            &pos_in_buffer, &chunk_id, output_directory,
					            &outputFP, &saFP);
			}
		}
		fclose(fp);
	}

	// Flush the final partial chunk.
	if (pos_in_buffer > 0)
		flush_chunk(output_buffer, buffer, sa_buffer, counts,
		            &pos_in_buffer, &chunk_id, output_directory,
		            &outputFP, &saFP);

	if (TEST_PERFORMANCE) {
		end_while = clock();
		time_total = (double)(end_while - start_while) / CLOCKS_PER_SEC;
		printf("Init sa and rank in %.4f, write:%.4f\n", time_total, time_write);
	}

	free(output_buffer);
	free(sa_buffer);
	free(buffer);
	free(count_buf);
	free(counts);
	free(ranks);

	return SUCCESS;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		puts("Run ./init <input_binary_dir> <output_dir> <working_chunk_size>");
		return FAILURE;
	}
	char *input_directory  = argv[1];
	char *output_directory = argv[2];
	long working_chunk_size = parse_chunk_size(argv[3]);
	return count_characters(input_directory, output_directory, working_chunk_size);
}
