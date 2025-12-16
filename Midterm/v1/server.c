#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define PORT 12345
#define BACKLOG 10
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

int execute_command(const char *cmd, char *out_buf, int max_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(out_buf, max_len, "Failed to run command: %s\n", strerror(errno));
        return strlen(out_buf);
    }

    int total = 0;
    while (!feof(fp) && total < max_len - 1) {
        int n = fread(out_buf + total, 1, max_len - 1 - total, fp);
        if (n <= 0) break;
        total += n;
    }
    out_buf[total] = '\0';
    pclose(fp);
    return total;
}

int main() {
    int listen_fd, new_fd, max_fd, i;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len;

    fd_set master_set, read_fds;

    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // lắng nghe mọi interface
    server_addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, BACKLOG) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Remote shell server listening on port %d...\n", PORT);

    FD_ZERO(&master_set);
    FD_SET(listen_fd, &master_set);
    max_fd = listen_fd;

    while (1) {
        read_fds = master_set;

        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) == -1) {
            perror("select");
            exit(EXIT_FAILURE);
        }

        for (i = 0; i <= max_fd; i++) {
            if (!FD_ISSET(i, &read_fds)) continue;

            if (i == listen_fd) {
                addr_len = sizeof(client_addr);
                new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
                if (new_fd == -1) {
                    perror("accept");
                    continue;
                }

                FD_SET(new_fd, &master_set);
                if (new_fd > max_fd) max_fd = new_fd;

                printf("New connection from %s:%d (fd=%d)\n",
                       inet_ntoa(client_addr.sin_addr),
                       ntohs(client_addr.sin_port),
                       new_fd);
            } else {
                uint32_t net_len;
                if (recv_all(i, &net_len, sizeof(net_len)) == -1) {
                    printf("Client fd=%d disconnected.\n", i);
                    close(i);
                    FD_CLR(i, &master_set);
                    continue;
                }

                uint32_t len = ntohl(net_len);
                if (len == 0 || len > 4096) {
                    printf("Invalid length from fd=%d\n", i);
                    close(i);
                    FD_CLR(i, &master_set);
                    continue;
                }

                char cmd_buf[BUF_SIZE];
                memset(cmd_buf, 0, sizeof(cmd_buf));

                if (recv_all(i, cmd_buf, len) == -1) {
                    printf("Client fd=%d disconnected while sending command.\n", i);
                    close(i);
                    FD_CLR(i, &master_set);
                    continue;
                }
                cmd_buf[len] = '\0';

                printf("Received command from fd=%d: %s\n", i, cmd_buf);

                if (strcmp(cmd_buf, "exit") == 0) {
                    printf("Client fd=%d requested exit.\n", i);
                    close(i);
                    FD_CLR(i, &master_set);
                    continue;
                }

                char out_buf[BUF_SIZE * 4];
                int out_len = execute_command(cmd_buf, out_buf, sizeof(out_buf));

                uint32_t send_len = htonl(out_len);
                if (send_all(i, &send_len, sizeof(send_len)) == -1 ||
                    send_all(i, out_buf, out_len) == -1) {
                    printf("Error sending result to fd=%d. Closing.\n", i);
                    close(i);
                    FD_CLR(i, &master_set);
                    continue;
                }
            }
        }
    }

    close(listen_fd);
    return 0;
}
