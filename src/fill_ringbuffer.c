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

#define PACKETSIZE 6356           // Size of the packet, including the header in bytes
#define RECORDSIZE 6250           // Size of the record = packet - header in bytes
#define PACKHEADER 106            // Size of the packet header = PACKETSIZE-RECORDSIZE in bytes
#define MMSG_VLEN  256            // Batch message into single syscal using recvmmsg()

/* We currently use
 *  - one compound beam per instance
 *  - one instance of fill_ringbuffer connected to
 *  - one HDU. 
 *
 * Send on to ringbuffer a single second of data as a three dimensional array:
 * [tab_index][channel][record] of sizes [0..11][0..1535][0..paddedsize] = 18432 * (paddedsize+1) for a ringbuffer page
 *
 */

#define NPACKETSSEGMENT (12*1536*4)

#define SOCKBUFSIZE 67108864      // Buffer size of socket

FILE *runlog = NULL;

/*
 * Header description based on:
 * ARTS Interface Specification from BF to SC3+4
 * ASTRON_SP_066_InterfaceSpecificationSC34.pdf
 */
typedef struct {
  unsigned char marker_byte;         // SC3: 130, SC4: 140
  unsigned char format_version;      // Version: 0
  unsigned char cb_index;            // [0,36] one compound beam per fill_ringbuffer instance:: ignore
  unsigned char tab_index;           // [0,11] all tabs per fill_ringbuffer instance
  unsigned short channel;            // [0,1535] all channels per fill_ringbuffer instance
  unsigned short samples_per_packet; // RECORDSIZE (6250)
  /**
   * contains two unsigned integer numbers (each of 4 bytes); the
   * first number contains the number of time units that have elapsed since the
   * midnight of the 1st of January 1970, while the second number contains
   * the unit of Number of Samples per Packet the packet is associated with.
   */
  unsigned long timestamp;
  unsigned long flags[3];
  unsigned char record[RECORDSIZE];
} packet_t;

// #define LOG(...) {fprintf(logio, __VA_ARGS__)}; 
#define LOG(...) {fprintf(stdout, __VA_ARGS__); fprintf(runlog, __VA_ARGS__); fflush(stdout);}

/**
 * Print commandline optinos
 */
void printOptions()
{
  printf("usage: fill_ringbuffer -h <header file> -k <hexadecimal key> -s <starttime seconds after 1970> -d <duration (s)> -p <port> -l <logfile>\n");
  printf("e.g. fill_ringbuffer -h \"header1.txt\" -k 10 -s 11565158400000 -d 3600 -p 4000 -l log.txt\n");
  return;
}

/**
 * Parse commandline
 */
void parseOptions(int argc, char*argv[], char **header, char **key, int *starttime, int *duration, int *port, int *padded_size, char **logfile) {
  int c;

  int seth=0, setk=0, sets=0, setd=0, setp=0, setb=0, setl=0;
  while((c=getopt(argc,argv,"h:k:s:d:p:b:l:"))!=-1) {
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

      // -s starttime (seconds after 1970)
      case('s'):
        // *starttime=atol(optarg);
        *starttime = atoi(optarg);
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

      // -b padded size (bytes)
      case('b'):
        *padded_size = atoi(optarg);
        setb=1;
        break;

      // -l log file
      case('l'):
        *logfile = strdup(optarg);
        setl=1;
        break;
      default: printOptions(); exit(0);
    }
  }

  // All arguments are required
  if (!seth || !setk || !sets || !setd || !setp || !setl || !setb) {
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
 * Open a connection to the ringbuffer
 * The metadata (header block) is read from file
 * @param {dada_hdu_t **} hdu pointer to a pointer of HDU
 * @param {char *} header String containing the header file names to read
 * @param {char *} key String containing the shared memeory keys as hexadecimal numbers
 * @param {int} padded_size Stride of data array, needed to check buffer size
 * @returns {hdu *} A connected HDU
 */
dada_hdu_t *init_ringbuffer(char *header, char *key, int padded_size) {
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

  if (bufsz < NPACKETSSEGMENT * padded_size) {
    LOG("ERROR. ring buffer data block too small, should be at least %i\n", NPACKETSSEGMENT * padded_size);
    exit(EXIT_FAILURE);
  }

  return hdu;
}

int main(int argc, char** argv) {
  // network state
  int port;                 // port number
  int sockfd;               // socket file descriptor

  // ringbuffer state
  dada_hdu_t *hdu;
  char *buf; // pointer to current buffer

  // run parameters
  int duration;            // run time in seconds
  float missing_pct;       // Number of packets missed in percentage of expected number
  int missing;             // Number of packets missed
  int starttime;           // Starttime
  int endtime;             // Endtime
  int padded_size;

  // local vars
  char *header;
  char *key;
  char *logfile;
  const char mode = 'w';

  packet_t packet_buffer[MMSG_VLEN];   // Buffer for batch requesting packets via recvmmsg
  unsigned int packet_idx;             // Current packet index in MMSG buffer
  struct iovec iov[MMSG_VLEN];         // IO vec structure for recvmmsg
  struct mmsghdr msgs[MMSG_VLEN];      // multimessage hearders for recvmmsg

  packet_t *packet;                 // Pointer to current packet
  unsigned short cb_index = 999;    // Current compound beam index (fixed per run)
  unsigned long curr_time;          // Current timestamp
  unsigned int curr_segment;
  unsigned long packets_in_segment; // number of records processed per time segment

  // parse commandline
  parseOptions(argc, argv, &header, &key, &starttime, &duration, &port, &padded_size, &logfile);

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
  endtime = starttime + duration;
  LOG("fill ringbuffer version: " VERSION "\n");
  LOG("Starttime = %i\n", starttime);
  LOG("Endtime = %i\n", endtime);
  LOG("Duration = %i\n", duration);

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
  hdu = init_ringbuffer(header, key, padded_size);
  free(header);
  free(key);

  // clear packet counters
  packets_in_segment = 0;

  // start at the end of the packet buffer, so the main loop starts with a recvmmsg call
  packet_idx = MMSG_VLEN - 1;
  packet = &packet_buffer[packet_idx];

  // ============================================================
  // idle till starttime, but keep track of which bands there are
  // ============================================================
 
  curr_time = 0;
  packet_idx = MMSG_VLEN - 1;
  while (curr_time < starttime) {
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

    // keep track of compound beams
    cb_index = packet->cb_index;

    // keep track of timestamps
    curr_time = bswap_64(packet->timestamp);
  }

  // process the first (already-read) package by moving the packet_idx one back
  // this to compensate for the packet_idx++ statement in the first pass of the mainloop
  packet_idx--;

  //  get a new buffer
  buf = ipcbuf_get_next_write ((ipcbuf_t *)hdu->data_block);
  packets_in_segment = 0;

  LOG("STARTING WITH CB_INDEX=%i\n", cb_index);

  // ============================================================
  // run till endtime
  // ============================================================
 
  while (curr_time < endtime) {
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
 
    // check compound beam index 
    if (packet->cb_index != cb_index) {
      LOG("ERROR: unexpected compound beam index %d\n", packet->cb_index);
      goto exit;
    }

    // check number of samples_per_packet
    if (packet->samples_per_packet != RECORDSIZE) {
      LOG("Warning: incorrect number of samples in the packet %d\n", RECORDSIZE);
      goto exit;
    }

    // check timestamps:
    curr_time = bswap_64(packet->timestamp);
    segment = (curr_time - starttime) / 800000;
    if (segment != curr_segment) {
      // start of a new time segment:
      //  - mark the ringbuffer as filled
      if (ipcbuf_mark_filled ((ipcbuf_t *)hdu->data_block, NPACKETSSEGMENT * padded_size) < 0) {
        LOG("ERROR: cannot mark buffer as filled\n");
        goto exit;
      }

      //  - get a new buffer
      buf = ipcbuf_get_next_write ((ipcbuf_t *)hdu->data_block);

      // - print diagnostics
      missing = NPACKETSSEGMENT - packets_in_segment;
      missing_pct = (100.0 * missing) / (1.0 * NPACKETSSEGMENT);
      LOG("Compound beam %4i: time %i, missing: %6.3f%% (%i)\n", cb_index, curr_time, missing_pct, missing);

      //  - reset the packets counter and segment number
      curr_segment = segment
      packets_in_segment = 0;
    }

    // copy to ringbuffer
    memcpy(&buf[((packet->tab_index * 1536) + packet->channel) * padded_size + (curr_time % 200000) * RECORDSIZE], packet->record, RECORDSIZE);

    // book keeping
    packets_in_segment++;
  }

  // clean up and exit
exit:
  fflush(stdout);
  fflush(stderr);
  fflush(runlog);

  close(sockfd);
  fclose(runlog);
  exit(EXIT_SUCCESS);
}
