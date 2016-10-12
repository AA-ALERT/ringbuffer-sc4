/* Program to read from the port and write to the ringbuffer */
/* Last edit 07 Sep 2016 */
/* Version 1.4 */
/* Code can now handle non-continuous bands and missing packets*/
/* Still debugging */
/* Author: Roy Smits, Jisk Attema */

/* Data comes in at 18.75 MHz in packages of 4800 bytes. */
/* 15833 blocks of 4800 bytes corresponds to 1 second of 19-MHz data */
/* 15625 blocks of 4800 bytes corresponds to 1 second of 18.75-MHz data */


#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <futils.h>
#include "udp.h"
#include "dada_hdu.h"
#include "dada_def.h"
#include "dada_pwc_main.h"
#include "assert.h"
#include "multilog.h"
#include <getopt.h>
#include "gsl/gsl_rng.h"
#include "gsl/gsl_randist.h"

#define MAX_STREAMS 64
#define MAX_HEADERS 64
#define MAX_KEYS    64
#define MAX_STRLEN  256
#define MAX_BANDS   16

#define PACKETSIZE 4840           // Size of the packet, including the header in bytes
#define RECSIZE 4800              // Size of the record = packet - header in bytes
#define PACKHEADER 40             // Size of the packet header = PACKETSIZE-RECSIZE in bytes
#define HEADERSIZE 4096           // Side of the dada header
#define HEADERFILE "header.txt"   // Name of the file containing the dadaheader
#define RECSPERSECOND 15625       // Number of packets / records per second = 781250 * 24 * 4 / 4800
#define NBLOCK 50                 // Number of data blocks in a packet / record, used to check the packet header
#define SOCKBUFSIZE 33554432      // Buffer size of socket 


/* structures dmadb datatype  */
typedef struct {
  int verbose;                             /* verbosity flag */
  long long nbufs;                         /* number of buffers to acquire */
  long long prev_pkt_cnt, bad_pkt_cnt;
  int FirstFlag;
  int fSize;                               /* file size of data */
  int nSecs;                               /* number of seconds to acquire */
  long long buf;
  char daemon;                             /* daemon mode */
  char *ObsId;
  char data[PACKETSIZE];
  char dataFromPrevPkt[2 * PACKETSIZE];
  int markerOffset;
  int sock;
  struct sockaddr_in sa;
} udp2db_t;

typedef struct {
  key_t array[MAX_KEYS];
  int nkeys;
} keys_type;

typedef struct {
  char *buffers[MAX_HEADERS];          /* Actual headers */ 
  uint64_t size[MAX_HEADERS];          /* Size of the buffer */
  char files[MAX_HEADERS][MAX_STRLEN]; /* All the filenames */
  int nheaders;
} header_type;

typedef struct {
  unsigned short band;         /* Which band */
  unsigned short nchannel;     /* Number of frequency channels */
  unsigned short nblocks;      /* Number of blocks */
  unsigned long timestamp;     /* Timestamp in units of 1/8000 s after Jan 1, 1970 at the beginning of the packet */
  char flags[25];              /* Flags */
  int lowband;                 /* Value of the lowest band */
  int highband;                /* Value of the lowest band */
  int allbands[MAX_STREAMS];   /* Value of all the bands */
  int allbands_index[MAX_BANDS];/* Store the index of each band */
} appheader_type;

char buf2d[MAX_STREAMS][PACKETSIZE];

/* Sesame Open. Opens file with proper check. */
FILE* Sopen(char *Fin, char *how)
{
  FILE *FileIn;
  if ((FileIn = fopen(Fin, how)) == NULL) 
  {
    fprintf(stderr, "Error opening file %s\n", Fin);
    exit(EXIT_FAILURE);
  }
  return(FileIn);
}

int cmpint(const void *v1, const void *v2)
{
  return ( *(int*)v1 - *(int*)v2 );
}

/* Function to find max and min of an int data set */
void MaxMin(int *a, int na, int *amax, int *amin)
{
  int i;
  *amax  = a[0];
  *amin  = *amax;
  for(i=0;i<na;i++) {
    if (*amin>a[i]) *amin = a[i];
    if (*amax<a[i]) *amax = a[i];
  }
  return;
}

int init(udp2db_t* udp2db, int port)
{
  int sockbufsize=SOCKBUFSIZE;
  udp2db->sock = socket(AF_INET, SOCK_DGRAM, 0); 
  setsockopt(udp2db->sock, SOL_SOCKET, SO_RCVBUF, &sockbufsize, sizeof(sockbufsize)); // Set socket buffer size
  memset(&udp2db->sa, 0, sizeof(udp2db->sa));
  udp2db->sa.sin_family = AF_UNSPEC;
  udp2db->sa.sin_addr.s_addr = htonl(INADDR_ANY);
  udp2db->sa.sin_port = htons(port);

  if (-1 == bind(udp2db->sock, (struct sockaddr *)&udp2db->sa, sizeof (udp2db->sa))){
    perror("bind failed");
    close(udp2db->sock);
    return -1;
  }

  udp2db->markerOffset = 0;
  udp2db->bad_pkt_cnt = 0;

  return 0;
}

/* Invert a string of length N. Make sure inverted is large enough. */
void invert_bytes(char *string, char inverted[], int N)
{
  int i;
  for (i=0; i<N; i++)
    inverted[N-1-i]=string[i];
}
    
void read_applicationheader(char *ah, appheader_type *appheader)
{
  char inverted[999];
  // Read header and convert from big endian to little endian
  invert_bytes(&ah[2], inverted, 2);
  memcpy(&appheader->band, &inverted, 2);
  invert_bytes(&ah[4], inverted, 2);
  memcpy(&appheader->nchannel, &inverted, 2);
  invert_bytes(&ah[6], inverted, 2);
  memcpy(&appheader->nblocks, &inverted, 2);
  invert_bytes(&ah[8], inverted, 8);
  memcpy(&appheader->timestamp, &inverted, 8);
  //  invert_bytes(&ah[16], inverted, 24);
  //  memcpy(&appheader->flags, &inverted, 24);
  return;
}

/* Wait for the packet matching the timestamp and then read the first nstream packets to extract the packet information */
int readfirstpackets(udp2db_t *udp2db, dada_hdu_t **hdu, appheader_type *appheader, int nstreams, unsigned long starttime, unsigned long *timestamps, FILE *logio)
{
  int recread=0, i, max, min;
  socklen_t length;
  int original_bandorder[MAX_STREAMS];
  char application_header_string[PACKHEADER];
  
  // fprintf(logio, "Going to read first packets.\n");

  // Loop until we get to the starttimestamp
  // ---------------------------------------
 
  do {
    // Read the packet
    recread = recvfrom(udp2db->sock, (void*) buf2d[0], PACKETSIZE, 0, (struct sockaddr *)&udp2db->sa, &length);
    if (recread < 0) {
      fprintf(logio, "error reading udp packet at initialization. recread = %d\n", recread);
      fclose(logio);
      exit(EXIT_FAILURE);
    }
    memcpy(application_header_string, buf2d[0], PACKHEADER); // Copy the application header to a string
    read_applicationheader(application_header_string, appheader); // Extract the application header from the string
    appheader->allbands[0]=appheader->band;
    timestamps[0] = appheader->timestamp;
  }
  while (appheader->timestamp < starttime);

  // Now we have the first packet after starttime in buf2d[]
  // -------------------------------------------------------

  // fprintf(logio, "band 0: %d\n", appheader->allbands[0]);

  for (i=1; i<nstreams; i++) { // Read the remaining nstreams-1 packets
    recread = recvfrom(udp2db->sock, (void*) buf2d[i], PACKETSIZE, 0, (struct sockaddr *)&udp2db->sa, &length);
    if (recread < 0) {
      fprintf(logio, "error reading udp packet at initialization. recread = %d\n", recread);
      fclose(logio);
      exit(EXIT_FAILURE);
    }
    memcpy(application_header_string, buf2d[i], PACKHEADER); // Copy the application header to a string
    read_applicationheader(application_header_string, appheader); // Extract the application header from the string
    appheader->allbands[i]=appheader->band;
    // fprintf(logio, "band %d: %d\n", i, appheader->allbands[i]);
    timestamps[i] = appheader->timestamp;
  } 
  
  // Now we have the first nstream packets in buf2d[]
  // ------------------------------------------------

  // Check the timestamp of the first packet
  if (timestamps[0] > starttime){
    fprintf(logio, "Warning! Timestamp in first packet is larger than given starttime (%ld > %ld).\n", appheader->timestamp, starttime);			     
  }

  // Check if bands are continuous
  MaxMin(appheader->allbands, nstreams, &max, &min);
  if (max-min+1 != nstreams) {
    fprintf(logio, "nstream = %d\n", nstreams);
    fprintf(logio, "Warning in fill_ringbuffer. Bandnumbers are not continuous.\n");
    fprintf(logio, "Bands are: ");
    for (i=0; i<nstreams; i++) {
      fprintf(logio, "%d\t", appheader->allbands[i]);
    }
    fprintf(logio, "\n");
  }
  appheader->lowband = min;   // Copy the value of lowest band into the appheader
  appheader->highband = max;  // Copy the value of highest band into the appheader
  // fprintf(logio, "band offset is %d\n", appheader->lowband);

  // Copy the original bandorder
  for (i=0; i<nstreams; i++) {
    original_bandorder[i] = appheader->allbands[i];
  }
  
  // Sort the bands and calculate the indices
  qsort(appheader->allbands, nstreams, sizeof(int), cmpint);
  for (i=0; i<nstreams; i++) {
    appheader->allbands_index[appheader->allbands[i]] = i;
  }

  // fprintf(logio, "The bands are: ");
  // for (i=0; i<nstreams; i++)
  //   fprintf(logio, "%d\t", appheader->allbands[i]);  
  // fprintf(logio, "\n");
  // fprintf(logio, "The index for these bands are: ");
  // for (i=0; i<nstreams; i++)
  //   fprintf(logio, "%d\t", appheader->allbands_index[appheader->allbands[i]]);
  // fprintf(logio, "\n");

  // Copy buffer into shared memory, skipping the PACKHEADER bytes of header
  for (i=0; i<nstreams; i++) { 
    memcpy(udp2db->data, &buf2d[i]+PACKHEADER, RECSIZE);
    if ( (ipcio_write(hdu[appheader->allbands_index[original_bandorder[i]]]->data_block, udp2db->data, RECSIZE) ) < RECSIZE) {
      fprintf(logio, "ERROR. Cannot write requested bytes to SHM\n");
      fclose(logio);
      exit(EXIT_FAILURE);
    }
  }

  if (appheader->nblocks != NBLOCK) {
    fprintf(logio, "Warning: number of blocks in packet is not equal to %d\n", NBLOCK);
  }

  // fprintf(logio, "Read the first record.\n");
  return recread;
}


// Read one packet and copy it to the ringbuffer
int readpacket(udp2db_t *udp2db, dada_hdu_t **hdu, appheader_type *appheader, unsigned long *timestamp, int record, int countband, int nstreams, FILE *logio)
{
  int recread=0;
  socklen_t length;
  char buf[PACKETSIZE];
  char application_header_string[PACKHEADER];
  
  // Read the packet 
  recread = recvfrom(udp2db->sock, (void*) buf, PACKETSIZE, 0, (struct sockaddr *)&udp2db->sa, &length);
  if (recread < 0) {
    fprintf(logio, "ERROR reading udp packet. recread = %d\n", recread);
    exit(0);
    return 0;
  }
  
  memcpy(application_header_string, buf, PACKHEADER);              /* Copy the application header to a string */
  read_applicationheader(application_header_string, appheader);    /* Extract the application header from the string */

  // Copy buffer into data, skipping the PACKHEADER bytes of header
  memcpy(udp2db->data, &buf[PACKHEADER], RECSIZE);

  // Check if timestamp is consistent with what is expected
  *timestamp = appheader->timestamp;

  // Copy record to shared memory
  if ( (ipcio_write(hdu[appheader->allbands_index[appheader->band]]->data_block, udp2db->data, RECSIZE) ) < RECSIZE){
    fprintf(logio, "ERROR. Cannot write requested bytes to shared memory\n");
    fclose(logio);
    exit(EXIT_FAILURE);
  }

  return recread;
}

/* This routine will scan the keystring and extract the keys */
int Readkeys(char *keystring, keys_type *keys)
{
  char *token;
  int nkeys = 0;

  token = strtok(keystring, " ");
  while (token) {
    // Copy the key to the array
    sscanf(token, "%x", &(keys->array[nkeys]));
    token = strtok(NULL, " ");
    nkeys++;
  }
  return nkeys;
}

/* This routine will scan the headerstring and extract the headers for the dada files */
int Readheaders(char *headerstring, header_type *headers)
{
  char *token;
  int nheaders = 0;

  token = strtok(headerstring, " ");
  while (token) {
    strncpy(headers->files[nheaders], token, MAX_STRLEN);
    headers->buffers[nheaders] = NULL;
    headers->size[nheaders] = 4096;

    token = strtok(NULL, " ");
    nheaders++;
  }
  return nheaders;
}

// Check if the nstream packets have identical timestamps.
int checktimestamps(int n, unsigned long *timestamps)
{
  int i;
  for (i=1; i<n; i++)
    if (timestamps[i] != timestamps[i-1]) return 1;
  return 0;
}

void PrintOptions()
{
  printf("usage: fill_ringbuffer -h <\"header files\"> -k <\"list of hexadecimal keys\"> -s <starttime (packets after 1970)> -d <duration (s)> -p <port> -l <logfile>\n");
  printf("e.g. fill_ringbuffer -h \"header1.txt header2.txt header3.txt header4.txt header5.txt header6.txt header7.txt header8.txt\" -k \"10 20 30 40 50 60 70 80\" -s 11565158400000 -d 3600 -p 4000 -l log.txt\n");
  return;
}


/* This function will process the arguments */
/* Arguments are header-file, list of keys and port */
void parseopts(int argc, char*argv[], char *headerstring, char *keystring, unsigned long *starttime, int *duration, int *port, char *logfile)
{
  int c;
  int seth=0, setk=0, sets=0, setd=0, setp=0, setl=0;
  while((c=getopt(argc,argv,"h:k:s:d:p:l:"))!=-1)
    switch(c) {
    case('h') : sprintf(headerstring, "%s", optarg); seth=1; break;
    case('k') : sprintf(keystring, "%s", optarg); setk=1; break;
    case('s') : *starttime=atol(optarg); sets=1; break;
    case('d') : *duration=atoi(optarg); setd=1; break;
    case('p') : *port=atoi(optarg); setp=1; break;
    case('l') : snprintf(logfile, 999, "%s", optarg); setl=1; break;
    default   : PrintOptions(); exit(0);
    }
  if (!seth || !setk || !sets || !setd || !setp || !setl) {PrintOptions(); exit(EXIT_FAILURE);}
}


int main(int argc, char** argv)
{
  udp2db_t udp2db;

  /* DADA header plus data */
  dada_hdu_t *hdu[MAX_STREAMS];
  char *ObsId = "Test";
  char logfile[999]; // location of the logfile
  FILE* logio;
  int port;
  int n, i, j;
  long long nbufs=0;
  char daemon = 0; /* daemon mode */
  int verbose = 0; /* verbosity */
  char writemode='W'; /* Needs to be a capital! */
  char headerstring[9999], keystring[999]; // read the arguments headerstring and keystring 
  multilog_t* log = multilog_open("fill_ringbuffer", 0);
  keys_type keys;
  header_type headers;
  appheader_type appheader;
  int nstreams;             // The number of headers = number of keys
  int duration;             // Duration of the observation in seconds
  long nrecs;               // Number of records / packets to read
  unsigned long starttime;  // Starttime of the observation in units of 1/781250 seconds after 1970
  unsigned long endtime;    // Endtime of the observation in units of 1/781250 seconds after 1970
  unsigned long timestamps[MAX_STREAMS]; // Keep track of the timestamps from each band.

  parseopts(argc, argv, headerstring, keystring, &starttime, &duration, &port, logfile); // Parse options
  logio = Sopen(logfile, "w"); // Open a logfile
  fprintf(logio, "Messages from fill_ringbuffer\n\n");
  nrecs = RECSPERSECOND * duration; // Number of records to read
  endtime = starttime + nrecs*NBLOCK;
  fprintf(logio, "Starttime = %ld\n", starttime);
  fprintf(logio, "Endtime = %ld\n", endtime);
  fprintf(logio, "Going to read %ld records\n", nrecs);

  /* read keys */
  Readkeys(keystring, &keys); // Read the keys.

  /* read headers */
  Readheaders(headerstring, &headers);

  if (headers.nheaders != keys.nkeys) {
    fprintf(logio, "ERROR: The number of headers do not match the number of keys\n");
    fclose(logio);
    exit(EXIT_FAILURE);
  }
  nstreams=headers.nheaders; // nstreams is just the number of headers = number of keys

  udp2db.verbose = verbose;
  udp2db.nbufs = nbufs;
  udp2db.buf = 0;
  udp2db.nSecs = duration;
  udp2db.daemon = daemon;
  udp2db.ObsId = ObsId;
  udp2db.FirstFlag = 0;
  
  fprintf(logio, "Going to initialise sockets...\n");
  if (init(&udp2db, port) < 0) {
    fprintf(logio, "Unable to initialise.\n");
    fclose(logio);
    exit(EXIT_FAILURE);
  }
  fprintf(logio, "Initialisation done.\n");

  /* Create the header/data blocks */
  for (i=0; i<nstreams; i++) {
    hdu[i] = dada_hdu_create (log);
    dada_hdu_set_key(hdu[i], keys.array[i]);
    if (dada_hdu_connect (hdu[i]) < 0) {
      fprintf(logio, "ERROR in dada_hdu_connect\n");
      fclose(logio);
      exit(EXIT_FAILURE);
    }

    /* Make data buffers readable */
    if (dada_hdu_lock_write_spec (hdu[i],writemode) < 0) {
      fprintf(logio, "ERROR in dada_hdu_lock_write_spec\n");
      fclose(logio);
      return EXIT_FAILURE;
      exit(EXIT_FAILURE);
    }

    /* Set headers */
    headers.size[i] = ipcbuf_get_bufsz (hdu[i]->header_block);
    headers.buffers[i] = ipcbuf_get_next_write (hdu[i]->header_block);
    if (!headers.buffers[i]) {
      fprintf(logio, "ERROR. Get next header block error.\n");
      fclose(logio);
      exit(EXIT_FAILURE);
    }

    /* Read header */
    if (fileread (headers.files[i], headers.buffers[i], headers.size[i]) < 0) {
      fprintf(logio, "ERROR. Cannot read header from %s\n", headers.files[i]);
      fclose(logio);
      exit(EXIT_FAILURE);
    }

    /* Mark filled */
    if (ipcbuf_mark_filled (hdu[i]->header_block, headers.size[i]) < 0) {
      fprintf(logio, "ERROR. Could not mark filled header block\n");
      fclose(logio);
      exit(EXIT_FAILURE);
    }
  }
  fprintf(logio, "Headers created.\n");

  /* First read all the bands once to get information from the packet headers */
  readfirstpackets(&udp2db, hdu, &appheader, nstreams, starttime, timestamps, logio);

  fprintf(logio, "Read first packets.\n");

  if (checktimestamps(nstreams, timestamps)) {
    fprintf(logio, "Warning: timestamps from record 0 are not identical!\n");
  }
  fprintf(logio, "Done checking timestamps.\n");

  for (i=0; i<nstreams; i++) {
    fprintf(logio, "%ld\n", timestamps[i]);
  }
  
  /* Loop over all runs */
  fprintf(logio, "Reading and buffering.\n");

  for (i=1; i<nrecs; i++) {                        // read from 1 to nrec records, because we already read number 0
    for (j=0; j<nstreams; j++) {
      n = readpacket(&udp2db, hdu, &appheader, &timestamps[j], i, j, nstreams, logio);
      if (n < 0) {
        fprintf(logio, "Warning, problem when reading record %d, stream %d\n", i, j);
      } else {
        fprintf(logio, "%ld\t", timestamps[j]-starttime); // FIXME: remove
      }
    }
    if (timestamps[0] > endtime) { 
      fprintf(logio, "Warning, end of observation occurred before all packets were read.\n");
      fprintf(logio, "I have %d packets out of %ld, which corresponds to %f seconds.\n", i+1, nrecs, i/(float)RECSPERSECOND);
      break;
    }
  }
  
  fprintf(logio, "Closing socket.\n");
  
  close(udp2db.sock);
  
  fprintf(logio, "All Done.\n");
  printf("All Done.\n");
  fclose(logio);
  multilog_close(log);  
  return 0;
}
