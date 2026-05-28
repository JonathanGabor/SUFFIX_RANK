#include "input_stream.h"
#include <dirent.h>
#include <sys/stat.h>

static int compare_str(const void *a, const void *b) {
	return strcmp(*(const char **)a, *(const char **)b);
}

char **collect_sorted_files(const char *dir, int *n_out) {
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

void free_sorted_files(char **files, int n) {
	int i;
	for (i = 0; i < n; i++) free(files[i]);
	free(files);
}

int input_stream_open(InputStream *s, const char *input_dir) {
	s->files = collect_sorted_files(input_dir, &s->n_files);
	if (s->n_files == 0) {
		fprintf(stderr, "No input files in %s\n", input_dir);
		return FAILURE;
	}
	s->cur_file = 0;
	s->fp = NULL;
	s->pending_sentinel = 0;
	return SUCCESS;
}

int input_stream_next(InputStream *s, uint32_t *out_char, int *out_sent_id) {
	if (s->cur_file >= s->n_files) return EMPTY;

	if (s->pending_sentinel) {
		*out_char = 0;
		*out_sent_id = s->cur_file;
		s->pending_sentinel = 0;
		s->cur_file++;
		return SUCCESS;
	}

	if (s->fp == NULL) {
		OpenBinaryFileRead(&s->fp, s->files[s->cur_file]);
	}

	int byte = fgetc(s->fp);
	if (byte == EOF) {
		fclose(s->fp);
		s->fp = NULL;
		s->pending_sentinel = 1;
		return input_stream_next(s, out_char, out_sent_id);
	}
	*out_char = (uint32_t) byte + 1;
	*out_sent_id = -1;
	return SUCCESS;
}

void input_stream_close(InputStream *s) {
	if (s->fp != NULL) {
		fclose(s->fp);
		s->fp = NULL;
	}
	if (s->files != NULL) {
		free_sorted_files(s->files, s->n_files);
		s->files = NULL;
	}
	s->n_files = 0;
	s->cur_file = 0;
}
