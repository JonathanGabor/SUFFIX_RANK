#include "utils.h"
#include <dirent.h>
#include <sys/stat.h>

// Brute-force suffix-array verifier. Mirrors init.c's view of the input:
//   * source files are enumerated in sorted lexicographic order
//   * each `word_length` consecutive bytes is packed big-endian into one symbol
//     value, shifted by +1 (so 0 is reserved)
//   * one sentinel (value 0) is appended after each file
// Adjacent SA entries are verified to be in non-decreasing suffix order over the
// concatenated symbol stream. Sentinels are smaller than any real symbol; among
// sentinels, the one belonging to the earlier-enumerated file sorts first.

static int compare_str(const void *a, const void *b) {
	return strcmp(*(const char **)a, *(const char **)b);
}

static char **collect_sorted_files(const char *dir, int *n_out) {
	DIR *dp;
	struct dirent *ep;
	int n = 0, cap = 16;
	char **files = (char **) malloc(cap * sizeof(char *));
	struct stat st;
	char path[MAX_PATH_LENGTH];

	dp = opendir(dir);
	if (dp == NULL) {
		fprintf(stderr, "Cannot open directory %s\n", dir);
		exit(FAILURE);
	}
	while ((ep = readdir(dp)) != NULL) {
		if (ep->d_name[0] == '.') continue;
		snprintf(path, MAX_PATH_LENGTH, "%s/%s", dir, ep->d_name);
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		if (n == cap) {
			cap *= 2;
			files = (char **) realloc(files, cap * sizeof(char *));
		}
		files[n++] = strdup(path);
	}
	closedir(dp);
	qsort(files, n, sizeof(char *), compare_str);
	*n_out = n;
	return files;
}

int main(int argc, char **argv) {
	if (argc < 4) {
		puts("Usage: ./test_sa <input_dir> <sa_file> <word_length>");
		return FAILURE;
	}
	const char *input_dir = argv[1];
	const char *sa_file = argv[2];
	int word_length = atoi(argv[3]);
	if (word_length < 1 || word_length > 4) {
		fprintf(stderr, "word_length must be in [1, 4]\n");
		return FAILURE;
	}

	int n_files;
	char **files = collect_sorted_files(input_dir, &n_files);
	if (n_files == 0) {
		fprintf(stderr, "No input files in %s\n", input_dir);
		return FAILURE;
	}

	// Determine each file's byte length and the total symbol count.
	long *file_bytes = (long *) calloc(n_files, sizeof(long));
	long total_bytes = 0;
	for (int i = 0; i < n_files; i++) {
		FILE *fp = fopen(files[i], "rb");
		if (!fp) { perror(files[i]); return FAILURE; }
		fseek(fp, 0, SEEK_END);
		file_bytes[i] = ftell(fp);
		fclose(fp);
		if (file_bytes[i] % word_length != 0) {
			fprintf(stderr, "%s has %ld bytes (not a multiple of word_length %d)\n",
			        files[i], file_bytes[i], word_length);
			return FAILURE;
		}
		total_bytes += file_bytes[i];
	}
	long total_symbols = total_bytes / word_length + (long) n_files;

	uint32_t *sym = (uint32_t *) calloc(total_symbols, sizeof(uint32_t));
	int *sent_id = (int *) malloc(total_symbols * sizeof(int));
	for (long i = 0; i < total_symbols; i++) sent_id[i] = -1;

	// Populate the symbol stream.
	long pos = 0;
	for (int i = 0; i < n_files; i++) {
		FILE *fp = fopen(files[i], "rb");
		uint8_t word_buf[4];
		long bytes_read_total = 0;
		while (bytes_read_total < file_bytes[i]) {
			size_t got = fread(word_buf, 1, word_length, fp);
			if ((int) got != word_length) {
				fprintf(stderr, "Unexpected short read in %s\n", files[i]);
				return FAILURE;
			}
			uint32_t v = 0;
			for (int k = 0; k < word_length; k++) v = (v << 8) | word_buf[k];
			sym[pos++] = v + 1;
			bytes_read_total += word_length;
		}
		fclose(fp);
		sym[pos] = 0;        // sentinel for this file
		sent_id[pos] = i;
		pos++;
	}

	// Walk the SA, comparing adjacent suffixes.
	FILE *saFP = NULL;
	OpenBinaryFileRead(&saFP, (char *) sa_file);

	long prev_pos = -1, curr_pos;
	long count = 0;
	while (fread(&curr_pos, sizeof(long), 1, saFP) == 1) {
		if (prev_pos >= 0) {
			long a = prev_pos, b = curr_pos;
			int cmp = 0;
			while (a < total_symbols && b < total_symbols) {
				uint32_t va = sym[a], vb = sym[b];
				int sa_is_sent = (va == 0);
				int sb_is_sent = (vb == 0);
				if (sa_is_sent && sb_is_sent) {
					// Sentinels are always at unique positions across files,
					// so the ids differ unless a == b (same SA position).
					if (sent_id[a] != sent_id[b]) {
						cmp = (sent_id[a] < sent_id[b]) ? -1 : 1;
					}
					break;
				}
				if (sa_is_sent) { cmp = -1; break; }
				if (sb_is_sent) { cmp = 1; break; }
				if (va != vb) { cmp = (va < vb) ? -1 : 1; break; }
				a++; b++;
			}
			if (cmp == 0) {
				// One side hit total_symbols without resolving; shorter sorts first.
				if (a == total_symbols && b < total_symbols) cmp = -1;
				else if (b == total_symbols && a < total_symbols) cmp = 1;
			}
			if (cmp > 0) {
				fprintf(stderr,
				        "TEST FAILED at SA index %ld: suffix at pos %ld > suffix at pos %ld\n",
				        count, prev_pos, curr_pos);
				fclose(saFP);
				return FAILURE;
			}
		}
		prev_pos = curr_pos;
		count++;
		if (count % 10000000 == 0)
			printf("Processed %ld positions. Correct so far.\n", count);
	}
	fclose(saFP);

	free(sym);
	free(sent_id);
	free(file_bytes);
	for (int i = 0; i < n_files; i++) free(files[i]);
	free(files);

	printf("Test passed. Verified %ld SA positions.\n", count);
	return SUCCESS;
}
