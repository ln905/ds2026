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

int send_message(int sock, const char *msg) {
    uint32_t len = strlen(msg);
    uint32_t net_len = htonl(len);
    if (send_all(sock, &net_len, sizeof(net_len)) == -1) return -1;
    if (len > 0 && send_all(sock, msg, len) == -1) return -1;
    return 0;
}

int recv_message(int sock, char *buf, int max_len) {
    uint32_t net_len;
    if (recv_all(sock, &net_len, sizeof(net_len)) == -1) return -1;
    uint32_t len = ntohl(net_len);
    if (len >= (uint32_t)max_len) return -1;
    if (len > 0 && recv_all(sock, buf, len) == -1) return -1;
    buf[len] = '\0';
    return (int)len;
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
    char buf[BUF_SIZE];

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

    if (connect(sockfd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) == -1) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    printf("Connected to %s:%d\n", server_ip, port);

/* ---- AUTH PHASE ---- */
    if (recv_message(sockfd, buf, sizeof(buf)) <= 0) {
        printf("Disconnected.\n");
        close(sockfd);
        return 0;
    }
    printf("%s", buf); fflush(stdout);

    char input[BUF_SIZE];

    if (!fgets(input, sizeof(input), stdin)) {
        close(sockfd);
        return 0;
    }

    size_t len = strlen(input);
    if (len > 0 && input[len - 1] == '\n') input[len - 1] = '\0';

    send_message(sockfd, input);

    if (recv_message(sockfd, buf, sizeof(buf)) <= 0) {
        printf("Disconnected.\n");
        close(sockfd);
        return 0;
    }
    printf("%s", buf); fflush(stdout);

    if (!fgets(input, sizeof(input), stdin)) {
        close(sockfd);
        return 0;
    }
    len = strlen(input);
    if (len > 0 && input[len - 1] == '\n') input[len - 1] = '\0';

    send_message(sockfd, input);

    if (recv_message(sockfd, buf, sizeof(buf)) <= 0) {
        printf("Disconnected.\n");
        close(sockfd);
        return 0;
    }
    printf("%s", buf);
    if (strstr(buf, "successful") == NULL) {
        while (1) {
            if (strncmp(buf, "Authentication failed", 21) == 0) {
                close(sockfd);
                return 0;
            }
            if (strncmp(buf, "Enter username", 14) == 0) {
                printf("%s", buf); fflush(stdout);
                if (!fgets(input, sizeof(input), stdin)) {
                    close(sockfd);
                    return 0;
                }
                len = strlen(input);
                if (len > 0 && input[len - 1] == '\n') input[len - 1] = '\0';
                send_message(sockfd, input);

                if (recv_message(sockfd, buf, sizeof(buf)) <= 0) {
                    printf("Disconnected.\n");
                    close(sockfd);
                    return 0;
                }
                printf("%s", buf); fflush(stdout);
                if (!fgets(input, sizeof(input), stdin)) {
                    close(sockfd);
                    return 0;
                }
                len = strlen(input);
                if (len > 0 && input[len - 1] == '\n') input[len - 1] = '\0';
                send_message(sockfd, input);

                if (recv_message(sockfd, buf, sizeof(buf)) <= 0) {
                    printf("Disconnected.\n");
                    close(sockfd);
                    return 0;
                }
                printf("%s", buf);
                if (strstr(buf, "successful")) break;
            } else {
                // các message khác
                if (recv_message(sockfd, buf, sizeof(buf)) <= 0) {
                    printf("Disconnected.\n");
                    close(sockfd);
                    return 0;
                }
                printf("%s", buf);
                if (strstr(buf, "successful")) break;
            }
        }
    }

/* ---- SHELL PHASE ---- */
    while (1) {
        printf("remote-shell> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        len = strlen(input);
        if (len > 0 && input[len - 1] == '\n') input[len - 1] = '\0';

        if (send_message(sockfd, input) == -1) {
            printf("Connection lost.\n");
            break;
        }

        if (strcmp(input, "exit") == 0) {
            if (recv_message(sockfd, buf, sizeof(buf)) > 0) {
                printf("%s", buf);
            }
            break;
        }

        int r = recv_message(sockfd, buf, sizeof(buf));
        if (r <= 0) {
            printf("Connection lost.\n");
            break;
        }
        printf("%s", buf);
    }

    close(sockfd);
    return 0;
}
