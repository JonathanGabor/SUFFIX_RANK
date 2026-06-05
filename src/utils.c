#include "utils.h"
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

static void report_open_failure(const char *description, const char *file_name) {
	fprintf(stderr, "Could not open %s \"%s\": %s\n",
			description, file_name, strerror(errno));
}

// Benchmark knob: when SUFFIXRANK_NOCACHE is set to a non-zero value, every file
// opened through these wrappers bypasses the OS page cache. This simulates the
// target environment (100s-of-GB inputs where RAM is fully consumed by the
// algorithm's working set, so scratch/input I/O never gets cached) on an
// otherwise-idle dev machine that would happily cache the small benchmark files.
// On macOS this uses F_NOCACHE; elsewhere it best-effort advises POSIX_FADV_DONTNEED.
static int nocache_enabled(void) {
	static int cached = -1;
	if (cached < 0) {
		const char *v = getenv("SUFFIXRANK_NOCACHE");
		cached = (v && v[0] && strcmp(v, "0") != 0) ? 1 : 0;
	}
	return cached;
}

static void apply_nocache(FILE *fp) {
	if (!fp || !nocache_enabled()) return;
	int fd = fileno(fp);
	if (fd < 0) return;
#if defined(__APPLE__)
	fcntl(fd, F_NOCACHE, 1);
#elif defined(POSIX_FADV_DONTNEED)
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#else
	(void) fd;
#endif
}

void * Calloc (size_t num_bytes) {
	void * result =  calloc (num_bytes, 1);
	if (result == NULL) {
		printf ("Could not allocate array of size %zu bytes\n", num_bytes);
		exit (1);
	}
	return result;
}

// Parse a chunk-size CLI argument. Must be a positive integer. Need not be a
// power of two: refine/update locate the next rank via division/modulo, and the
// position math elsewhere only relies on chunks being uniformly sized.
long parse_chunk_size (const char *arg) {
	char *end = NULL;
	long value = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || value <= 0) {
		fprintf(stderr, "Invalid chunk size \"%s\": must be a positive integer\n", arg);
		exit(1);
	}
	// Chunk size is kept within 32-bit int to avoid 64-bit-index complexity;
	// in practice it is never this large anyway.
	if (value > INT_MAX) {
		fprintf(stderr, "Invalid chunk size \"%s\": must be <= %d\n", arg, INT_MAX);
		exit(1);
	}
	return value;
}

void OpenBinaryFileRead (FILE ** fp, char * file_name) {
	if(!(*fp= fopen ( file_name, "rb" )))  {
		report_open_failure("input binary file for reading", file_name);
		exit (1);
	}
	apply_nocache(*fp);
}

void OpenBinaryFileReadWrite (FILE ** fp, char * file_name) {
	if(!(*fp= fopen ( file_name, "r+b" ))) {
		report_open_failure("input binary file for reading and writing", file_name);
		exit (1);
	}
	apply_nocache(*fp);
}

void OpenBinaryFileWrite (FILE ** fp, char * file_name) {
	if(!(*fp= fopen ( file_name, "wb" )))  {
		report_open_failure("output binary file for writing", file_name);
		exit (1);
	}
	apply_nocache(*fp);
}


void OpenBinaryFileAppend (FILE **fp, char * file_name) {
	if(!(*fp= fopen ( file_name, "ab" )))  {
		report_open_failure("binary file for appending", file_name);
		exit (1);
	}
	apply_nocache(*fp);
}

void Fwrite (const void *buffer, size_t elem_size, size_t num_elements, FILE *fp ) {
	size_t written = fwrite (buffer, elem_size, num_elements, fp);
	if (written != num_elements) {
		printf ("Failed to write %zu elements to file: fwrite returned %zu\n", num_elements, written);
		exit (1);
	}
}

void OpenFileWrite (FILE ** fp, char * file_name) {
	if(!(*fp= fopen ( file_name, "w" )))   {
		report_open_failure("file for writing", file_name);
		exit (1);
	}
	apply_nocache(*fp);
}

void OpenFileRead (FILE ** fp, char * file_name) {
	if(!(*fp= fopen ( file_name, "r" )))   {
		report_open_failure("file for reading", file_name);
		exit (1);
	}
	apply_nocache(*fp);
}

//reference <citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.14.8162&rep=rep1&type=pdf> p1260
void tsort(int *sa, long *next_ranks, int n){
	int a, b, c, d, l, h, s, tmp;
	long v;
	if (n <= 1) return;
	if (n < 5) {
		int i;
		for (i = 1; i < n; i++) {
			int j = i;
			while (KEY(j) < KEY(j-1)) {
				SWAP(j, j-1);
				j--;
				if (j==0) break;
			}
		}
		return;
	}
	v = KEY(n/2);
	if (n > 5) {
		long v1, v2;
		v1 = KEY(0);
		v2 = KEY(n-1);
		if (n > 40) {
			s = n/8;
			v1 = MED3(v1, KEY(s), KEY(2*s));
			v = MED3(KEY(n/2-s), v, KEY(n/2+s));
			v2 = MED3(KEY(n-1-2*s), KEY(n-1-s), v2);
		}
		v = MED3(v, v1, v2);
	}
	a = b = 0;
	c = d = n-1;
	for (;;) {
		while (b <= c && v >= KEY(b)) {
			if (v == KEY(b)) {
				SWAP(a, b);
				a++;
			}
			b++;
		}
		while (c >= b && KEY(c) >= v) {
			if (v == KEY(c)) {
				SWAP(d, c);
				d--;
			}
			c--;
		}
		if (b > c) break;
		SWAP(b, c);
		b++;
		c--;
	}
	s = MIN(a, b-a);
	for(l = 0, h = b-s; s; s--) {
		SWAP(l, h);
		l++;
		h++;
	}
	s = MIN(d-c, n-1-d);
	for(l = b, h = n-s; s; s--) {
		SWAP(l, h);
		l++;
		h++;
	}
	tsort(sa, next_ranks, b-a);
	tsort(sa + n-(d-c), next_ranks,  d-c);
}
