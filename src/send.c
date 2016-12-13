// http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html#theory

// needed for GNU extension to recvfrom: recvmmsg, bswap
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <byteswap.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

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
#define PACKETSIZE 4840           // Size of the packet, including the header in bytes
#define NBLOCKS 50                // Number of data blocks in a packet / record, used to check the packet header
#define MMSG_VLEN  256            // Batch message into single syscal using recvmmsg()

typedef struct {
  unsigned short unused_A;
  unsigned short band;
  unsigned short channel;
  unsigned short nblocks;
  unsigned long  timestamp;
  unsigned long flags[3];
  char record[4800];
} packet_t;

struct addrinfo *connect_to_client(int *sockfd) {
  struct addrinfo hints, *servinfo;

  // first, load up address structs with getaddrinfo():
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  getaddrinfo(NULL, "4000", &hints, &servinfo);

  // make a socket:
  *sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);

  // bind local address
  if(bind(*sockfd, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
    perror(NULL);
    exit(1);
  }

  return servinfo;
}

int main(int argc , char *argv[]) {
  int sockfd;
  struct addrinfo hints, *servinfo, *p;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  char service[256];
  snprintf(service, 255, "%i", 4000);

  // find possible connections
  if(getaddrinfo("127.0.0.1", service, &hints, &servinfo) != 0) {
    perror(NULL);
    exit(EXIT_FAILURE);
  }

  // loop through all the results and make a socket
  for(p = servinfo; p != NULL; p = p->ai_next) {
    sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sockfd == -1) {
      perror("talker: socket");
      continue;
    }

    if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      continue;
    }

    break;
  }

  if (p==NULL) {
    fprintf(stderr, "Cannot open connection\n");
    exit(EXIT_FAILURE);
  }

  // keep sending packets till user hits C-c
  packet_t packet_buffer[MMSG_VLEN];   // Buffer for batch requesting packets via recvmmsg
  unsigned int packet_idx;             // Current packet index in MMSG buffer
  struct iovec iov[MMSG_VLEN];         // IO vec structure for recvmmsg
  struct mmsghdr msgs[MMSG_VLEN];      // multimessage hearders for recvmmsg

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

  packet_t *packet;                // Pointer to current packet
  unsigned long timestamp = 0;     // timestamp of current packet
  while(timestamp < 1000000000) {
    for(packet_idx=0; packet_idx < MMSG_VLEN; packet_idx++) {
      packet = &packet_buffer[packet_idx];

      packet->nblocks = bswap_16(NBLOCKS);
      packet->band = bswap_16(4);
      packet->timestamp = bswap_64(timestamp);
      timestamp += NBLOCKS;
    }

    // Send some data
    // res = sendto(sockfd, &packet, PACKETSIZE, 0, p->ai_addr, p->ai_addrlen);
    if (sendmmsg(sockfd, msgs, MMSG_VLEN, 0) == -1) {
      perror("ERROR Could not send packets");
      goto exit;
    }
  }

exit:
  // done, clean up
  free(servinfo); 
  close(sockfd);
}
