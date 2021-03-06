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
    struct filemetadata meta;
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
    // print_hdr(0, hdrsend);
    hdrsend.syn = 0;

    int n;
    do {
        n = recvmsg(client_fd, &msgrecv, 0);
        // print_hdr(1, hdrrecv);
    } while (n < sizeof(struct pkthdr) || hdrrecv.ack != seq);

    // Unpack file metadata
    memcpy(&meta, inbuf, sizeof(struct filemetadata));
    printf("Filename: %s\n", meta.fn);
    printf("Length: %lu\n", meta.filelen);
    printf("Create Time: %lu\n", meta.ctime);
    printf("SHA1: ");
    print_hex(meta.sha1, SHA1_DIGEST_SIZE);
    puts("");

    hdrsend.seq = seq;
    hdrsend.is_ack = 1;
    hdrsend.ack = hdrrecv.seq + 1;
    sendmsg(client_fd, &msgsend, 0);
    // print_hdr(0, hdrsend);
    hdrsend.is_ack = 0;
    // Connected, transfer data
    int LAR = hdrrecv.seq + 1;
    struct timeval tik, tok;
    gettimeofday(&tik, NULL);
    while (1) {
        do {
            n = recvmsg(client_fd, &msgrecv, 0);
        } while (n < sizeof(struct pkthdr) || hdrrecv.ack != seq);

        if (hdrrecv.seq == LAR) {
            LAR = hdrrecv.seq + 1;
            if (hdrrecv.fin) break;
            fwrite(inbuf, 1, n - sizeof(struct pkthdr), fp);
            hdrsend.seq = seq;
            if (LAR % 1000 == 0) {
                printf("\r%d MB Received", LAR / 1000);
                fflush(stdout);
            }
        } else {
            // printf("Expected %d Actual %d\n", LAR, hdrrecv.seq);
        }

        hdrsend.ack = LAR;
        hdrsend.is_ack = 1;
        hdrsend.ts = hdrrecv.ts;
        sendmsg(client_fd, &msgsend, 0);
        hdrsend.is_ack = 0;
    }

    hdrsend.is_ack = 1;
    hdrsend.ack = hdrrecv.seq + 1;
    hdrsend.seq = seq++;
    sendmsg(client_fd, &msgsend, 0);
    // print_hdr(0, hdrsend);

    hdrsend.is_ack = 0;
    hdrsend.fin = 1;
    hdrsend.seq = seq++;
    sendmsg(client_fd, &msgsend, 0);
    // print_hdr(0, hdrsend);

    fclose(fp);
    gettimeofday(&tok, NULL);
    uint64_t elapsed = (tok.tv_usec + tok.tv_sec * 1000000) -
                       (tik.tv_usec + tik.tv_sec * 1000000);
    printf(
        "\rBytes received: %ld  Speed: %.2lf bytes/sec (%.2lf Mbps) "
        "Elapsed: %.2lf seconds",
        meta.filelen, (double)meta.filelen / elapsed * 1000000,
        (double)meta.filelen / elapsed * 8, (double)elapsed / 1000000);
    fp = fopen(argv[3], "r");

    puts("");

    unsigned char buffer[BUFFER_LEN];
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

    if (filelen != meta.filelen) {
        printf("File Length mismatched!\n");
        exit(1);
    }

    if (memcmp(hval, meta.sha1, SHA1_DIGEST_SIZE)) {
        printf("SHA1 mismatched!");
        printf("Expected: ");
        print_hex(meta.sha1, SHA1_DIGEST_SIZE);
        puts("");

        printf("Actual: ");
        print_hex(hval, SHA1_DIGEST_SIZE);
        puts("");
        exit(1);
    }

    puts("Everything seems ok.");
}