#include "libsha1.h"
#include "uftp.h"
int client_fd;
unsigned int filelen;
char *fn, *path;
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

int main(int argc, char *argv[]) {
    struct addrinfo hints, *servinfo, *p;
    int rv;
    char s[INET6_ADDRSTRLEN];
    FILE *fp;

    if (4 != argc) {
        fprintf(stderr, "usage: %s hostname port fn\n", argv[0]);
        exit(1);
    }

    fp = fopen(argv[3], "w");

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    if ((rv = getaddrinfo(argv[1], argv[2], &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(1);
    }

    for (p = servinfo; p != NULL; p = p->ai_next) {
        if ((client_fd =
                 socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("client: socket error");
            continue;
        }
        break;
    }

    if (p == NULL) {
        fprintf(stderr, "client: failed to connect\n");
        exit(2);
    }

    puts("Request file...");

    freeaddrinfo(servinfo);

    char inbuf[BUFFER_LEN], outbuf[BUFFER_LEN];
    int cwnd = 1, ssthresh = 65535;
    int seq = 0;
    struct iovec iovsend[2], iovrecv[2];
    // Try to connect
    struct pkthdr hdrsend = {0}, hdrrecv = {0};
    struct msghdr msgsend = {0}, msgrecv = {0};

    msgsend.msg_name = p->ai_addr;
    msgsend.msg_namelen = p->ai_addrlen;
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
    hdrsend.seq = seq++;
    sendmsg(client_fd, &msgsend, 0);
    print_hdr(0, hdrsend);
    hdrsend.syn = 0;

    int n;
    do {
        n = recvmsg(client_fd, &msgrecv, 0);
        print_hdr(1, hdrrecv);
    } while (n < sizeof(struct pkthdr) || hdrrecv.ack != seq);

    hdrsend.seq = seq;
    hdrsend.is_ack = 1;
    hdrsend.ack = hdrrecv.seq + 1;
    sendmsg(client_fd, &msgsend, 0);
    print_hdr(0, hdrsend);
    hdrsend.is_ack = 0;
    // Connected, transfer data
    int LAR = hdrrecv.seq + 1;
    while (1) {
        do {
            n = recvmsg(client_fd, &msgrecv, 0);
            print_hdr(1, hdrrecv);
        } while (n < sizeof(struct pkthdr) || hdrrecv.ack != seq);
        
        if (hdrrecv.seq == LAR) {
            LAR = hdrrecv.seq + 1;
            fwrite(inbuf, 1, n - sizeof(struct pkthdr), fp);
            if (hdrrecv.fin) break;
            hdrsend.seq = seq;
        } else {
            printf("Expected %d Actual %d\n", LAR, hdrrecv.seq);
        }
        
        hdrsend.ack = LAR;
        hdrsend.is_ack = 1;
        hdrsend.ts = hdrrecv.ts;
        sendmsg(client_fd, &msgsend, 0);
        print_hdr(0, hdrsend);
        hdrsend.is_ack = 0;
    }

    hdrsend.is_ack = 1;
    hdrsend.ack = hdrrecv.seq + 1;
    hdrsend.seq = seq++;
    sendmsg(client_fd, &msgsend, 0);
    print_hdr(0, hdrsend);

    hdrsend.is_ack = 0;
    hdrsend.fin = 1;
    hdrsend.seq = seq++;
    sendmsg(client_fd, &msgsend, 0);
    print_hdr(0, hdrsend);

    fclose(fp);
}