#include "utils.h"
#include "algorithm.h"
#include <stdio.h>

void invert(char * pairs_dir, char * output_dir, int chunk_id, long working_chunk_size,
            InverseRecord * pairs_buffer, long * suffixarray) {
	char pairs_file_name[MAX_PATH_LENGTH];
	char output_file_name[MAX_PATH_LENGTH];

	FILE * pairsFP;
	FILE * outputFP;

	snprintf(pairs_file_name, sizeof pairs_file_name, "%s/pairs_%d", pairs_dir, chunk_id);
	snprintf(output_file_name, sizeof output_file_name, "%s/suffixarray_%d", output_dir, chunk_id);

	OpenBinaryFileRead(&pairsFP, pairs_file_name);
	long total_records = fread(pairs_buffer, sizeof(InverseRecord), working_chunk_size, pairsFP);
	fclose(pairsFP);

	int i;
	for (i=0; i < total_records; i++) {
		suffixarray[i40_load(&pairs_buffer[i].value) % working_chunk_size] = i40_load(&pairs_buffer[i].index);
	}

	OpenBinaryFileWrite(&outputFP, output_file_name);
	Fwrite(suffixarray, sizeof(long), total_records, outputFP);
	fclose(outputFP);
}

int main(int argc, char ** args) {
	char * pairs_dir;
	char * output_dir;
	int total_chunks, chunk_id;

	if (argc < 5) {
		puts ("Run ./invert <pairs_dir> <output_dir> <total_chunks> <working_chunk_size>");
		return FAILURE;
	}

	pairs_dir = args[1];
	output_dir = args[2];
	total_chunks = atoi(args[3]);
	long working_chunk_size = parse_chunk_size(args[4]);

	InverseRecord * pairs_buffer = (InverseRecord *) Calloc((size_t)working_chunk_size * sizeof(InverseRecord));
	long * suffixarray = (long *) Calloc((size_t)working_chunk_size * sizeof(long));

	for (chunk_id=0; chunk_id<total_chunks; chunk_id++) {
		invert(pairs_dir, output_dir, chunk_id, working_chunk_size, pairs_buffer, suffixarray);
	}

	free(pairs_buffer);
	free(suffixarray);
	return SUCCESS;
}
