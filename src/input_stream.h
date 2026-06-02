#ifndef INPUT_STREAM_H
#define INPUT_STREAM_H

#include "utils.h"

// Iterator over the symbol stream of the single input file (matching init.cpp):
//   * byte symbols packed into uint32_t values
//   * real symbol values shifted by +1, so 0 is reserved as the sentinel marker
//   * exactly one sentinel emitted after the file's bytes
typedef struct {
	FILE *fp;              // open handle, or NULL once bytes are exhausted
	int pending_sentinel;  // 1 once the file's bytes are exhausted
	int done;              // 1 once the trailing sentinel has been emitted
} InputStream;

// Open the stream over input_file. Returns SUCCESS or FAILURE.
int  input_stream_open(InputStream *s, const char *input_file);

// Emit the next symbol.
//   *out_char    = T[p] (0 for the sentinel, packed_value+1 for a real symbol)
//   *out_sent_id = 0 if sentinel, -1 otherwise
// Returns SUCCESS, EMPTY (stream exhausted), or FAILURE.
int  input_stream_next(InputStream *s, uint32_t *out_char, int *out_sent_id);

void input_stream_close(InputStream *s);

#endif
