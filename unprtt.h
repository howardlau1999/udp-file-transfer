#ifndef	__unp_rtt_h
#define	__unp_rtt_h
#define _POSIX_SOURCE
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <error.h>
#include <errno.h>
#include <sys/time.h>

struct rtt_info {
  float		rtt_rtt;	/* most recent measured RTT, seconds */
  float		rtt_srtt;	/* smoothed RTT estimator, seconds */
  float		rtt_rttvar;	/* smoothed mean deviation, seconds */
  float		rtt_rto;	/* current RTO to use, seconds */
  int		rtt_nrexmt;	/* #times retransmitted: 0, 1, 2, ... */
  uint32_t	rtt_base;	/* #sec since 1/1/1970 at start */
};

#define	RTT_RXTMIN      2	/* min retransmit timeout value, seconds */
#define	RTT_RXTMAX     60	/* max retransmit timeout value, seconds */
#define	RTT_MAXNREXMT 	3	/* max #times to retransmit */

				/* function prototypes */
void	 rtt_debug(struct rtt_info *);
void	 rtt_init(struct rtt_info *);
void	 rtt_newpack(struct rtt_info *);
int		 rtt_start(struct rtt_info *);
void	 rtt_stop(struct rtt_info *, uint32_t);
int		 rtt_timeout(struct rtt_info *);
uint32_t rtt_ts(struct rtt_info *);

extern int	rtt_d_flag;	/* can be set nonzero for addl info */

#endif	/* __unp_rtt_h */
