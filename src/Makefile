CC = gcc
CFLAGS = -O3 -Wall
CFLAGS += -D_FILE_OFFSET_BITS=64
CFLAGS += -D_LARGEFILE_SOURCE
CFLAGS += -fno-exceptions
CFLAGS += -funroll-loops

default: all

all: input_to_binary init refine update merge create_pairs invert

input_to_binary: input_to_binary.c utils.c
	$(CC) $(CFLAGS) input_to_binary.c utils.c -o input_to_binary

init: init.c utils.c
	$(CC) $(CFLAGS) init.c utils.c -o init

refine: refine.c utils.c
	$(CC) $(CFLAGS) refine.c utils.c -o refine

merge: merge.c utils.c
	$(CC) $(CFLAGS) merge.c utils.c -o merge

update: update.c utils.c
	$(CC) $(CFLAGS) update.c utils.c -o update

create_pairs: create_pairs.c utils.c
	$(CC) $(CFLAGS) create_pairs.c utils.c -o create_pairs

invert: invert.c utils.c
	$(CC) $(CFLAGS) invert.c utils.c -o invert

clean:
	rm -f input_to_binary init refine merge update create_pairs invert
