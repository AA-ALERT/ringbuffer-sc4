/**
 * Program to read from the port and write to the ringbuffer 
 * Author: Jisk Attema, based on code by Roy Smits
 *
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <getopt.h>
#include <netinet/in.h>
#include <byteswap.h>

#include "dada_hdu.h"
#include "futils.h"

#define PACKETSIZE 4840           // Size of the packet, including the header in bytes
#define RECORDSIZE 4800           // Size of the record = packet - header in bytes
#define PACKHEADER 40             // Size of the packet header = PACKETSIZE-RECORDSIZE in bytes

#define MAX_STREAMS 16
#define MAX_BANDS   16
#define SOCKBUFSIZE 33554432      // Buffer size of socket
#define RECSPERSECOND 15625       // Number of packets / records per second = 781250 * 24 * 4 / 4800
#define NBLOCKS 50                // Number of data blocks in a packet / record, used to check the packet header

/*
 * Packet header values are stored big endian
 * Bytes length    description
 * 01-02  2
 * 02-03  2        band
 * 04-05  2        nchannel
 * 06-07  2        nblocks
 * 08-15  8        timestamp
 * 16-40 24        flags
 *  <4800 bytes of RECORDS> 
 */
typedef struct {
  unsigned short unused_A;
  unsigned short band;
  unsigned short channel;
  unsigned short nblocks;
  unsigned long  timestamp;
  unsigned long flags[3];
  char record[4800];
} packet_t;

/**
 * Data comes in at 18.75 MHz in packages of 4800 bytes:
 * 15833 blocks of 4800 bytes corresponds to 1 second of 19-MHz data
 * 15625 blocks of 4800 bytes corresponds to 1 second of 18.75-MHz data
 */

// #define LOG(...) {fprintf(logio, __VA_ARGS__)}; 
#define LOG(...) {fprintf(stdout, __VA_ARGS__); fflush(stdout); fflush(stderr);}


/**
 * Print commandline optinos
 */
void printOptions()
{
  printf("usage: fill_ringbuffer -h <\"header files\"> -k <\"list of hexadecimal keys\"> -s <starttime (packets after 1970)> -d <duration (s)> -p <port> -l <logfile>\n");
  printf("e.g. fill_ringbuffer -h \"header1.txt header2.txt header3.txt header4.txt header5.txt header6.txt header7.txt header8.txt\" -k \"10 20 30 40 50 60 70 80\" -s 11565158400000 -d 3600 -p 4000 -l log.txt\n");
  return;
}

/**
 * Parse commandline
 */
void parseOptions(int argc, char*argv[], char **headers, char **keys, unsigned long *starttime, int *duration, int *port, char **logfile) {
  int c;

  int seth=0, setk=0, sets=0, setd=0, setp=0, setl=0;
  while((c=getopt(argc,argv,"h:k:s:d:p:l:"))!=-1)
    switch(c) {
      // -h <heaer_files>
      case('h'):
        *headers = strdup(optarg);
        seth=1;
        break;

      // -k <list of hexadecimal keys
      case('k'):
        *keys = strdup(optarg);
        setk=1;
        break;

      // -s starttime (packets after 1970)
      case('s'):
        *starttime=atol(optarg);
        sets=1; 
        break;

      // -d duration in seconds
      case('d'):
        *duration=atoi(optarg);
        setd=1;
        break;

      // -p port number
      case('p'):
        *port=atoi(optarg);
        setp=1;
        break;

      // -l log file
      case('l'):
        *logfile = strdup(optarg);
        setl=1;
        break;
      default: printOptions(); exit(0);
    }

  // All arguments are required
  if (!seth || !setk || !sets || !setd || !setp || !setl) {
    printOptions();
    exit(EXIT_FAILURE);
  }
}


/**
 * Open a socket to read from a network port
 *
 * @param {int} port Network port to connect to
 * @param {int *} sock Socket identifier as returned by socket()
 * @param {struct sockaddr_in *} sa socket address
 */
void init_network(int port, int *sock, struct sockaddr_in *sa) {
  *sock = socket(AF_INET, SOCK_DGRAM, 0);

  // set socket buffer size
  int sockbufsize = SOCKBUFSIZE;
  setsockopt(*sock, SOL_SOCKET, SO_RCVBUF, &sockbufsize, sizeof(sockbufsize));

  // set socket address
  memset(sa, 0, sizeof(*sa));
  sa->sin_family = AF_UNSPEC;
  sa->sin_addr.s_addr = htonl(INADDR_ANY);
  sa->sin_port = htons(port);

  // bind
  if (bind(*sock, (struct sockaddr *) sa, sizeof (sa)) == -1) {
    perror("bind failed");
    close(*sock);
    exit(EXIT_FAILURE);
  }
}

/**
 * Open a connection to the ringbuffer for each stream
 * The metadata (header block) is read from file
 * @param {dada_hdu_t **} hdu pointer to an array of HDU pointers to connect
 * @param {char *} headers String containing the header file names to read
 * @param {char *} keys String containing the shared memeory keys as hexadecimal numbers
 * @returns {int} The number of opened HDU streams
 */
int init_ringbuffer(dada_hdu_t **hdu, char *headers, char *keys) {
  int nheaders;
  char *header;
  char *buf;
  uint64_t bufsz;

  int nkeys;
  char *key;
  key_t shmkey;

  multilog_t* log = NULL; // TODO: See if this is used in anyway by dada
  char writemode='W';     // needs to be a capital

  // create a hdu for each key in the keys string
  nkeys = 0;
  key = strtok(keys, " ");

  while(key) {
    // create hdu
    hdu[nkeys] = dada_hdu_create (log);

    // init key
    sscanf(key, "%x", &shmkey);
    dada_hdu_set_key(hdu[nkeys], shmkey);

    // connect
    if (dada_hdu_connect (hdu[nkeys]) < 0) {
      LOG("ERROR in dada_hdu_connect\n");
      exit(EXIT_FAILURE);
    }

    // Make data buffers readable
    if (dada_hdu_lock_write_spec (hdu[nkeys], writemode) < 0) {
      LOG("ERROR in dada_hdu_lock_write_spec\n");
      exit(EXIT_FAILURE);
    }

    // next key
    key = strtok(NULL, " ");
    nkeys++;
  }

  // read a hdu header for each header the headers string
  nheaders = 0;
  header = strtok(headers, " ");

  while(header && nheaders < nkeys) {
    // get dada buffer size
    bufsz = ipcbuf_get_bufsz (hdu[nheaders]->header_block);

    // get write address
    buf = ipcbuf_get_next_write (hdu[nheaders]->header_block);
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
    if (ipcbuf_mark_filled (hdu[nheaders]->header_block, bufsz) < 0) {
      LOG("ERROR. Could not mark filled header block\n");
      exit(EXIT_FAILURE);
    }

    // next header
    header = strtok(NULL, " ");
    nheaders++;
  }

  // check we read a header for each key
  if (nheaders != nkeys) {
    LOG("ERROR. Not enough headers for given keys: headers=%i keys=%i\n", nheaders, nkeys);
    exit(EXIT_FAILURE);
  }
  // check if we read all headers, ie. no header remaining
  if (header != NULL) {
    LOG("ERROR: Too many headers, need only %i\n", nkeys);
    exit(EXIT_FAILURE);
  }

  LOG("Initialized ring buffer using %d streams\n", nheaders);
  return nheaders;
}

int main(int argc, char** argv) {
  // network state
  int sock;                 // port number
  struct sockaddr_in sa;    // socket address
  ssize_t recread;
  socklen_t length;

  // ringbuffer state
  dada_hdu_t *hdu[MAX_STREAMS];
  int band_to_hdu[MAX_BANDS];

  // run parameters
  int duration;            // run time in seconds
  int nstreams;            // Number of HDU streams opened
  long nrecs;              // Number of records / packets to read
  float missing_pct;       // Number of records missed in percentage of expected number
  unsigned long missing;   // Number of records missed
  unsigned long starttime; // Starttime of the observation in units of 1/781250 seconds after 1970
  unsigned long endtime;   // Endtime of the observation in units of 1/781250 seconds after 1970

  // local vars
  char *headers;
  char *keys;
  char *logfile;
  FILE *log = NULL;
  const char mode = 'w';
  packet_t buffer;
  unsigned long timestamp;     // Current timestamp
  unsigned short band;         // Current band
  unsigned short stream;       // HDU stream
  // unsigned long previous_stamp[MAX_BANDS];   // last encountered timestamp per band
  unsigned long records_per_band[MAX_BANDS]; // number of records processed per band

  // parse commandline
  parseOptions(argc, argv, &headers, &keys, &starttime, &duration, &sock, &logfile);

  // set up logging
  if (logfile) {
    log = fopen(logfile, &mode);
    if (! log) {
      LOG("ERROR opening logfile: %s\n", logfile);
      exit(EXIT_FAILURE);
    }
    LOG("Logging to logfile: %s\n", logfile);
    free (logfile);
  }

  // calculate run length
  nrecs = RECSPERSECOND * duration;
  endtime = starttime + nrecs * NBLOCKS;
  LOG("Starttime = %ld\n", starttime);
  LOG("Endtime = %ld\n", endtime);
  LOG("Planning to read %i x %i = %ld records\n", duration, RECSPERSECOND, nrecs);

  // sockets
  LOG("Opening network port %i\n", sock);
  init_network(sock, &sock, &sa);

  // ring buffer
  LOG("Connecting to ringbuffer\n");
  nstreams = init_ringbuffer(hdu, headers, keys);
  free(headers);
  free(keys);

  // clear band mapping etc.
  for (band=0; band<MAX_BANDS; band++) {
    band_to_hdu[band] = -1;
    // previous_stamp[band] = 0;
    records_per_band[band] = 0;
  }

  // idle till starttime, but keep track of which bands there are
  timestamp = 0;
  while (timestamp < starttime) {
    recread = recvfrom(sock, (void *) &buffer, PACKETSIZE, 0, (struct sockaddr *) &sa, &length);
    if (recread != PACKETSIZE) {
      LOG("ERROR Could not read packet\n");
      goto exit;
    }
    band = bswap_16(buffer.band);
    timestamp = bswap_64(buffer.timestamp);

    // keep track of timestamps, 
    // mark this band as present
    // previous_stamp[band] = timestamp;
    band_to_hdu[band] = 1;
  }

  // map bands to HDU streams
  stream = 0;
  for (band=0; band < MAX_BANDS; band++) {
    if (band_to_hdu[band] == 1) {
      band_to_hdu[band] = stream;
      LOG("Mapping band %i to HDU unit %i\n", band, stream);
      stream++;
    }
  }
  if (stream != nstreams) {
    LOG("ERROR Number of bands does not match number of streams: %d and %d\n", stream, nstreams);
    goto exit;
  }

  // run till endtime
  while (timestamp < endtime) {
    // read packet
    if (recvfrom(sock, (void *) &buffer, PACKETSIZE, 0, (struct sockaddr *) &sa, &length) != PACKETSIZE) {
      LOG("ERROR Could not read packet\n");
      goto exit;
    }

    // parse header for band
    band = bswap_16(buffer.band);

    // check number of blocks
    if (bswap_16(buffer.nblocks) != NBLOCKS) {
      LOG("Warning: number of blocks in packet is not equal to %d\n", NBLOCKS);
      goto exit;
    }

    // match to hdu
    stream = band_to_hdu[band];
    if (stream == -1) {
      LOG("ERROR: unexpected band number %d\n", stream);
      goto exit;
    }

    // copy to ringbuffer
    if (ipcio_write(hdu[stream]->data_block, (char *) &buffer.record, RECORDSIZE) != RECORDSIZE){
      LOG("ERROR. Cannot write requested bytes to SHM\n");
      goto exit;
    }

    // do some extra processing on the packet
    timestamp = bswap_64(buffer.timestamp);
    records_per_band[band]++;

    // TODO: what more is required?
    // * do we need to signal packet loss immediately, or is it sufficient to give statistics at the end of the run?
    // previous_stamp[band] = timestamp;
  }

  // print diagnostics
  LOG("Packets read:\n");
  for (band=0; band < MAX_BANDS; band++) {
    if (band_to_hdu[band] != -1) {
      missing = nrecs - records_per_band[band];
      missing_pct = (100.0 * missing) / (1.0 * nrecs);
      LOG("Band %4i: %10ld out of %10ld, missing %10ld [%5.2f%%] records\n", band, records_per_band[band], nrecs, missing, missing_pct);
    }
  }

  // clean up and exit
exit:
  close(sock);
  fclose(log);
  exit(EXIT_SUCCESS);
}
