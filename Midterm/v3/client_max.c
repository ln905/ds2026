#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdint.h>
#include <limits.h>

#define BUF_SIZE 4096
#define MAX_FILE_SIZE (10*1024*1024)

int send_all(int sock, const void *buf, int len) {
    int total = 0;
    const char *p = (const char *)buf;
    while (total < len) {
        int n = send(sock, p + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

int recv_all(int sock, void *buf, int len) {
    int total = 0;
    char *p = (char *)buf;
    while (total < len) {
        int n = recv(sock, p + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

int send_message(int sock, const char *msg) {
    uint32_t len = (uint32_t)strlen(msg);
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

void trim_end(char *s) {
    int i = (int)strlen(s) - 1;
    while (i >= 0 && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
        s[i--] = '\0';
}

int handle_upload_cmd(int sockfd, char *input) {
    char *tok = strtok(input, " ");
    tok = strtok(NULL, " ");
    if (!tok) {
        printf("Usage: upload <localfile> <remotefile>\n");
        return 0;
    }
    char local[PATH_MAX];
    char remote[PATH_MAX];
    strncpy(local, tok, sizeof(local));
    local[sizeof(local) - 1] = '\0';

    tok = strtok(NULL, " ");
    if (!tok) {
        printf("Usage: upload <localfile> <remotefile>\n");
        return 0;
    }
    strncpy(remote, tok, sizeof(remote));
    remote[sizeof(remote) - 1] = '\0';

    FILE *f = fopen(local, "rb");
    if (!f) {
        perror("fopen local file");
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || size > MAX_FILE_SIZE) {
        fclose(f);
        printf("File size invalid or too large.\n");
        return 0;
    }

    char *buf = (char *)malloc(size);
    if (!buf) {
        fclose(f);
        printf("malloc failed.\n");
        return 0;
    }

    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    if (rd != (size_t)size) {
        free(buf);
        printf("Read error.\n");
        return 0;
    }

    char header[BUF_SIZE];
    snprintf(header, sizeof(header), "UPLOAD %s %ld", remote, size);
    if (send_message(sockfd, header) == -1) {
        free(buf);
        printf("Connection lost.\n");
        return -1;
    }

    if (send_all(sockfd, buf, (int)size) == -1) {
        free(buf);
        printf("Connection lost while sending file.\n");
        return -1;
    }

    free(buf);

    char resp[BUF_SIZE];
    int r = recv_message(sockfd, resp, sizeof(resp));
    if (r <= 0) {
        printf("Failed to receive upload response.\n");
        return -1;
    }
    printf("%s", resp);
    return 0;
}

int handle_download_cmd(int sockfd, char *input) {
    char *tok = strtok(input, " ");
    tok = strtok(NULL, " ");
    if (!tok) {
        printf("Usage: download <remotefile> <localfile>\n");
        return 0;
    }
    char remote[PATH_MAX];
    char local[PATH_MAX];
    strncpy(remote, tok, sizeof(remote));
    remote[sizeof(remote) - 1] = '\0';

    tok = strtok(NULL, " ");
    if (!tok) {
        printf("Usage: download <remotefile> <localfile>\n");
        return 0;
    }
    strncpy(local, tok, sizeof(local));
    local[sizeof(local) - 1] = '\0';

    char header[BUF_SIZE];
    snprintf(header, sizeof(header), "DOWNLOAD %s", remote);
    if (send_message(sockfd, header) == -1) {
        printf("Connection lost.\n");
        return -1;
    }

    char resp[BUF_SIZE];
    int r = recv_message(sockfd, resp, sizeof(resp));
    if (r <= 0) {
        printf("Connection lost.\n");
        return -1;
    }

    if (strncmp(resp, "DOWNLOAD_ERR", 12) == 0) {
        printf("%s\n", resp);
        return 0;
    }

    long size;
    if (sscanf(resp, "DOWNLOAD_OK %ld", &size) != 1) {
        printf("Invalid response: %s\n", resp);
        return 0;
    }

    if (size < 0 || size > MAX_FILE_SIZE) {
        printf("Download size invalid.\n");
        return 0;
    }

    char *buf = (char *)malloc(size);
    if (!buf) {
        printf("malloc failed.\n");
        return 0;
    }

    if (recv_all(sockfd, buf, (int)size) == -1) {
        free(buf);
        printf("Connection lost while receiving file.\n");
        return -1;
    }

    FILE *f = fopen(local, "wb");
    if (!f) {
        perror("fopen local");
        free(buf);
        return 0;
    }

    size_t written = fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);

    if (written != (size_t)size) {
        printf("Write error.\n");
        return 0;
    }

    printf("Downloaded %ld bytes to %s\n", size, local);
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
    trim_end(input);
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
    trim_end(input);
    send_message(sockfd, input);

    while (1) {
        int r = recv_message(sockfd, buf, sizeof(buf));
        if (r <= 0) {
            printf("Disconnected.\n");
            close(sockfd);
            return 0;
        }
        printf("%s", buf);

        if (strstr(buf, "successful")) {
            break;
        }
        if (strstr(buf, "Authentication failed too many times")) {
            close(sockfd);
            return 0;
        }
        if (strstr(buf, "Enter username:")) {
            if (!fgets(input, sizeof(input), stdin)) {
                close(sockfd);
                return 0;
            }
            trim_end(input);
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
            trim_end(input);
            send_message(sockfd, input);
        }
    }

/* ---- SHELL PHASE ---- */
    while (1) {
        printf("remote-shell> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        trim_end(input);

        if (strncmp(input, "upload ", 7) == 0) {
            // handle upload locally
            char tmp[BUF_SIZE];
            strncpy(tmp, input, sizeof(tmp));
            tmp[sizeof(tmp) - 1] = '\0';
            if (handle_upload_cmd(sockfd, tmp) == -1) break;
            continue;
        }

        if (strncmp(input, "download ", 9) == 0) {
            char tmp[BUF_SIZE];
            strncpy(tmp, input, sizeof(tmp));
            tmp[sizeof(tmp) - 1] = '\0';
            if (handle_download_cmd(sockfd, tmp) == -1) break;
            continue;
        }

        if (send_message(sockfd, input) == -1) {
            printf("Connection lost.\n");
            break;
        }

        if (strcmp(input, "exit") == 0) {
            int r = recv_message(sockfd, buf, sizeof(buf));
            if (r > 0) printf("%s", buf);
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