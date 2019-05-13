#ifndef _UFTP_H_
#define _UFTP_H_
#include <arpa/inet.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <memory.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUFFER_LEN 1024
#define MAX_WINDOW 4096

#define SYN 0x1
#define ACK 0x2
#define FIN 0x4

struct rtt_info {
    float rtt_rtt;     /* most recent measured RTT, seconds */
    float rtt_srtt;    /* smoothed RTT estimator, seconds */
    float rtt_rttvar;  /* smoothed mean deviation, seconds */
    float rtt_rto;     /* current RTO to use, seconds */
    int rtt_nrexmt;    /* #times retransmitted: 0, 1, 2, ... */
    uint32_t rtt_base; /* #sec since 1/1/1970 at start */
};

struct pkthdr {
    uint32_t syn;
    uint32_t seq;
    uint32_t is_ack;
    uint32_t ack;
    uint32_t fin;
    uint32_t win;
};

struct threadarg {
    struct sockaddr client_addr;
    socklen_t addr_len;
    uint32_t syn;
};

#define RTT_RXTMIN 2    /* min retransmit timeout value, seconds */
#define RTT_RXTMAX 60   /* max retransmit timeout value, seconds */
#define RTT_MAXNREXMT 3 /* max #times to retransmit */

/* function prototypes */
void rtt_debug(struct rtt_info *);
void rtt_init(struct rtt_info *);
void rtt_newpack(struct rtt_info *);
int rtt_start(struct rtt_info *);
void rtt_stop(struct rtt_info *, uint32_t);
int rtt_timeout(struct rtt_info *);
uint32_t rtt_ts(struct rtt_info *);

extern int rtt_d_flag; /* can be set nonzero for addl info */

void print_hdr(int recv, struct pkthdr hdr) {
    printf("%4s: %3s %3s %3s Seq=%d Ack=%d Win=%d\n", recv ? "Recv" : "Send", hdr.syn ? "SYN" : "", hdr.is_ack ? "ACK" : "",
           hdr.fin ? "FIN" : "", hdr.seq, hdr.ack, hdr.win);
}

#endif