/**
 * fake data generation code; used for development and debugging
 * Connect to a ringbuffer, and mark pages as 'filled'
 *
 * This whole code can probably be replaced by some psrdada tools
 *
 */
// needed for GNU extension to recvfrom: recvmmsg, bswap
#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <getopt.h>
#include <netinet/in.h>
#include <byteswap.h>

#include "ascii_header.h"
#include "dada_hdu.h"
#include "futils.h"
#include "config.h"

#define NCHANNELS 1536

#define UMSBATCH (1000000.0)       // sleep time in microseconds between sending batches

FILE *runlog = NULL;

char *science_modes[] = {"I+TAB", "IQUV+TAB", "I+IAB", "IQUV+IAB"};

// #define LOG(...) {fprintf(logio, __VA_ARGS__)}; 
#define LOG(...) {fprintf(stdout, __VA_ARGS__); fprintf(runlog, __VA_ARGS__); fflush(stdout);}

/**
 * Print commandline optinos
 */
void printOptions() {
  printf("usage: fill_fake -h <header file> -k <hexadecimal key> -d <duration (s)> -l <logfile>\n");
  printf("e.g. fill_fake -h \"header1.txt\" -k dada -d 60 -l log.txt\n");
  return;
}

/**
 * Parse commandline
 */
void parseOptions(int argc, char*argv[], char **header, char **key, int *duration, char **logfile) {
  int c;

  int seth=0, setk=0, setd=0, setl=0;
  while((c=getopt(argc,argv,"h:k:d:l:"))!=-1) {
    switch(c) {
      // -h <heaer_file>
      case('h'):
        *header = strdup(optarg);
        seth=1;
        break;

      // -k <hexadecimal_key>
      case('k'):
        *key = strdup(optarg);
        setk=1;
        break;

      // -d duration in seconds
      case('d'):
        *duration=atoi(optarg);
        setd=1;
        break;

      // -l log file
      case('l'):
        *logfile = strdup(optarg);
        setl=1;
        break;

      default:
        printOptions();
        exit(0);
    }
  }

  // All arguments are required
  if (!seth || !setk || !setd || !setl) {
    if (!seth) fprintf(stderr, "Header file not set\n");
    if (!setk) fprintf(stderr, "DADA key not set\n");
    if (!setd) fprintf(stderr, "Duration not set\n");
    if (!setl) fprintf(stderr, "Logfile not set\n");
    printOptions();
    exit(EXIT_FAILURE);
  }
}

/**
 * Open a connection to the ringbuffer
 * The metadata (header block) is read from file
 * The required_size field is updated with the actual buffer size
 *
 * @param {char *} filename String containing the header file names to read
 * @param {char *} key String containing the shared memeory keys as hexadecimal numbers
 * @param {size_t *} required_size Minimum required ring buffer page size
 * @param {int *} science_case read from header, and value stored here
 * @param {int *} science_mode read from header, and value stored here
 * @param {int *} padded_size read from header, and value stored here
 * @returns {hdu *} A connected HDU
 */
dada_hdu_t *init_ringbuffer(char *filename, char *key, size_t *required_size, int *science_case, int *science_mode, int *padded_size) {
  char *buf;
  int incomplete_header = 0;
  uint64_t bufsz;
  uint64_t nbufs;
  dada_hdu_t *hdu;

  key_t shmkey;

  multilog_t* multilog = NULL; // TODO: See if this is used in anyway by dada
  char writemode='W';     // needs to be a capital

  // create hdu
  hdu = dada_hdu_create (multilog);

  // init key
  sscanf(key, "%x", &shmkey);
  dada_hdu_set_key(hdu, shmkey);
  LOG("psrdada SHMKEY: %s\n", key);

  // connect
  if (dada_hdu_connect (hdu) < 0) {
    LOG("ERROR in dada_hdu_connect\n");
    exit(EXIT_FAILURE);
  }

  // Make data buffers readable
  if (dada_hdu_lock_write_spec (hdu, writemode) < 0) {
    LOG("ERROR in dada_hdu_lock_write_spec\n");
    exit(EXIT_FAILURE);
  }

  // get dada buffer size
  bufsz = ipcbuf_get_bufsz (hdu->header_block);

  // get write address
  buf = ipcbuf_get_next_write (hdu->header_block);
  if (! buf) {
    LOG("ERROR. Get next header block error\n");
    exit(EXIT_FAILURE);
  }

  // read header from file
  if (fileread (filename, buf, bufsz) < 0) {
    LOG("ERROR. Cannot read header from %s\n", filename);
    exit(EXIT_FAILURE);
  }

  // parse relevant metadata for ourselves
  if (ascii_header_get(buf, "SCIENCE_CASE", "%i", science_case) == -1) {
    LOG("ERROR. SCIENCE_CASE not set in dada header\n");
    incomplete_header = 1;
  }
  if (ascii_header_get(buf, "SCIENCE_MODE", "%i", science_mode) == -1) {
    LOG("ERROR. SCIENCE_MODE not set in dada header\n");
    incomplete_header = 1;
  }
  if (ascii_header_get(buf, "PADDED_SIZE", "%i", padded_size) == -1) {
    LOG("ERROR. PADDED_SIZE not set in dada header\n");
    incomplete_header = 1;
  }
  if (incomplete_header) {
    exit(EXIT_FAILURE);
  }

  // tell the ringbuffer the header is filled
  if (ipcbuf_mark_filled (hdu->header_block, bufsz) < 0) {
    LOG("ERROR. Could not mark filled header block\n");
    exit(EXIT_FAILURE);
  }
  LOG("psrdada HEADER: %s\n", filename);

  dada_hdu_db_addresses(hdu, &nbufs, &bufsz);

  if (bufsz < *required_size) {
    LOG("ERROR. ring buffer data block too small, should be at least %lu\n", *required_size);
    exit(EXIT_FAILURE);
  }

  // set the required size to the actual size
  // this is needed when marking a page full.
  // If we need to use the actual buffer size to prevent the stream from closing (too small) or reading outside of the array bounds (too big)
  *required_size = bufsz;

  return hdu;
}

int main(int argc, char** argv) {
  // ringbuffer state
  dada_hdu_t *hdu;
  char *buf; // pointer to current buffer

  // run parameters
  int duration;            // run time in seconds
  int science_case;        // 3 or 4
  int science_mode;        // 0: I+TAB, 1: IQUV+TAB, 2: I+IAB, 3: IQUV+IAB
  int ntabs;
  int ntimes;
  int padded_size;

  // local vars
  char *header;
  char *key;
  char *logfile;
  const char mode = 'w';
  size_t required_size = 0;

  // parse commandline
  parseOptions(argc, argv, &header, &key, &duration, &logfile);

  // set up logging
  if (logfile) {
    runlog = fopen(logfile, &mode);
    if (! runlog) {
      LOG("ERROR opening logfile: %s\n", logfile);
      exit(EXIT_FAILURE);
    }
    LOG("Logging to logfile: %s\n", logfile);
    free (logfile);
  }
  LOG("fill_fake version: " VERSION "\n");

  // ring buffer
  LOG("Connecting to ringbuffer\n");
  hdu = init_ringbuffer(header, key, &required_size, &science_case, &science_mode, &padded_size);

  free(header); header = NULL;
  free(key); key = NULL;

  LOG("Science case = %i\n", science_case);
  LOG("Science mode = %i [ %s ]\n", science_mode, science_modes[science_mode]);
  LOG("Duration (batches) = %i\n", duration);

  if (science_case == 3) {
    ntimes = 12500;
    switch (science_mode) {
      case 0: ntabs = 12; break;
      case 1: ntabs = 12; break;
      case 2: ntabs = 1; break;
      case 3: ntabs = 1; break;
      default:
        LOG("Science mode not supported");
        break;
    }
  } else if (science_case == 4) {
    ntimes = 12500;
    switch (science_mode) {
      case 0: ntabs = 12; break;
      case 1: ntabs = 12; break;
      case 2: ntabs = 1; break;
      case 3: ntabs = 1; break;
      default:
        LOG("Science mode not supported");
        break;
    }
  } else {
    ntabs = 1;
    LOG("Science case not supported");
    goto exit;
  }

  // ============================================================
  // run till end time
  // ============================================================

  int batch;
  for (batch = 0; batch < duration; batch++) {
    // get a new buffer
    buf = ipcbuf_get_next_write ((ipcbuf_t *)hdu->data_block);

    if (batch == duration-1) {
      ipcbuf_enable_eod((ipcbuf_t *)hdu->data_block);
    }

    if (ipcbuf_mark_filled ((ipcbuf_t *) hdu->data_block, required_size) < 0) {
      LOG("ERROR: cannot mark buffer as filled\n");
      goto exit;
    }

    // slow down sending a bit
    usleep(UMSBATCH);
  }

  // clean up and exit
exit:
  fflush(stdout);
  fflush(stderr);
  fflush(runlog);

  fclose(runlog);
  exit(EXIT_SUCCESS);
}

