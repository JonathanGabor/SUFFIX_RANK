#ifndef ALGORITHM_H
#define ALGORITHM_H

typedef struct run_triple {
	long currentRank;
	long nextRank;
	int count;
} RunRecord;

typedef struct rank_pos_pair {
	long index;
	long value;
} InverseRecord;

int count_characters (char *input_directory, char * temp_directory);
int generate_local_runs_parallel (char * rank_dir, char * runs_dir, int total_chunks, int chunk_id, int h);
int generate_local_runs (char * input_dir, char * temp_dir, int total_chunks, int chunk_id, int h, long * current_ranks_buffer, long * next_ranks_buffer, int * sa_buffer, RunRecord * runs_buffer);
// int generate_local_runs_parallel (char * rank_dir, char * runs_dir, int total_chunks, int chunk_id, int h, int factor);

int resolve_global_ranks (char *temp_dir );
int update_local_ranks_parallel (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, int h);
int update_local_ranks (char * rank_dir, char * temp_dir, int total_chunks, int chunk_id, int h, long * buffer_current, long * buffer_next, int * sa_buffer, long * updated_ranks);
//int update_ranks_collect_new (char * ranks_dir, char * temp_dir, int h);

#endif
