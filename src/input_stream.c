#include "input_stream.h"

int input_stream_open(InputStream *s, const char *input_file) {
	s->fp = NULL;
	OpenBinaryFileRead(&s->fp, (char *) input_file);
	s->pending_sentinel = 0;
	s->done = 0;
	return SUCCESS;
}

int input_stream_next(InputStream *s, uint32_t *out_char, int *out_sent_id) {
	if (s->done) return EMPTY;

	if (s->pending_sentinel) {
		*out_char = 0;
		*out_sent_id = 0;          // single string: sentinel id is always 0
		s->pending_sentinel = 0;
		s->done = 1;
		return SUCCESS;
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
}
