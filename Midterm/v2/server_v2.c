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
#include <time.h>
#include <limits.h>

#define PORT 12345
#define BACKLOG 10
#define BUF_SIZE 4096
#define MAX_CLIENTS FD_SETSIZE

typedef struct {
    int fd;
    int in_use;
    int auth_stage;     // 0: need username, 1: need password, 2: done
    int auth_fail;
    char username[32];
    char addr_str[64];
    int port;
    char cwd[PATH_MAX];
} Client;

Client clients[MAX_CLIENTS];
int listen_fd;
FILE *log_fp = NULL;

/* ------------ Helper: send/recv message (length + payload) ------------ */

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
    if (len >= (uint32_t)max_len) {
        // message too long
        return -1;
    }
    if (len > 0 && recv_all(sock, buf, len) == -1) return -1;
    buf[len] = '\0';
    return (int)len;
}

/* ------------ Logging ------------ */

void log_event(const char *username, const char *addr, int port,
               const char *cwd, const char *cmd, int bytes_out) {
    if (!log_fp) return;
    time_t now = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(log_fp, "[%s] %s@%s:%d cwd=\"%s\" cmd=\"%s\" bytes_out=%d\n",
            tbuf, username ? username : "UNKNOWN",
            addr ? addr : "?", port,
            cwd ? cwd : "?", cmd ? cmd : "?", bytes_out);
    fflush(log_fp);
}

/* ------------ Command execution ------------ */

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

/* ------------ Auth check (simple) ------------ */

int check_credentials(const char *user, const char *pass) {
    // Hard-code vài account cho demo
    if ((strcmp(user, "admin") == 0 && strcmp(pass, "123456") == 0) ||
        (strcmp(user, "user")  == 0 && strcmp(pass, "usth") == 0)) {
        return 1;
    }
    return 0;
}

/* ------------ Client management ------------ */

void init_clients() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].in_use = 0;
    }
}

Client* add_client(int fd, struct sockaddr_in *addr) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].in_use) {
            clients[i].fd = fd;
            clients[i].in_use = 1;
            clients[i].auth_stage = 0;
            clients[i].auth_fail = 0;
            clients[i].username[0] = '\0';
            inet_ntop(AF_INET, &addr->sin_addr, clients[i].addr_str,
                      sizeof(clients[i].addr_str));
            clients[i].port = ntohs(addr->sin_port);

            // Lưu cwd ban đầu của server làm cwd của client
            if (!getcwd(clients[i].cwd, sizeof(clients[i].cwd))) {
                strcpy(clients[i].cwd, "/");
            }
            return &clients[i];
        }
    }
    return NULL;
}

void remove_client(Client *c) {
    if (!c || !c->in_use) return;
    printf("Closing connection fd=%d (%s@%s:%d)\n",
           c->fd,
           c->username[0] ? c->username : "UNKNOWN",
           c->addr_str, c->port);
    close(c->fd);
    c->fd = -1;
    c->in_use = 0;
}

/* ------------ Process commands after auth ------------ */

void handle_shell_command(Client *c, const char *cmd_str) {
    char out_buf[BUF_SIZE * 4];

    // Trim
    char cmd[BUF_SIZE];
    strncpy(cmd, cmd_str, sizeof(cmd));
    cmd[sizeof(cmd) - 1] = '\0';
    // remove trailing \r\n spaces
    for (int i = strlen(cmd) - 1; i >= 0; i--) {
        if (cmd[i] == '\n' || cmd[i] == '\r' || cmd[i] == ' ')
            cmd[i] = '\0';
        else break;
    }

    if (strlen(cmd) == 0) {
        send_message(c->fd, "");
        return;
    }

    // exit
    if (strcmp(cmd, "exit") == 0) {
        send_message(c->fd, "Bye.\n");
        log_event(c->username, c->addr_str, c->port, c->cwd, "exit", 4);
        remove_client(c);
        return;
    }

    // help
    if (strcmp(cmd, "help") == 0) {
        const char *help_msg =
            "Available commands:\n"
            "  help                - show this help\n"
            "  who                 - list connected users\n"
            "  cd <dir>            - change directory\n"
            "  exit                - disconnect\n"
            "Other commands are executed on server shell.\n";
        send_message(c->fd, help_msg);
        log_event(c->username, c->addr_str, c->port, c->cwd, "help",
                  strlen(help_msg));
        return;
    }

    // who
    if (strcmp(cmd, "who") == 0) {
        char tmp[BUF_SIZE];
        tmp[0] = '\0';
        strcat(tmp, "Connected clients:\n");
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use && clients[i].auth_stage == 2) {
                char line[256];
                snprintf(line, sizeof(line), "  %s@%s:%d\n",
                         clients[i].username,
                         clients[i].addr_str,
                         clients[i].port);
                strcat(tmp, line);
            }
        }
        send_message(c->fd, tmp);
        log_event(c->username, c->addr_str, c->port, c->cwd, "who",
                  strlen(tmp));
        return;
    }

    // cd
    if (strncmp(cmd, "cd ", 3) == 0) {
        const char *path = cmd + 3;
        if (chdir(path) == -1) {
            char msg[BUF_SIZE];
            snprintf(msg, sizeof(msg),
                     "cd: %s: %s\n", path, strerror(errno));
            send_message(c->fd, msg);
            log_event(c->username, c->addr_str, c->port, c->cwd, cmd,
                      strlen(msg));
            // quay về cwd cũ cho chắc
            chdir(c->cwd);
            return;
        }
        // cập nhật cwd mới
        if (!getcwd(c->cwd, sizeof(c->cwd))) {
            strcpy(c->cwd, "/");
        }
        char msg[BUF_SIZE];
        snprintf(msg, sizeof(msg), "Current directory: %s\n", c->cwd);
        send_message(c->fd, msg);
        log_event(c->username, c->addr_str, c->port, c->cwd, cmd,
                  strlen(msg));
        return;
    }

    // Lệnh shell bình thường
    // đảm bảo cwd của process = cwd client
    if (chdir(c->cwd) == -1) {
        snprintf(out_buf, sizeof(out_buf),
                 "Failed to chdir to %s: %s\n", c->cwd, strerror(errno));
        send_message(c->fd, out_buf);
        log_event(c->username, c->addr_str, c->port, c->cwd, cmd,
                  strlen(out_buf));
        return;
    }

    int out_len = execute_command(cmd, out_buf, sizeof(out_buf));
    send_message(c->fd, out_buf);
    log_event(c->username, c->addr_str, c->port, c->cwd, cmd, out_len);
}

/* ------------ Main ------------ */

int main() {
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len;
    fd_set read_fds, master_set;
    int max_fd;
    char buf[BUF_SIZE];

    log_fp = fopen("server.log", "a");
    if (!log_fp) {
        perror("fopen log");
        // không exit, chỉ là ko log được
    }

    init_clients();

    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                   sizeof(opt)) == -1) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, BACKLOG) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    printf("Remote shell server v2 listening on port %d...\n", PORT);

    FD_ZERO(&master_set);
    FD_SET(listen_fd, &master_set);
    max_fd = listen_fd;

    while (1) {
        read_fds = master_set;
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) == -1) {
            perror("select");
            exit(EXIT_FAILURE);
        }

        for (int fd = 0; fd <= max_fd; fd++) {
            if (!FD_ISSET(fd, &read_fds)) continue;

            if (fd == listen_fd) {
                /* New connection */
                addr_len = sizeof(client_addr);
                int new_fd = accept(listen_fd,
                                    (struct sockaddr *)&client_addr,
                                    &addr_len);
                if (new_fd == -1) {
                    perror("accept");
                    continue;
                }

                Client *c = add_client(new_fd, &client_addr);
                if (!c) {
                    printf("Too many clients, rejecting.\n");
                    close(new_fd);
                    continue;
                }

                FD_SET(new_fd, &master_set);
                if (new_fd > max_fd) max_fd = new_fd;

                printf("New connection fd=%d from %s:%d\n",
                       new_fd, c->addr_str, c->port);

                // Gửi prompt username
                send_message(new_fd, "Enter username: ");
            } else {
                /* Existing client */
                // tìm client struct
                Client *c = NULL;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].in_use && clients[i].fd == fd) {
                        c = &clients[i];
                        break;
                    }
                }
                if (!c) {
                    // không tìm thấy, đóng
                    close(fd);
                    FD_CLR(fd, &master_set);
                    continue;
                }

                int len = recv_message(fd, buf, sizeof(buf));
                if (len <= 0) {
                    // disconnect
                    remove_client(c);
                    FD_CLR(fd, &master_set);
                    continue;
                }

                if (c->auth_stage < 2) {
                    // đang trong phase auth
                    if (c->auth_stage == 0) {
                        // nhận username
                        strncpy(c->username, buf, sizeof(c->username));
                        c->username[sizeof(c->username) - 1] = '\0';
                        c->auth_stage = 1;
                        send_message(fd, "Enter password: ");
                    } else if (c->auth_stage == 1) {
                        // nhận password
                        char password[64];
                        strncpy(password, buf, sizeof(password));
                        password[sizeof(password) - 1] = '\0';

                        if (check_credentials(c->username, password)) {
                            c->auth_stage = 2;
                            char welcome[256];
                            snprintf(welcome, sizeof(welcome),
                                     "Authentication successful. "
                                     "Welcome %s!\nType 'help' for commands.\n",
                                     c->username);
                            send_message(fd, welcome);

                            log_event(c->username, c->addr_str, c->port,
                                      c->cwd, "LOGIN", strlen(welcome));
                        } else {
                            c->auth_fail++;
                            if (c->auth_fail >= 3) {
                                send_message(fd,
                                    "Authentication failed too many times. Bye.\n");
                                log_event(c->username, c->addr_str, c->port,
                                          c->cwd, "AUTH_FAIL", 0);
                                FD_CLR(fd, &master_set);
                                remove_client(c);
                            } else {
                                send_message(fd,
                                    "Invalid credentials. Try again.\nEnter username: ");
                                c->auth_stage = 0;
                            }
                        }
                    }
                } else {
                    // phase shell
                    handle_shell_command(c, buf);
                    // chú ý: handle_shell_command có thể đã remove_client
                    if (!c->in_use) {
                        FD_CLR(fd, &master_set);
                    }
                }
            }
        }
    }

    fclose(log_fp);
    close(listen_fd);
    return 0;
}
