#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BUF_SIZE 4096

int send_all(int sock, const void *buf, int len) {
    int total = 0;
    const char *p = buf;
    while (total < len) {
        int n = send(sock, p + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

int recv_all(int sock, void *buf, int len) {
    int total = 0;
    char *p = buf;
    while (total < len) {
        int n = recv(sock, p + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    int sockfd;
    struct sockaddr_in server_addr;

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    printf("Connected to %s:%d. Type commands, 'exit' to quit.\n",
           server_ip, port);

    char cmd_buf[BUF_SIZE];
    char out_buf[BUF_SIZE * 4];

    while (1) {
        printf("remote-shell> ");
        fflush(stdout);

        if (!fgets(cmd_buf, sizeof(cmd_buf), stdin)) {
            break;
        }

        size_t len = strlen(cmd_buf);
        if (len > 0 && cmd_buf[len - 1] == '\n') {
            cmd_buf[len - 1] = '\0';
            len--;
        }

        uint32_t net_len = htonl(len);
        if (send_all(sockfd, &net_len, sizeof(net_len)) == -1 ||
            send_all(sockfd, cmd_buf, len) == -1) {
            printf("Connection lost.\n");
            break;
        }

        if (strcmp(cmd_buf, "exit") == 0) {
            printf("Exit requested. Bye.\n");
            break;
        }

        uint32_t recv_len_net;
        if (recv_all(sockfd, &recv_len_net, sizeof(recv_len_net)) == -1) {
            printf("Failed to receive result length.\n");
            break;
        }

        uint32_t recv_len = ntohl(recv_len_net);
        if (recv_len >= sizeof(out_buf)) {
            printf("Result too large (%u bytes).\n", recv_len);
            break;
        }

        if (recv_all(sockfd, out_buf, recv_len) == -1) {
            printf("Failed to receive result body.\n");
            break;
        }

        out_buf[recv_len] = '\0';
        printf("%s", out_buf);
    }

    close(sockfd);
    return 0;
}
