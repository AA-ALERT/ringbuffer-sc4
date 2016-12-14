SOURCE_ROOT ?= $(HOME)
BIN_DIR := $(SOURCE_ROOT)/install/bin

# http://psrdada.sourceforge.net/
PSRDADA  := $(SOURCE_ROOT)/src/psrdada

INCLUDES := -I"$(PSRDADA)/src/"
DADA_DEPS := $(PSRDADA)/src/dada_hdu.o $(PSRDADA)/src/ipcbuf.o $(PSRDADA)/src/ipcio.o $(PSRDADA)/src/ipcutil.o $(PSRDADA)/src/ascii_header.o $(PSRDADA)/src/multilog.o $(PSRDADA)/src/tmutil.o $(PSRDADA)/src/fileread.o $(PSRDADA)/src/filesize.o

CFLAGS := -Wall
ifneq ($(debug), 1)
	CFLAGS += -O3 -g0 -march=native 
else
	CFLAGS += -O0 -g3
endif
ifeq ($(openmp), 1)
	CFLAGS += -fopenmp
endif

LDFLAGS := -lm

CC := gcc

all: src/fill_ringbuffer.c
	mkdir -p bin
	$(CC) -o bin/fill_ringbuffer src/fill_ringbuffer.c $(DADA_DEPS) -I"$(PSRDADA)/src" $(CFLAGS)

.PHONY: clean test

clean:
	-@rm bin/*

src/send: src/send.c
	gcc -o src/send src/send.c

test: src/send
	# delete old ringbuffer
	-sudo $(BIN_DIR)/dada_db -d
	# start a new ringbuffer on key 'dada' with 512 packets per buffer x 4840 byes = 1239040
	sudo $(BIN_DIR)/dada_db -p -k dada -n 4 -l -b 1239040
	# start a scrubber to empty the buffer (this fakes the pipeline reading data from the buffer)
	$(BIN_DIR)/dada_dbscrubber -v -k dada &
	# start spewing packages
	taskset 4 src/send &
	# test the fill_ringbuffer program
	taskset 2 bin/fill_ringbuffer -k dada -h test/header -s 100000000 -d 1000 -p 4000 -l test/log
	# clean up
	-sudo $(BIN_DIR)/dada_db -d

