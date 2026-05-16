#include "utils.h"

// Converts a single input text file into the binary representation consumed by
// `init`. Each byte of the source becomes one uint16_t in the output, then one
// uint16_t sentinel (value 0) is appended to mark end-of-string.
//
// File-as-string semantics: bytes pass through verbatim (newlines included).
// Literal NUL bytes in the source would collide with the sentinel, so they are
// replaced with DEFAULT_CHAR (0x23 '#') and a single warning is printed for
// the file.
//
// Usage: ./input_to_binary <text_file> <output_dir> <working_chunk_size>
// The output is appended to <output_dir>/binary_input, so callers loop over
// every input file in order.
//
// Buffer sizing: the read and write buffers together consume the same memory
// as one working chunk of longs (`working_chunk_size * sizeof(long)` bytes),
// split evenly. This matches the per-buffer memory footprint of the rest of
// the pipeline so input_to_binary doesn't become the high-water mark.

// DEFAULT_CHAR (= 35 = '#') is defined in utils.h; used to replace literal NUL bytes.

int main (int argc, char ** argv) {
	if (argc < 4) {
		printf("Run ./input_to_binary <text_file> <output_dir> <working_chunk_size>\n");
		return FAILURE;
	}

	const char *file_name = argv[1];
	const char *output_directory = argv[2];
	long working_chunk_size = parse_chunk_size(argv[3]);

	size_t total_budget_bytes = (size_t)working_chunk_size * sizeof(long);
	size_t read_buf_bytes = total_budget_bytes / 2;
	size_t write_buf_elems = (total_budget_bytes - read_buf_bytes) / sizeof(uint16_t);
	if (read_buf_bytes < 1) read_buf_bytes = 1;
	if (write_buf_elems < 1) write_buf_elems = 1;

	FILE *inputFP = NULL;
	FILE *outputFP = NULL;
	OpenBinaryFileRead(&inputFP, (char *)file_name);

	char output_file_name[MAX_PATH_LENGTH];
	snprintf(output_file_name, sizeof output_file_name, "%s/binary_input", output_directory);
	OpenBinaryFileAppend(&outputFP, output_file_name);

	unsigned char *read_buf = (unsigned char *) Calloc(read_buf_bytes);
	uint16_t *write_buf = (uint16_t *) Calloc(write_buf_elems * sizeof(uint16_t));

	long total_chars = 0;
	long nul_count = 0;
	int warned = 0;
	size_t n;

	while ((n = fread(read_buf, 1, read_buf_bytes, inputFP)) > 0) {
		size_t i = 0;
		while (i < n) {
			size_t room = write_buf_elems;
			size_t take = (n - i < room) ? (n - i) : room;
			for (size_t k = 0; k < take; k++) {
				unsigned char b = read_buf[i + k];
				if (b == 0) {
					nul_count++;
					if (!warned) {
						fprintf(stderr,
						        "WARNING: %s contains NUL byte(s); replacing with '#' (0x%02X) to preserve sentinel uniqueness\n",
						        file_name, DEFAULT_CHAR);
						warned = 1;
					}
					write_buf[k] = DEFAULT_CHAR;
				} else {
					write_buf[k] = (uint16_t) b;
				}
			}
			Fwrite(write_buf, sizeof(uint16_t), take, outputFP);
			total_chars += take;
			i += take;
		}
	}

	// Append per-file sentinel.
	uint16_t sentinel = 0;
	Fwrite(&sentinel, sizeof(uint16_t), 1, outputFP);

	fclose(inputFP);
	fclose(outputFP);
	free(read_buf);
	free(write_buf);

	if (nul_count > 0) {
		fprintf(stderr, "WARNING: %s: %ld NUL byte(s) replaced\n", file_name, nul_count);
	}
	printf("Wrote %ld chars + 1 sentinel from %s\n", total_chars, file_name);
	return SUCCESS;
}
