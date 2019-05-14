#include <setjmp.h>
#include "libsha1.h"
#include "uftp.h"
int server_fd;
unsigned int filelen;
char *fn, *path;
static sigjmp_buf jmpbuf;
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

uint64_t time_now() {
    struct timeval current;
    gettimeofday(&current, 0);
    return current.tv_sec * 1000000 + current.tv_usec;
}

int64_t timeOut, estimatedRTT = 500000, deviation = 1, difference = 0,
                 minRTT = 200000, maxRTT = 6000000;
void update_timeout(uint64_t sentTime) {
    uint64_t sampleRTT = time_now() - sentTime;
    estimatedRTT = 0.875 * estimatedRTT + 0.125 * sampleRTT;  // alpha = 0.875
    deviation +=
        (0.25 * (abs(sampleRTT - estimatedRTT) - deviation));  // delta = 0.25
    timeOut = (estimatedRTT + 4 * deviation);  // mu = 1, phi = 4
    
    if (timeOut < minRTT) timeOut = minRTT;
    if (timeOut > maxRTT) timeOut = maxRTT;
}

int ack_cnt = 0, last_ack_cnt = 0, cwnd = 1, ssthresh = 32;

static void rtt_alarm(union sigval val) {
    if (cwnd >= ssthresh && cwnd < MAX_WINDOW - 1 && ack_cnt - last_ack_cnt > cwnd * 10) ++cwnd, last_ack_cnt = ack_cnt;
    printf("RTT Alarm: ssthresh=%d cwnd=%d, ack_cnt=%d\n", ssthresh, cwnd, ack_cnt);
}

static void sig_alarm(int signo) { siglongjmp(jmpbuf, 1); }

void worker(int syn, struct sockaddr client_addr, socklen_t addr_len) {
    signal(SIGALRM, sig_alarm);

    timer_t timerid;
    struct sigevent sev;
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_signo = SIGALRM;
    sev.sigev_value.sival_ptr = &timerid;
    sev.sigev_notify_function = rtt_alarm;
    timer_create(CLOCK_REALTIME, &sev, &timerid);
    struct itimerspec spec;
    spec.it_value.tv_nsec = estimatedRTT * 1000;
    spec.it_interval.tv_nsec = estimatedRTT * 1000;
    timer_settime(timerid, 0, &spec, NULL);

    cwnd = 1, ssthresh = 64;
    char inbuf[BUFFER_LEN], outbuf[BUFFER_LEN];
    char outwnd[MAX_WINDOW + 1][BUFFER_LEN];
    int outlen[MAX_WINDOW + 1];
    struct pkthdr outhdr[MAX_WINDOW + 1];
    int seq = 0, n, rseq = syn;
    int sendbase = 0;
    struct iovec iovsend[2], iovrecv[2];
    // Try to connect
    struct pkthdr hdrsend = {0}, hdrrecv = {0};
    struct msghdr msgsend = {0}, msgrecv = {0};

    msgsend.msg_name = &client_addr;
    msgsend.msg_namelen = addr_len;
    msgsend.msg_iov = iovsend;
    msgsend.msg_iovlen = 2;

    iovsend[0].iov_base = &hdrsend;
    iovsend[0].iov_len = sizeof(struct pkthdr);
    iovsend[1].iov_base = outbuf;
    iovsend[1].iov_len = 0;

    msgrecv.msg_name = NULL;
    msgrecv.msg_namelen = 0;
    msgrecv.msg_iov = iovrecv;
    msgrecv.msg_iovlen = 2;

    iovrecv[0].iov_base = &hdrrecv;
    iovrecv[0].iov_len = sizeof(struct pkthdr);
    iovrecv[1].iov_base = inbuf;
    iovrecv[1].iov_len = BUFFER_LEN;

    hdrsend.syn = 1;
    hdrsend.is_ack = 1;
    hdrsend.seq = seq++;
    hdrsend.ack = ++rseq;
    sendmsg(server_fd, &msgsend, 0);
    print_hdr(0, hdrsend);
    hdrsend.syn = 0;
    hdrsend.is_ack = 0;

    do {
        n = recvmsg(server_fd, &msgrecv, 0);
        print_hdr(1, hdrrecv);
    } while (n < sizeof(struct pkthdr) || hdrrecv.ack != seq);
    sendbase++;
    int rxmt = 0;
    // Connected, transfer data
    FILE *fp = fopen(path, "r");
    int finished = 0;
    int LAR = 1;
    int stuffed = 0;
    while (1) {
    sendnext:
        for (; seq - sendbase < cwnd && !finished; ++seq) {
            printf("Base: %d, Seq: %d, Cwnd: %d\n", sendbase, seq, cwnd);
            n = fread(outwnd[seq - sendbase], 1, BUFFER_LEN, fp);
            if (!n) {
                finished = 1;
                hdrsend.fin = 1;
                hdrsend.seq = seq;
                hdrsend.ack = rseq;
                hdrsend.ts = time_now();
                iovsend[1].iov_len = 0;
                iovsend[0].iov_base = &hdrsend;
                outlen[seq - sendbase] = 0;
                memcpy(&outhdr[seq - sendbase], &hdrsend,
                       sizeof(struct pkthdr));
                sendmsg(server_fd, &msgsend, 0);
                print_hdr(0, outhdr[seq - sendbase]);
            } else {
                iovsend[1].iov_base = outwnd[seq - sendbase];
                iovsend[1].iov_len = outlen[seq - sendbase] = n;
                hdrsend.seq = seq;
                hdrsend.ack = rseq;
                hdrsend.ts = time_now();
                outhdr[seq - sendbase] = hdrsend;
                iovsend[0].iov_base = &hdrsend;
                memcpy(&outhdr[seq - sendbase], &hdrsend,
                       sizeof(struct pkthdr));
                sendmsg(server_fd, &msgsend, 0);
                // print_hdr(0, hdrsend);
            }
        }

        if (sigsetjmp(jmpbuf, 1) != 0) {
	        printf("Timeout\n");
	        if (++rxmt > 3) goto finish; 
	    clearstuff:;    
            for (int i = 0; i < seq - sendbase && i < cwnd; ++i) {
                iovsend[1].iov_len = outlen[i];
                iovsend[1].iov_base = outwnd[i];
                iovsend[0].iov_base = &outhdr[i];
		        outhdr[i].ts = time_now();
                sendmsg(server_fd, &msgsend, 0);
                print_hdr(0, outhdr[i]);
            }
            ssthresh = cwnd * 0.8;
            if (ssthresh < 2) ssthresh = 2;
	        cwnd = 1; 
	        goto waitack;
        fastrxmt:;
            
            for (int i = 0; i < seq - sendbase && i < cwnd; ++i) {
                iovsend[1].iov_len = outlen[i];
                iovsend[1].iov_base = outwnd[i];
                iovsend[0].iov_base = &outhdr[i];
		        outhdr[i].ts = time_now();
                sendmsg(server_fd, &msgsend, 0);
                print_hdr(0, outhdr[i]);
            }
        }
    waitack:;
        struct itimerval timer;
        struct timeval rto;
        rto.tv_sec = timeOut / 1000000;
        rto.tv_usec = timeOut % 1000000;
	    // printf("RTO: %lu\n", timeOut);
        timer.it_value = rto;
        setitimer(ITIMER_REAL, &timer, NULL);
        do {
            n = recvmsg(server_fd, &msgrecv, 0);
            // printf("Sendbase: %d\n", sendbase);
            // print_hdr(1, hdrrecv);
	        update_timeout(hdrrecv.ts);

            if (hdrrecv.ack >= sendbase + 1) {
                alarm(0);
		        ++ack_cnt;
		    if (cwnd < ssthresh) {
		    cwnd *= 2;
            if (cwnd > ssthresh)  { cwnd= ssthresh; }
                    if (cwnd >= MAX_WINDOW) cwnd = MAX_WINDOW - 1;
                }
		    rxmt = 0;
                int acked = hdrrecv.ack - sendbase;
                sendbase = hdrrecv.ack;
                for (int i = 0; i < seq - sendbase; ++i) {
                    memmove(outwnd[i], outwnd[i + acked], BUFFER_LEN);
                }
                memmove(outlen, outlen + acked, sizeof(int) * (seq - sendbase));
                memmove(outhdr, outhdr + acked,
                        sizeof(struct pkthdr) * (seq - sendbase));
		
                goto sendnext;
            }
            if (hdrrecv.fin) goto finish;
        } while (1);


    }

finish:

    fclose(fp);
    alarm(0);
    timer_delete(timerid);
    printf("File transfer finished.\n");
}

int main(int argc, char *argv[]) {
    int new_fd, epoll_fd;
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage clients_addr;
    socklen_t sin_size, addr_size;
    struct sigaction sa;
    const int BACKLOG = 10;
    const int TIMEOUT = 30000;
    int yes = 1;
    char s[INET6_ADDRSTRLEN];
    int rv;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    FILE *fp;

    if (3 != argc) {
        fprintf(stderr, "usage: %s port filename\n", argv[0]);
        exit(1);
    }

    fp = fopen(argv[2], "r");
    unsigned char buffer[BUFFER_LEN], sendbuf[BUFFER_LEN];
    unsigned int n;

    puts("Calculating SHA1...");
    // Caculate SHA1 and length of the file
    sha1_ctx cx[1];
    unsigned char hval[SHA1_DIGEST_SIZE];

    sha1_begin(cx);
    while ((n = fread(buffer, 1, BUFFER_LEN, fp)) != 0) {
        sha1_hash(buffer, n, cx);
        filelen += n;
    }
    sha1_end(hval, cx);

    // Get file metadata
    struct stat metadata;
    fstat(fileno(fp), &metadata);

    if (!fp) {
        fprintf(stderr, "cannot open file %s\n", argv[2]);
    }

    path = argv[2];
    fn = argv[2];

    if ((rv = getaddrinfo(NULL, argv[1], &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((server_fd =
                 socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("server: socket error");
            continue;
        }

        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes,
                       sizeof(int)) == -1) {
            perror("server: setsockopt error");
            exit(1);
        }

        if (bind(server_fd, p->ai_addr, p->ai_addrlen) == -1) {
            close(server_fd);
            perror("server: bind error");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo);

    if (p == NULL) {
        fprintf(stderr, "server: failed to bind\n");
        exit(1);
    }

    printf("Server listening on port %s\n", argv[1]);

    memset(buffer, 0, sizeof buffer);
    addr_size = sizeof clients_addr;
    struct msghdr msgrecv = {0};
    struct pkthdr hdrrecv = {0};
    struct iovec iovrecv[2];
    msgrecv.msg_name = &clients_addr;
    msgrecv.msg_namelen = addr_size;
    msgrecv.msg_iov = iovrecv;
    msgrecv.msg_iovlen = 2;

    iovrecv[0].iov_base = &hdrrecv;
    iovrecv[0].iov_len = sizeof(struct pkthdr);
    iovrecv[1].iov_base = buffer;
    iovrecv[1].iov_len = BUFFER_LEN;

    while (1) {
        int n = recvmsg(server_fd, &msgrecv, 0);

        if (hdrrecv.syn) {
            printf("New client\n");
            print_hdr(1, hdrrecv);
            worker(hdrrecv.seq, *(struct sockaddr *)&clients_addr, addr_size);
        }
    }
}
