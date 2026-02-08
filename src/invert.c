#include "utils.h"
#include "algorithm.h"
#include <stdio.h>

void invert(char * pairs_dir, char * output_dir, int chunk_id) {
	char pairs_file_name[MAX_PATH_LENGTH];
	char output_file_name[MAX_PATH_LENGTH];

	FILE * pairsFP;
	FILE * outputFP;

	sprintf(pairs_file_name, "%s/pairs_%d", pairs_dir, chunk_id);
	sprintf(output_file_name, "%s/suffixarray_%d", output_dir, chunk_id);

	InverseRecord * pairs_buffer = (InverseRecord *) Calloc(WORKING_CHUNK_SIZE * sizeof(InverseRecord));

	OpenBinaryFileRead(&pairsFP, pairs_file_name);
	long total_records = fread(pairs_buffer, sizeof(InverseRecord), WORKING_CHUNK_SIZE, pairsFP);
	fclose(pairsFP);

	long * suffixarray = (long *) Calloc(WORKING_CHUNK_SIZE * sizeof(long));
	int i;
	for (i=0; i < total_records; i++) {
		suffixarray[pairs_buffer[i].value % WORKING_CHUNK_SIZE] = pairs_buffer[i].index;
	}
	free(pairs_buffer);

	OpenBinaryFileWrite(&outputFP, output_file_name);
	Fwrite(suffixarray, sizeof(long), total_records, outputFP);
	fclose(outputFP);

	free(suffixarray);
}

int main(int argc, char ** args) {
	char * pairs_dir;
	char * output_dir;
	int total_chunks, chunk_id;

	if (argc < 4) {
		puts ("Run ./invert <pairs)dir> <output_dir> <total_chunks>");
		return FAILURE;
	}

	pairs_dir = args[1];
	output_dir = args[2];
	total_chunks = atoi(args[3]);

	for (chunk_id=0; chunk_id<total_chunks; chunk_id++) {
		invert(pairs_dir, output_dir, chunk_id);
	}
	return SUCCESS;
}
