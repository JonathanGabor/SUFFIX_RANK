#include "utils.h"
#include "algorithm.h"
#include <stdio.h>
#include <stdlib.h>

void create_pairs(char * ranks_dir, char * output_dir, int chunk_id, int total_chunks,
                  long working_chunk_size, int40 * current_rank, InverseRecord * inverse_buffer) {
	int i;

	char ranks_file_name[MAX_PATH_LENGTH];
	char output_file_name[MAX_PATH_LENGTH];

	FILE * ranksFP = NULL;
	FILE * outputFP = NULL;

	snprintf(ranks_file_name, sizeof ranks_file_name, "%s/ranks_%d", ranks_dir, chunk_id);

	OpenBinaryFileRead(&ranksFP, ranks_file_name);
	int num_elements = fread(current_rank, sizeof(int40), working_chunk_size, ranksFP);
	fclose(ranksFP);

	long *bucket_starts = (long *) Calloc(total_chunks * sizeof(long));

	for (i=0; i < num_elements; i++) {
		bucket_starts[(-i40_load(&current_rank[i])) / working_chunk_size]++;
	}

	long sum = 0;
	long temp;
	for (i=0; i < total_chunks; i++) {
		temp = bucket_starts[i];
		bucket_starts[i] = sum;
		sum += temp;
	}

	for (i=0; i < num_elements; i++) {
		long val = -i40_load(&current_rank[i]);
		long idx = (long)i + (long)chunk_id * working_chunk_size;
		InverseRecord *dst = &inverse_buffer[bucket_starts[val / working_chunk_size]++];
		i40_store(&dst->index, idx);
		i40_store(&dst->value, val);
	}

	snprintf(output_file_name, sizeof output_file_name, "%s/pairs_0", output_dir);
	OpenBinaryFileAppend(&outputFP, output_file_name);
	fwrite(inverse_buffer, sizeof(InverseRecord), bucket_starts[0], outputFP);
	fclose(outputFP);

	for (i=1; i < total_chunks; i++) {
		snprintf(output_file_name, sizeof output_file_name, "%s/pairs_%d", output_dir, i);
		OpenBinaryFileAppend(&outputFP, output_file_name);
		fwrite(inverse_buffer+bucket_starts[i-1], sizeof(InverseRecord), bucket_starts[i]-bucket_starts[i-1], outputFP);
		fclose(outputFP);
	}

	free(bucket_starts);
}

int main(int argc, char ** args) {
	char * ranks_dir;
	char * output_dir;
	int total_chunks, chunk_id;

	if (argc < 5) {
		puts ("Run ./create_pairs <rank_dir> <output_dir> <total_chunks> <working_chunk_size>");
		return FAILURE;
	}

	ranks_dir = args[1];
	output_dir = args[2];
	total_chunks = atoi(args[3]);
	long working_chunk_size = parse_chunk_size(args[4]);

	int40 * current_rank = (int40 *) Calloc((size_t)working_chunk_size * sizeof(int40));
	InverseRecord * inverse_buffer = (InverseRecord *) Calloc((size_t)working_chunk_size * sizeof(InverseRecord));

	printf("total chunks: %d\n", total_chunks);
	for (chunk_id = 0; chunk_id < total_chunks; chunk_id++) {
		create_pairs(ranks_dir, output_dir, chunk_id, total_chunks, working_chunk_size,
		             current_rank, inverse_buffer);
	}

	free(current_rank);
	free(inverse_buffer);
	return SUCCESS;
}
