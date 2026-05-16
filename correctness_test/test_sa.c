#include "utils.h"
#include "algorithm.h"

char * input_directory;
FILE* currFP=NULL;
FILE* prevFP=NULL;

int compare_strings_from_files(){
	uint16_t prev_char;
	uint16_t curr_char;

	while ((fread(&prev_char, sizeof (uint16_t), 1, prevFP) == 1) &&
		(fread(&curr_char, sizeof (uint16_t), 1, currFP) == 1)) {
		// printf("curr char:%d, prev char:%d\n", curr_char, prev_char);

		if (curr_char == 0 && prev_char == 0){
			fclose (currFP);
			fclose (prevFP);
			return 0;
		}
		if (prev_char < curr_char){
			fclose (currFP);
			fclose (prevFP);
			return -1;
		}
		if(prev_char > curr_char){
			fclose (currFP);
			fclose (prevFP);
			return 1;
		}
	}
	fclose (currFP);
	fclose (prevFP);
	return 0;
}

int print_strings(){
	uint16_t prev_char;
	uint16_t curr_char;

	while ((fread(&prev_char, sizeof (uint16_t), 1, prevFP) == 1) &&
		(fread(&curr_char, sizeof (uint16_t), 1, currFP) == 1)) {
		// printf ("%c %c\n", (char)prev_char, (char)curr_char);
		if (prev_char < curr_char){
			fclose (currFP);
			fclose (prevFP);
			return -1;
		}
		if(prev_char > curr_char){
			fclose (currFP);
			fclose (prevFP);
			return 1;
		}
	}
	fclose (currFP);
	fclose (prevFP);
	return 0;
}

int move_to_positions (long curr_pos, long prev_pos) {
	char curr_filename[MAX_PATH_LENGTH];
	char prev_filename[MAX_PATH_LENGTH];

	sprintf(curr_filename, "%s/binary_input", input_directory);
	sprintf(prev_filename, "%s/binary_input", input_directory);

	OpenBinaryFileRead (&currFP, curr_filename);
	OpenBinaryFileRead (&prevFP, prev_filename);

	// printf("Looking for char at position: prev:%d, curr:%d\n",
			// (prev_pos + WORKING_CHUNK_SIZE * prev_fid),
			// (curr_pos + WORKING_CHUNK_SIZE * curr_fid));
	fseek(currFP, (curr_pos)*sizeof(uint16_t), SEEK_SET);
	fseek(prevFP, (prev_pos)*sizeof(uint16_t), SEEK_SET);

	return SUCCESS;
}



int main (int argc, char **argv){
	char * sa_filename;
	char line [MAX_LINE];
    	long count = 0;
	int started = 0;
	if (argc<3) {
		puts ("Run ./test_order <input_directory> <sa_filename>");
		return FAILURE;
	}

	input_directory = argv[1];
	sa_filename = argv[2];

	FILE* saFP = NULL;
	OpenFileRead(&saFP, sa_filename);

	long curr_pos, prev_pos;

	// printf("started\n");

	while (fread(&curr_pos, sizeof(long), 1, saFP)) {
		// printf("in\n");
        count++;
	 	if (started) {
	 		move_to_positions(curr_pos, prev_pos);
	 		int res = compare_strings_from_files();
	 		if (res > 0) {
	 			printf ("TEST FAILED: suffix at post %ld is bigger than suffix at post %ld\n",
	 				prev_pos, curr_pos);
	 			move_to_positions(curr_pos, prev_pos);
	 			print_strings ();
	 			exit(1);
	 		}
	 	}
		else started = 1;
	 	prev_pos = curr_pos;
        if (count % 10000000 == 0)
            printf ("Processed %ld positions. Correct so far\n", count);
	}
	return 0;
}
