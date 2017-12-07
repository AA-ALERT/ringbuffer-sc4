/**
 * Program to read from the port and write to the ringbuffer 
 * Author: Jisk Attema, based on code by Roy Smits
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

#include "dada_hdu.h"
#include "futils.h"

#define NCHANNELS 1536

#define UMSBATCH (1000000.0)       // sleep time in microseconds between sending batches

FILE *runlog = NULL;

char *science_modes[] = {"I+TAB", "IQUV+TAB", "I+IAB", "IQUV+IAB"};

// #define LOG(...) {fprintf(logio, __VA_ARGS__)}; 
#define LOG(...) {fprintf(stdout, __VA_ARGS__); fprintf(runlog, __VA_ARGS__); fflush(stdout);}

void generatePulsar(
    // pulsar properties
    const float DM,
    const unsigned int period,      // period of the pulse in number of samples
    const unsigned int width,       // width of the pules in number of samples
    const int random,               // if true, randomize peak height and noise level

    // signal properties
    const float minFreq,
    const float maxFreq,
    const float bandwidth,
    const int ntabs,                 // size of the tab dimension of the data array
    const int nchannels,             // size of the channel dimension of data array
    const int nsamples,              // the number of samples per channel in a batch (ie. time dimension)
    const unsigned int paddedSize,   // actual size of the time dimension of data array, ie nsamples + padding
    const unsigned int batch,        // batch number
    unsigned char *data              // Byte array of size [ntabs][nchannels][paddedSize]
) {

  unsigned int channel;
  unsigned int sample;
  unsigned int internalSample;

  // Set random number generator seed
  srand(batch + 42);
 
  // Set background signal, either constant or random (ie. approximation of white noise
  int tab;
  for (tab = 0; tab < ntabs; tab++) {
    for (channel = 0; channel < nchannels; channel++) {
      for (sample = 0; sample < nsamples; sample++) {
        data[tab * nchannels * paddedSize + channel * paddedSize + sample] = random ? rand() % 25 : 8;
      }
    }
  }

  // Generate the pulsar
  float inverseHighFreq = 1.0f / (maxFreq * maxFreq);
  float kDM = 4148.808f * DM;

  for (channel = 0; channel < nchannels; channel++) {
    float inverseFreq = 1.0f / ((minFreq + channel * bandwidth) * (minFreq + channel * bandwidth));
    float delta = kDM * (inverseFreq - inverseHighFreq);

    // Find the first pulse in this batch: first N batches contain floor(N x samples per batch / pulse period) pulses
    int firstPulse = ((batch * nsamples) / period) + 1;
    int lastPulse = ((batch+1) * nsamples / period);
  
    // Loop over the pulses in this batch
    int pulse;
    for (pulse = firstPulse; pulse <= lastPulse; pulse++) {
      // fill in a pulse of specified width in tab 3
      unsigned int i;
      for (i = 0; i < width; i++) {
        sample = pulse * period + delta; // TODO: check math for delta

        if (sample + i >= (batch + 1)*nsamples) {
          // dont write outside this batch
          // NOTE: the part of the pulse that overlaps with the next batch is dropped, and lost completely.
          //       It is *not* inserted at the beginning of the next batch
          break;
        } else {
          // Write a pulse in tab 3
          internalSample = (sample + i) % nsamples;
          data[3 * nchannels * paddedSize + channel * paddedSize + internalSample] = random? rand() % 128 : 42;
        }
      }
    }
  }
}

/**
 * Print commandline optinos
 */
void printOptions() {
  printf("usage: fill_fake -h <header file> -k <hexadecimal key> -c <science case> -d <duration (s)> -p <padded size> -l <logfile>\n");
  printf("e.g. fill_fake -h \"header1.txt\" -k dada -c 4 -m 0 -d 60 -l log.txt\n");
  return;
}

/**
 * Parse commandline
 */
void parseOptions(int argc, char*argv[], char **header, char **key, int *science_case, int *science_mode, int *duration, int *padded_size, char **logfile) {
  int c;

  int seth=0, setk=0, setd=0, setb=0, setl=0, setc=0, setm=0;
  while((c=getopt(argc,argv,"h:k:d:p:b:l:c:m:"))!=-1) {
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

      // -b padded_size (bytes)
      case('b'):
        *padded_size = atoi(optarg);
        setb=1;
        break;

      // -l log file
      case('l'):
        *logfile = strdup(optarg);
        setl=1;
        break;

      // -c case
      case('c'):
        *science_case = atoi(optarg);
        setc=1;
        if (*science_case < 3 || *science_case > 4) {
          printOptions();
          exit(0);
        }
        break;

      // -m mode
      case('m'):
        *science_mode = atoi(optarg);
        setm=1;
        if (*science_mode < 0 || *science_mode > 4) {
          printOptions();
          exit(0);
        }
        break;

      default:
        printOptions();
        exit(0);
    }
  }

  // All arguments are required
  if (!seth || !setk || !setd || !setl || !setb || !setc || !setm) {
    if (!seth) fprintf(stderr, "Header file not set\n");
    if (!setk) fprintf(stderr, "DADA key not set\n");
    if (!setd) fprintf(stderr, "Duration not set\n");
    if (!setl) fprintf(stderr, "Logfile not set\n");
    if (!setc) fprintf(stderr, "Science case not set\n");
    if (!setm) fprintf(stderr, "Science mode not set\n");
    printOptions();
    exit(EXIT_FAILURE);
  }
}

/**
 * Open a connection to the ringbuffer
 * The metadata (header block) is read from file
 * The required_size field is updated with the actual buffer size
 *
 * @param {dada_hdu_t **} hdu pointer to a pointer of HDU
 * @param {char *} header String containing the header file names to read
 * @param {char *} key String containing the shared memeory keys as hexadecimal numbers
 * @param {size_t *} required_size Minimum required ring buffer page size
 * @returns {hdu *} A connected HDU
 */
dada_hdu_t *init_ringbuffer(char *header, char *key, size_t *required_size) {
  char *buf;
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
  if (fileread (header, buf, bufsz) < 0) { 
    LOG("ERROR. Cannot read header from %s\n", header);
    exit(EXIT_FAILURE);
  }

  // tell the ringbuffer the header is filled
  if (ipcbuf_mark_filled (hdu->header_block, bufsz) < 0) {
    LOG("ERROR. Could not mark filled header block\n");
    exit(EXIT_FAILURE);
  }
  LOG("psrdada HEADER: %s\n", header);

  dada_hdu_db_addresses(hdu, &nbufs, &bufsz);

  if (bufsz < *required_size) {
    LOG("ERROR. ring buffer data block too small, should be at least %li\n", required_size);
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
  parseOptions(argc, argv, &header, &key, &science_case, &science_mode, &duration, &padded_size, &logfile);

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

  // calculate run length
  LOG("fill_fake version: " VERSION "\n");
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
    ntimes = 25000;
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

  // ring buffer
  LOG("Connecting to ringbuffer\n");
  hdu = init_ringbuffer(header, key, &required_size);
  free(header);
  free(key);

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

    /*
    // copy to ringbuffer
    if ((science_mode & 1) == 0) {
      // ring buffer contains matrix:
      // [tab][channel][time]
      // memcpy(
      //   &buf[((packet->tab_index * NCHANNELS) + curr_channel) * padded_size + packet->sequence_number * expected_payload],
      //   packet->record, expected_payload);
      generatePulsar(
        // pulsar properties
        2,  // DM
        50, // period of the pulse in number of samples
        5,  // width of the pules in number of samples
        0,  // if true, randomize peak height and noise level
 
        // signal properties
        52.5,          // minFreq
        7732.5,        // maxFreq
        5,             // bandwidth
        ntabs,         // size of the tab dimension of the data array
        NCHANNELS,     // size of the channel dimension of data array
        ntimes,        // the number of samples per channel in a batch (ie. time dimension)
        padded_size,   // actual size of the time dimension of data array, ie nsamples + padding
        batch,         // batch number
        (unsigned char *) buf            // Byte array of size [ntabs][nchannels][paddedSize]
      );
    } else {
      // stokes IQUV
    }
    */
  }

  // clean up and exit
exit:
  fflush(stdout);
  fflush(stderr);
  fflush(runlog);

  fclose(runlog);
  exit(EXIT_SUCCESS);
}

