#include "libsha1.h"
#include "uftp.h"
int server_fd;
unsigned int filelen;
char *fn, *path;
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

void *worker(void *args) {
    struct threadarg arg = *(struct threadarg *)(args);
    int syn = arg.syn;
    struct sockaddr client_addr = arg.client_addr;
    socklen_t addr_len = arg.addr_len;
    free(args);

    char inbuf[BUFFER_LEN], outbuf[BUFFER_LEN];
    int cwnd = 1, ssthresh = 65535;
    int seq = 1, n;
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
    hdrsend.seq = seq++;
    hdrsend.ack = syn + 1;
    sendmsg(server_fd, &msgsend, 0);
    printf("Send: Syn %d Ack %d\n", hdrsend.syn, hdrsend.ack);
    hdrsend.syn = 0;

    do {
        n = recvmsg(server_fd, &msgrecv, 0);
        printf("Recv: Ack %d\n", hdrrecv.ack);
    } while (n < sizeof(struct pkthdr) || hdrrecv.ack != seq);

    // Connected, transfer data
    FILE *fp = fopen(path, "r");
    while (1) {
        for (int i = 0; i < cwnd; ++i) {
            n = fread(outbuf, 1, BUFFER_LEN, fp);
            if (!n) goto finish;
            iovsend[1].iov_len = n;
            hdrsend.seq = seq++;
            hdrsend.ack = hdrrecv.seq + 1;
            sendmsg(server_fd, &msgsend, 0);
            printf("Send: Seq %d Ack %d\n", hdrsend.seq, hdrsend.ack);
        }

        do {
            n = recvmsg(server_fd, &msgrecv, 0);
            printf("Recv: Ack %d\n", hdrrecv.ack);
        } while (n < sizeof(struct pkthdr) || hdrrecv.ack != seq);
    }

finish:
    hdrsend.fin = 1;
    hdrsend.seq = seq++;
    hdrsend.ack = hdrrecv.seq + 1;
    iovsend[1].iov_len = 0;
    sendmsg(server_fd, &msgsend, 0);
    printf("Send: Fin %d Ack %d\n", hdrsend.fin, hdrsend.ack);

    do {
        recvmsg(server_fd, &msgrecv, 0);
        printf("Recv: Ack %d\n", hdrrecv.ack);
    } while (hdrrecv.ack != seq);

    fclose(fp);

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
            printf("New client: Syn %d\n", hdrrecv.syn);
            struct threadarg *arg = malloc(sizeof(struct threadarg));
            pthread_t tid;
            arg->addr_len = msgrecv.msg_namelen;
            arg->client_addr = *(struct sockaddr *)msgrecv.msg_name;
            arg->syn = hdrrecv.syn;
            pthread_create(&tid, NULL, worker, arg);
            pthread_join(tid, NULL);
        }
    }
}