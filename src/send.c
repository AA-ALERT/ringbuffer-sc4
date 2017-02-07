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

#define PACKETSIZE 6356           // Size of the packet, including the header in bytes
#define RECORDSIZE 6250           // Size of the record = packet - header in bytes
#define PACKHEADER 106            // Size of the packet header = PACKETSIZE-RECORDSIZE in bytes
#define MMSG_VLEN  256            // Batch message into single syscal using recvmmsg()

#define UMSPPACKET (1.0)


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
  int timestamp;
  int subtime;
  unsigned long flags[3];
  unsigned char record[6250];
} packet_t;

int main(int argc , char *argv[]) {
  int sockfd;
  struct addrinfo hints, *servinfo, *p;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;

  char service[256];
  snprintf(service, 255, "%i", 7469);

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
  int counter = 0;
  int prev_time = 0, curr_time = 0;
  unsigned long dropped = 0;       // deliberately dropped packets
  while(1) {
    for(packet_idx=0; packet_idx < MMSG_VLEN; packet_idx++) {
      packet = &packet_buffer[packet_idx];

      packet->marker_byte = 140;
      packet->format_version = 0;
      packet->cb_index = 1;
      packet->samples_per_packet = RECORDSIZE;

      packet->channel = counter % 12;
      packet->tab_index = (counter / 12) % 1536;

      curr_time = counter / (12 * 1536);
      if (curr_time != prev_time) {
        prev_time = curr_time;
      }
      packet->timestamp = counter / (12 * 1536);

      if (counter % 12345 == 0) {
        // deliberately drop some packets
        counter += 1;
        dropped++;
      } else {
        packet->timestamp = counter / (12 * 1536);
      }

      counter += 1;
    }

    // Send some data
    // res = sendto(sockfd, &packet, PACKETSIZE, 0, p->ai_addr, p->ai_addrlen);
    if (sendmmsg(sockfd, msgs, MMSG_VLEN, 0) == -1) {
      perror("ERROR Could not send packets");
      goto exit;
    }

    // slow down sending a bit
    usleep(UMSPPACKET  * 0.5);
  }

exit:
  fprintf(stderr, "Deliberately unsent packets:    %li\n", dropped);
  // done, clean up
  free(servinfo); 
  close(sockfd);
}
