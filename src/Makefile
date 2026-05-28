CC = gcc
CFLAGS = -O3 -Wall
CFLAGS += -D_FILE_OFFSET_BITS=64
CFLAGS += -D_LARGEFILE_SOURCE
CFLAGS += -fno-exceptions
CFLAGS += -funroll-loops

CXX = g++
CXXFLAGS = -O3 -Wall
CXXFLAGS += -D_FILE_OFFSET_BITS=64
CXXFLAGS += -D_LARGEFILE_SOURCE
CXXFLAGS += -funroll-loops

DSS_INC = -I/Users/jonathangabor/programming/libdivsufsort/build/include
DSS_LIB = /Users/jonathangabor/programming/libdivsufsort/build/lib/libdivsufsort.a

default: all

all: init refine update merge create_pairs invert verify

profile: CFLAGS += -g -fno-omit-frame-pointer
profile: clean all

init: init.cpp utils_c.o input_stream_c.o bitvector.hpp srank.hpp
	$(CXX) $(CXXFLAGS) $(DSS_INC) init.cpp utils_c.o input_stream_c.o -o init $(DSS_LIB)

refine: refine.cpp utils_c.o
	$(CXX) $(CXXFLAGS) refine.cpp utils_c.o -o refine

merge: merge.c utils.c
	$(CC) $(CFLAGS) merge.c utils.c -o merge

update: update.c utils.c
	$(CC) $(CFLAGS) update.c utils.c -o update

create_pairs: create_pairs.c utils.c
	$(CC) $(CFLAGS) create_pairs.c utils.c -o create_pairs

invert: invert.c utils.c
	$(CC) $(CFLAGS) invert.c utils.c -o invert

verify: verify.c utils.c input_stream.c
	$(CC) $(CFLAGS) verify.c utils.c input_stream.c -o verify

utils_c.o: utils.c utils.h
	$(CC) $(CFLAGS) -c utils.c -o utils_c.o

input_stream_c.o: input_stream.c input_stream.h utils.h
	$(CC) $(CFLAGS) -c input_stream.c -o input_stream_c.o

clean:
	rm -f init refine merge update create_pairs invert verify utils_c.o input_stream_c.o
