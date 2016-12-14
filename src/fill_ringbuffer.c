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

#define PACKETSIZE 4840           // Size of the packet, including the header in bytes
#define RECORDSIZE 4800           // Size of the record = packet - header in bytes
#define PACKHEADER 40             // Size of the packet header = PACKETSIZE-RECORDSIZE in bytes
#define MMSG_VLEN  256            // Batch message into single syscal using recvmmsg()

/* We currently use
 *  - one band per port, with 
 *  - one instance of fill_ringbuffer connected to
 *  - one HDU. 
 *  This allows for some extra optimizations
 *  To turn those of, define MULTIBAND, fi. with compiler flag -DMULTIBAND
 */
#define MAX_STREAMS 16
#define MAX_BANDS   16

#define SOCKBUFSIZE 67108864      // Buffer size of socket
#define RECSPERSECOND 15625       // Number of packets / records per second = 781250 * 24 * 4 / 4800
#define NBLOCKS 50                // Number of data blocks in a packet / record, used to check the packet header

FILE *runlog = NULL;

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
#define LOG(...) {fprintf(stdout, __VA_ARGS__); fprintf(runlog, __VA_ARGS__); fflush(stdout);}

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
 * @returns {int} socket file descriptor
 */
int init_network(int port) {
  int sock;
  struct addrinfo hints, *servinfo, *p;
  char service[256];

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET; // set to AF_INET to force IPv4
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE; // use my IP

  snprintf(service, 255, "%i", port);
  if (getaddrinfo(NULL, service, &hints, &servinfo) != 0) {
    perror(NULL);
    exit(EXIT_FAILURE);
  }

  for(p = servinfo; p != NULL; p = p->ai_next) {
    sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sock == -1) {
      perror(NULL);
      continue;
    }

    // set socket buffer size
    int sockbufsize = SOCKBUFSIZE;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &sockbufsize, (socklen_t)sizeof(int));

    if(bind(sock, p->ai_addr, p->ai_addrlen) == -1) {
      perror(NULL);
      close(sock);
      continue;
    }

    // set up, break the loop
    break;
  }

  if (p == NULL) {
    fprintf(stderr, "Cannot setup connection\n" );
    exit(EXIT_FAILURE);
  }

  free(servinfo);

  return sock;
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

  multilog_t* multilog = NULL; // TODO: See if this is used in anyway by dada
  char writemode='W';     // needs to be a capital

  // create a hdu for each key in the keys string
  nkeys = 0;
  key = strtok(keys, " ");

  LOG("Mapping between HDUs and keys\n");
  while(key) {
    // create hdu
    hdu[nkeys] = dada_hdu_create (multilog);

    // init key
    sscanf(key, "%x", &shmkey);
    dada_hdu_set_key(hdu[nkeys], shmkey);
    LOG("HDU %02i: key: %s\n", nkeys, key);

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

  LOG("Mapping between HDUs and headers\n");
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
    LOG("HDU %02i: header: %s\n", nheaders, header);

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
  int port;                 // port number
  int sockfd;               // socket file descriptor

  // ringbuffer state
  dada_hdu_t *hdu[MAX_STREAMS];
#ifdef MULTIBAND
  int band_to_hdu[MAX_BANDS];
#endif
  int bands_present[MAX_BANDS];

  // run parameters
  int duration;            // run time in seconds
  int nstreams;            // Number of HDU streams opened
  long npackets;           // Number of packets to read
  float missing_pct;       // Number of packets missed in percentage of expected number
  unsigned long missing;   // Number of packets missed
  unsigned long notime;    // Number of packets without valid timestamp
  unsigned long starttime; // Starttime (start block) of the observation in units of 1/781250 seconds after 1970
  unsigned long endtime;   // Endtime (end block) of the observation in units of 1/781250 seconds after 1970

  // local vars
  char *headers;
  char *keys;
  char *logfile;
  const char mode = 'w';

  packet_t packet_buffer[MMSG_VLEN];   // Buffer for batch requesting packets via recvmmsg
  unsigned int packet_idx;             // Current packet index in MMSG buffer
  struct iovec iov[MMSG_VLEN];         // IO vec structure for recvmmsg
  struct mmsghdr msgs[MMSG_VLEN];      // multimessage hearders for recvmmsg

  packet_t *packet;            // Pointer to current packet
  unsigned long timestamp;     // Current timestamp
  unsigned short band;         // Current band
  unsigned short stream;       // HDU stream
  unsigned long previous_stamp[MAX_BANDS];   // last encountered timestamp per band
  unsigned long packets_per_band[MAX_BANDS]; // number of records processed per band

  // parse commandline
  parseOptions(argc, argv, &headers, &keys, &starttime, &duration, &port, &logfile);

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
  npackets = RECSPERSECOND * duration;
  endtime = starttime + npackets * NBLOCKS;
  LOG("fill ringbuffer version: " VERSION "\n");
  LOG("Starttime = %ld\n", starttime);
  LOG("Endtime = %ld\n", endtime);
  LOG("Planning to read %i (s) x %i (rec/s) = %ld records\n", duration, RECSPERSECOND, npackets);

  // sockets
  LOG("Opening network port %i\n", port);
  sockfd = init_network(port);

  // multi message setup
  memset(msgs, 0, sizeof(msgs));
  for(packet_idx=0; packet_idx < MMSG_VLEN; packet_idx++) {
    iov[packet_idx].iov_base = (char *) &packet_buffer[packet_idx];
    iov[packet_idx].iov_len = PACKETSIZE;

    msgs[packet_idx].msg_hdr.msg_name    = NULL; // we don't need to know who sent the data
    msgs[packet_idx].msg_hdr.msg_iov     = &iov[packet_idx];
    msgs[packet_idx].msg_hdr.msg_iovlen  = 1;
    msgs[packet_idx].msg_hdr.msg_control = NULL; // we're not interested in OoB data
  }

  // ring buffer
  LOG("Connecting to ringbuffer\n");
  nstreams = init_ringbuffer(hdu, headers, keys);
  free(headers);
  free(keys);

  // clear band mapping etc.
  for (band=0; band<MAX_BANDS; band++) {
    bands_present[band] = 0;
    packets_per_band[band] = 0;
  }
  notime = 0;

  // start at the end of the packet buffer, so the main loop starts with a recvmmsg call
  packet_idx = MMSG_VLEN - 1;
  packet = &packet_buffer[packet_idx];


  // ============================================================
  // idle till starttime, but keep track of which bands there are
  // ============================================================
 
  timestamp = 0;
  packet_idx = MMSG_VLEN - 1;
  while (timestamp < starttime) {
    // go to next packet in the packet buffer
    packet_idx++;

    // did we reach the end of the packet buffer?
    if (packet_idx == MMSG_VLEN) {
      // read new packets from the network into the buffer
      if(recvmmsg(sockfd, msgs, MMSG_VLEN, 0, NULL) != MMSG_VLEN) {
        LOG("ERROR Could not read packets\n");
        goto exit;
      }
      // go to start of buffer
      packet_idx = 0;
    }
    packet = &packet_buffer[packet_idx];

    // mark this band as present
    band = bswap_16(packet->band);
    if (band >= MAX_BANDS) {
      LOG("ERROR. Band number higher than maximum value: %i >= %i\n", band, MAX_BANDS);
      LOG("ERROR. Increase MAX_BANDS and recompile.\n");
      exit(EXIT_FAILURE);
    }
    bands_present[band] = 1;

    // keep track of timestamps, 
    timestamp = bswap_64(packet->timestamp);
    previous_stamp[band] = timestamp;
  }

  // map bands to HDU streams
  stream = 0;
  for (band=0; band < MAX_BANDS; band++) {
    if (bands_present[band] == 1) {
#ifdef MULTIBAND
      band_to_hdu[band] = stream;
#endif
      LOG("Mapping band %i to HDU unit %i\n", band, stream);
      stream++;
    }
  }
  if (stream != nstreams) {
    LOG("ERROR Number of bands does not match number of streams: %d and %d\n", stream, nstreams);
    goto exit;
  }

#ifdef MULTIBAND
  // band to stream mapping happens in the main loop below
#else
  // always copy to first HDU stream in the ringbuffer;
  stream = 0;
#endif

  // process the first (already-read) package by moving the packet_idx one back
  // this to compensate for the packet_idx++ statement in the first pass of the mainloop
  packet_idx--;

  // ============================================================
  // run till endtime
  // ============================================================
 
  while (timestamp < endtime) {
    // go to next packet in the packet buffer
    packet_idx++;

    // did we reach the end of the packet buffer?
    if (packet_idx == MMSG_VLEN) {
      // read new packets from the network into the buffer
      if(recvmmsg(sockfd, msgs, MMSG_VLEN, 0, NULL) != MMSG_VLEN) {
        LOG("ERROR Could not read packets\n");
        goto exit;
      }
      // go to start of buffer
      packet_idx = 0;
    }
    packet = &packet_buffer[packet_idx];
 
    // check band number
    band = bswap_16(packet->band);
    if (bands_present[band] == 0) {
      LOG("ERROR: unexpected band number %d\n", band);
      goto exit;
    }

    // check number of blocks
    if (bswap_16(packet->nblocks) != NBLOCKS) {
      LOG("Warning: number of blocks in packet is not equal to %d\n", NBLOCKS);
      goto exit;
    }

#ifdef MULTIBAND
    // match band to stream
    stream = band_to_hdu[band];
#endif

    // check timestamps
    timestamp = bswap_64(packet->timestamp);
    if (timestamp == 0) {
      // assume it is the expected packet, and set timestamp ourselves
      timestamp = previous_stamp[band] + NBLOCKS;
      notime++;
    }

    // repeatedly add this packet to make up for missed / dropped packets
    while (previous_stamp[band] < timestamp) {
      // copy to ringbuffer
      if (ipcio_write(hdu[stream]->data_block, (char *) packet->record, RECORDSIZE)
          != RECORDSIZE) {
        LOG("ERROR. Cannot write requested bytes to SHM\n");
        goto exit;
      }
      previous_stamp[band] += NBLOCKS;
    }

    // book keeping
    packets_per_band[band]++;
    previous_stamp[band] = timestamp;
  }

  // print diagnostics
  LOG("Packets read:\n");
  for (band=0; band < MAX_BANDS; band++) {
    if (bands_present[band] == 1) {
      missing = npackets - packets_per_band[band];
      missing_pct = (100.0 * missing) / (1.0 * npackets);
      LOG("Band %4i: %10ld out of %10ld, missing %10ld [%5.2f%%] records\n",
          band, packets_per_band[band], npackets, missing, missing_pct);
    }
  }
  LOG("Number of packets without timestamp: %li\n", notime);
  LOG("NOTE: Missing packets are filled in by repeatedly copying current packet.\n");

  // clean up and exit
exit:
  fflush(stdout);
  fflush(stderr);
  fflush(runlog);

  close(sockfd);
  fclose(runlog);
  exit(EXIT_SUCCESS);
}
