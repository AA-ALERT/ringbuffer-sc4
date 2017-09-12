SOURCE_ROOT ?= $(HOME)
BIN_DIR ?= $(SOURCE_ROOT)/install/bin

VERSION := $(shell git rev-parse HEAD )

TESTKEY=aaaa

# http://psrdada.sourceforge.net/
PSRDADA  ?= $(SOURCE_ROOT)/src/psrdada

INCLUDES := -I"$(PSRDADA)/src/"
DADA_DEPS := $(PSRDADA)/src/dada_hdu.o $(PSRDADA)/src/ipcbuf.o $(PSRDADA)/src/ipcio.o $(PSRDADA)/src/ipcutil.o $(PSRDADA)/src/ascii_header.o $(PSRDADA)/src/multilog.o $(PSRDADA)/src/tmutil.o $(PSRDADA)/src/fileread.o $(PSRDADA)/src/filesize.o

CFLAGS := -Wall
ifneq ($(debug), 1)
	CFLAGS += -O3 -g0 -march=native -fstrict-aliasing
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
	$(CC) -o bin/fill_ringbuffer src/fill_ringbuffer.c -DVERSION='"$(VERSION)"' $(DADA_DEPS) -I"$(PSRDADA)/src" $(CFLAGS)

.PHONY: clean test

clean:
	-@rm bin/*

send: src/send.c
	mkdir -p bin
	gcc -o src/send src/send.c

fake: src/fake.c
	mkdir -p bin
	$(CC) -o bin/fill_fake src/fake.c -DVERSION='"$(VERSION)"' $(DADA_DEPS) -I"$(PSRDADA)/src" $(CFLAGS)

fill_30: src/send
	# delete old ringbuffer
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)

	# start a new ringbuffer with NTABS x NCHANNELS x 25088 bytes = 460800000
	$(BIN_DIR)/dada_db -p -k $(TESTKEY) -n 3 -b 462422016

	# no scrubber

	# start spewing packages
	taskset 1 src/send -c 3 -m 0 -s 0 -p 7469 &

	# test the fill_ringbuffer program
	taskset 2 bin/fill_ringbuffer -k $(TESTKEY) -h test/header -c 3 -m 0 -s 20 -d 10000 -p 7469 -b 25088 -l test/log

	# clean up
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)
	-killall -u `whoami` src/send

fill_40: src/send
	# delete old ringbuffer
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)

	# start a new ringbuffer with NTABS x NCHANNELS x 25088 bytes = 460800000
	$(BIN_DIR)/dada_db -p -k $(TESTKEY) -n 3 -b 462422016

	# no scrubber

	# start spewing packages
	taskset 1 src/send -c 4 -m 0 -s 0 -p 7469 &

	# test the fill_ringbuffer program
	taskset 2 bin/fill_ringbuffer -k $(TESTKEY) -h test/header -c 4 -m 0 -s 20 -d 10000 -p 7469 -b 25088 -l test/log

	# clean up
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)
	-killall -u `whoami` src/send

fill_41: src/send
	# delete old ringbuffer
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)

	# start a new ringbuffer with NTABS x NCHANNELS x IQUV x 25000 bytes
	$(BIN_DIR)/dada_db -p -k $(TESTKEY) -n 8 -b 1843200000

	# no scrubber

	# start spewing packages
	taskset 1 src/send -c 4 -m 1 -s 0 -p 7469 &

	# test the fill_ringbuffer program
	taskset 2 bin/fill_ringbuffer -k $(TESTKEY) -h test/header -c 4 -m 1 -s 20 -d 10000 -p 7469 -b 25088 -l test/log

	# clean up
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)
	-killall -u `whoami` src/send

test: test40 test41 test42

test40: src/send
	# testing science case 4 mode 0 (I+TAB)
	# delete old ringbuffer
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)

	# start a new ringbuffer with NTABS x NCHANNELS x 2500 bytes = 460800000
	$(BIN_DIR)/dada_db -p -k $(TESTKEY) -n 3 -b 462422016

	# start a scrubber to empty the buffer (this fakes the pipeline reading data from the buffer)
	$(BIN_DIR)/dada_dbscrubber -v -k $(TESTKEY) &

	# start spewing packages
	taskset 1 src/send -c 4 -m 0 -s 0 -p 7469 &

	# test the fill_ringbuffer program
	taskset 2 bin/fill_ringbuffer -k $(TESTKEY) -h test/header -c 4 -m 0 -s 8000 -d 65 -p 7469 -b 25088 -l test/log

	# clean up
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)
	-killall -u `whoami` src/send

test41: src/send
	# testing science case 4 mode 1 (IQUV)
	# delete old ringbuffer
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)

	# start a new ringbuffer with
	# [tab=12]x[time=25000]x[the 4 components IQUV]x[1536 channels] = 1843200000
	$(BIN_DIR)/dada_db -p -k $(TESTKEY) -n 5 -b 1843200000

	# start a scrubber to empty the buffer (this fakes the pipeline reading data from the buffer)
	$(BIN_DIR)/dada_dbscrubber -v -k $(TESTKEY) &

	# start spewing packages
	taskset 1 src/send -c 4 -m 1 -s 0 -p 7469 &

	# test the fill_ringbuffer program
	taskset 2 bin/fill_ringbuffer -k $(TESTKEY) -h test/header -c 4 -m 1 -s 8000 -d 65 -p 7469 -b 25088 -l test/log

	# clean up
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)
	-killall -u `whoami` src/send

test42: src/send
	# testing science case 4 mode 2 (I+IAB)
	# delete old ringbuffer
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)

	# start a new ringbuffer with NTABS x NCHANNELS x 25000 bytes = 3840000
	$(BIN_DIR)/dada_db -p -k $(TESTKEY) -n 3 -b 38400000

	# start a scrubber to empty the buffer (this fakes the pipeline reading data from the buffer)
	$(BIN_DIR)/dada_dbscrubber -v -k $(TESTKEY) &

	# start spewing packages
	taskset 1 src/send -c 4 -m 2 -s 0 -p 7469 &

	# test the fill_ringbuffer program
	taskset 2 bin/fill_ringbuffer -k $(TESTKEY) -h test/header -c 4 -m 2 -s 8000 -d 65 -p 7469 -b 25000 -l test/log

	# clean up
	-$(BIN_DIR)/dada_db -d -k $(TESTKEY)
	-killall -u `whoami` src/send
