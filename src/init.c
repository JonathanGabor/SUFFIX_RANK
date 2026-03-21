#include "utils.h"
#include <dirent.h>
#include <sys/stat.h>
#include "time.h"

clock_t start, end;
double time_read;
double time_total, time_write = 0.0;
clock_t start_while, end_while;


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
		printf("Cannot open directory %s\n", dir);
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

// Bucket-sort input_buffer into sa_buffer using per-chunk character counts.
// counts[i] holds the frequency of char i in this chunk on entry;
// it is used as bucket positions and reset to 0 on exit.
static void init_sa(unsigned int *input_buffer, int *sa_buffer, long *counts, int total) {
	int i;
	long sum = 0, temp;
	for (i = 0; i < 256; i++) {
		temp = counts[i];
		counts[i] = sum;
		sum += temp;
	}
	for (i = 0; i < total; i++) {
		sa_buffer[counts[input_buffer[i]]] = i;
		counts[input_buffer[i]]++;
	}
	for (i = 0; i < 256; i++) counts[i] = 0;
}

// Flush the current chunk to disk: write ranks and initial SA.
// Resets pos_in_buffer to 0 and increments chunk_id.
static void flush_chunk(long *output_buffer, unsigned int *buffer, int *sa_buffer,
                        long *counts, int *pos_in_buffer, int *chunk_id,
                        const char *output_directory,
                        FILE **outputFP, FILE **saFP) {
	char output_file_name[MAX_PATH_LENGTH];
	char sa_file_name[MAX_PATH_LENGTH];

	sprintf(output_file_name, "%s/ranks_%d", output_directory, *chunk_id);
	sprintf(sa_file_name, "%s/sa_%d", output_directory, *chunk_id);
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

int count_characters(char *input_directory, char *output_directory) {
	int i, j;
	long counts[256] = {0};
	long ranks[256]  = {0};
	FILE *outputFP = NULL;
	FILE *saFP = NULL;
	char line[MAX_LINE];
	int line_len;
	int n_files;

	char **files = collect_sorted_files(input_directory, &n_files);
	if (n_files == 0) {
		printf("No input files found in %s\n", input_directory);
		return FAILURE;
	}

	printf("Current chunk size is set to %ld bytes\n", (long) WORKING_CHUNK_SIZE);

	if (TEST_PERFORMANCE) {
		start = clock();
	}

	// Pass 1: count character frequencies across all input files
	for (j = 0; j < n_files; j++) {
		FILE *fp = NULL;
		OpenFileRead(&fp, files[j]);
		while (fgets(line, MAX_LINE - 1, fp) != NULL) {
			line[strcspn(line, "\r\n")] = '\0';
			line_len = strlen(line);
			for (i = 0; i < line_len; i++) {
				unsigned int val = DEFAULT_CHAR;
				if (line[i] > 0 && (unsigned char)line[i] < 255)
					val = (unsigned int)(unsigned char)line[i];
				counts[val]++;
			}
		}
		fclose(fp);
	}
	counts[0] += n_files; // account for one sentinel (value 0) per file

	if (TEST_PERFORMANCE) {
		end = clock();
		time_read = (double)(end - start) / CLOCKS_PER_SEC;
		printf("Global character count in %.4f\n", time_read);
	}

	// Compute initial rank for each character (cumulative sum in sorted order)
	long rank = 0;
	for (i = 0; i < 256; i++) {
		ranks[i] = rank;
		rank += counts[i];
		counts[i] = 0; // reset for per-chunk bucket sort in pass 2
	}

	long         *output_buffer = (long *)         Calloc(WORKING_CHUNK_SIZE * sizeof(long));
	int          *sa_buffer     = (int *)           Calloc(WORKING_CHUNK_SIZE * sizeof(int));
	unsigned int *buffer        = (unsigned int *)  Calloc(WORKING_CHUNK_SIZE * sizeof(unsigned int));

	int pos_in_buffer    = 0;
	int chunk_id         = 0;
	int current_sentinel = 0;

	if (TEST_PERFORMANCE) {
		start_while = clock();
	}

	// Pass 2: assign ranks and build initial per-chunk suffix arrays
	for (j = 0; j < n_files; j++) {
		FILE *fp = NULL;
		OpenFileRead(&fp, files[j]);
		while (fgets(line, MAX_LINE - 1, fp) != NULL) {
			line[strcspn(line, "\r\n")] = '\0';
			line_len = strlen(line);
			for (i = 0; i < line_len; i++) {
				unsigned int val = DEFAULT_CHAR;
				if (line[i] > 0 && (unsigned char)line[i] < 255)
					val = (unsigned int)(unsigned char)line[i];
				output_buffer[pos_in_buffer] = ranks[val];
				buffer[pos_in_buffer]        = val;
				counts[val]++;
				pos_in_buffer++;
				if (pos_in_buffer == WORKING_CHUNK_SIZE)
					flush_chunk(output_buffer, buffer, sa_buffer, counts,
					            &pos_in_buffer, &chunk_id, output_directory,
					            &outputFP, &saFP);
			}
		}
		fclose(fp);

		// Append sentinel (value 0) at the end of each input file
		output_buffer[pos_in_buffer] = current_sentinel--;
		buffer[pos_in_buffer]        = 0;
		counts[0]++;
		pos_in_buffer++;
		if (pos_in_buffer == WORKING_CHUNK_SIZE)
			flush_chunk(output_buffer, buffer, sa_buffer, counts,
			            &pos_in_buffer, &chunk_id, output_directory,
			            &outputFP, &saFP);
	}

	// Flush the final partial chunk
	if (pos_in_buffer > 0)
		flush_chunk(output_buffer, buffer, sa_buffer, counts,
		            &pos_in_buffer, &chunk_id, output_directory,
		            &outputFP, &saFP);

	if (TEST_PERFORMANCE) {
		end_while = clock();
		time_total = (double)(end_while - start_while) / CLOCKS_PER_SEC;
		printf("Init sa and rank in %.4f, write:%.4f\n", time_total, time_write);
	}

	for (j = 0; j < n_files; j++) free(files[j]);
	free(files);
	free(output_buffer);
	free(sa_buffer);
	free(buffer);

	return SUCCESS;
}

int main(int argc, char **argv) {
	char *input_directory;
	char *output_directory;
	if (argc < 3) {
		puts("Run ./init <input_text_dir> <output_dir>");
		return FAILURE;
	}
	input_directory  = argv[1];
	output_directory = argv[2];
	return count_characters(input_directory, output_directory);
}
