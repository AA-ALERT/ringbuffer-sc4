ROOT ?= $(HOME)

# http://psrdada.sourceforge.net/
PSRDADA  := $(ROOT)/src/psrdada

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

clean:
	-@rm bin/*
