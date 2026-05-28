#ifndef INPUT_STREAM_H
#define INPUT_STREAM_H

#include "utils.h"

// Iterator over the symbol stream defined by init.cpp and verify.c:
//   * regular files in `input_dir`, enumerated in sorted lexicographic order
//   * each `word_length` consecutive bytes packed big-endian into a uint32_t
//   * real symbol values shifted by +1, so 0 is reserved as the per-file
//     sentinel marker
//   * exactly one sentinel emitted after each file
typedef struct {
	char **files;
	int n_files;
	int cur_file;          // 0 .. n_files; n_files means stream exhausted
	int word_length;
	FILE *fp;              // open handle for files[cur_file], or NULL
	int pending_sentinel;  // 1 once the current file's bytes are exhausted
} InputStream;

// Open the stream. Returns SUCCESS or FAILURE.
int  input_stream_open(InputStream *s, const char *input_dir, int word_length);

// Emit the next symbol.
//   *out_char   = T[p] (0 for sentinel, packed_value+1 for real symbol)
//   *out_sent_id = file index if sentinel, -1 otherwise
// Returns SUCCESS, EMPTY (stream exhausted), or FAILURE (trailing partial word).
int  input_stream_next(InputStream *s, uint32_t *out_char, int *out_sent_id);

void input_stream_close(InputStream *s);

// Enumerate regular files in `dir` in sorted lexicographic order. Hidden
// (dot-prefixed) entries are skipped. Caller owns the returned array and each
// string; free via free_sorted_files.
char **collect_sorted_files(const char *dir, int *n_out);
void   free_sorted_files(char **files, int n);

#endif
