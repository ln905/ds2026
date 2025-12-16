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
#include <stdint.h>

#define PORT 12345
#define BACKLOG 10
#define BUF_SIZE 4096
#define MAX_CLIENTS FD_SETSIZE
#define MAX_FILE_SIZE (10*1024*1024)

#define CMD_WINDOW_SECONDS 5
#define CMD_WINDOW_MAX     10

typedef struct {
    int fd;
    int in_use;
    int auth_stage;
    int auth_fail;
    char username[32];
    char role[16];
    char addr_str[64];
    int port;
    char cwd[PATH_MAX];
    int cmd_count;

    // Rate limiting
    time_t window_start;
    int    window_count;
} Client;

typedef struct {
    const char *user;
    const char *pass;
    const char *role;
} Cred;

Cred credentials[] = {
    {"admin", "123456", "admin"},
    {"user",  "usth",   "user"},
};
const int NUM_CREDS = sizeof(credentials) / sizeof(credentials[0]);

Client clients[MAX_CLIENTS];
int listen_fd;
FILE *log_fp = NULL;
int total_commands_executed = 0;

/* ------------ Network: send/recv raw ------------ */
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

/* ------------ Network: send/recv message (length + payload) ------------ */
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
    if (len >= (uint32_t)max_len) {
        // message quá dài
        return -1;
    }
    if (len > 0 && recv_all(sock, buf, len) == -1) return -1;
    buf[len] = '\0';
    return (int)len;
}

/* ------------ Clients management ------------ */
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
            clients[i].role[0] = '\0';
            inet_ntop(AF_INET, &addr->sin_addr, clients[i].addr_str,
                      sizeof(clients[i].addr_str));
            clients[i].port = ntohs(addr->sin_port);
            clients[i].cmd_count = 0;

            if (!getcwd(clients[i].cwd, sizeof(clients[i].cwd))) {
                strcpy(clients[i].cwd, "/");
            }

            // init rate limiting
            clients[i].window_start = time(NULL);
            clients[i].window_count = 0;

            return &clients[i];
        }
    }
    return NULL;
}

void remove_client(Client *c) {
    if (!c || !c->in_use) return;
    printf("Closing fd=%d (%s@%s:%d)\n",
           c->fd,
           c->username[0] ? c->username : "UNKNOWN",
           c->addr_str,
           c->port);
    close(c->fd);
    c->fd = -1;
    c->in_use = 0;
}

/* ------------ Authentication check and Security ------------ */
const char* get_role_for_credentials(const char *user, const char *pass) {
    for (int i = 0; i < NUM_CREDS; i++) {
        if (strcmp(credentials[i].user, user) == 0 &&
            strcmp(credentials[i].pass, pass) == 0) {
            return credentials[i].role;
        }
    }
    return NULL;
}

/* ------------ Security: check command allowed ------------ */
int is_dangerous_for_user(const char *cmd) {
    const char *banned[] = {
        "rm ", " rm", "rm -", "shutdown", "reboot",
        "mkfs", "iptables", "poweroff", NULL
    };
    for (int i = 0; banned[i]; i++) {
        if (strstr(cmd, banned[i])) return 1;
    }
    return 0;
}

/* ------------ Security: rate limiting ------------ */
int is_rate_limited(Client *c) {
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return 0;
    }

    if (c->window_start == 0 || now - c->window_start >= CMD_WINDOW_SECONDS) {
        c->window_start = now;
        c->window_count = 0;
    }

    if (c->window_count >= CMD_WINDOW_MAX) {
        int wait = (int)(CMD_WINDOW_SECONDS - (now - c->window_start));
        if (wait < 1) wait = 1;

        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Rate limit exceeded: max %d commands / %d seconds.\n"
                 "Please wait ~%d seconds...\n",
                 CMD_WINDOW_MAX, CMD_WINDOW_SECONDS, wait);

        send_message(c->fd, msg);
        return 1;  // bị limit
    }

    c->window_count++;
    return 0;
}

/* ------------ Features: who/stats ------------ */
void build_who(char *out, size_t out_sz) {
    out[0] = '\0';
    strcat(out, "Connected clients:\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].auth_stage == 2) {
            char line[256];
            snprintf(line, sizeof(line), "  %s (role=%s) @ %s:%d, cmds=%d\n",
                     clients[i].username,
                     clients[i].role,
                     clients[i].addr_str,
                     clients[i].port,
                     clients[i].cmd_count);
            strncat(out, line, out_sz - strlen(out) - 1);
        }
    }
}

void build_stats(char *out, size_t out_sz) {
    int active = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].auth_stage == 2) active++;
    }
    snprintf(out, out_sz,
             "Server stats:\n"
             "  Active clients: %d\n"
             "  Total commands executed: %d\n",
             active, total_commands_executed);
}

/* ------------ Utility: trim tail whitespace ------------ */
void trim_end(char *s) {
    int i = (int)strlen(s) - 1;
    while (i >= 0 && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
        s[i--] = '\0';
}

/* ------------ Utility: logging ------------ */
void log_event(const char *username, const char *role, const char *addr, int port,
               const char *cwd, const char *cmd, int bytes_out) {
    if (!log_fp) return;
    time_t now = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(log_fp, "[%s] user=%s role=%s ip=%s:%d cwd=\"%s\" cmd=\"%s\" bytes_out=%d\n",
            tbuf,
            username ? username : "UNKNOWN",
            role ? role : "?",
            addr ? addr : "?",
            port,
            cwd ? cwd : "?",
            cmd ? cmd : "?",
            bytes_out);
    fflush(log_fp);
}

/* ------------ Logic: Command execution ------------ */
int execute_command(const char *cmd, char *out_buf, int max_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(out_buf, max_len, "Failed to run command: %s\n", strerror(errno));
        return (int)strlen(out_buf);
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

/* ------------ Logic: File transfer helpers ------------ */
int handle_upload_data(Client *c, const char *remote_path, long size) {
    if (size < 0 || size > MAX_FILE_SIZE) {
        send_message(c->fd, "UPLOAD_ERROR: invalid size.\n");
        return -1;
    }
    char *buf = (char *)malloc(size);
    if (!buf) {
        send_message(c->fd, "UPLOAD_ERROR: malloc failed.\n");
        return -1;
    }
    if (chdir(c->cwd) == -1) {
        free(buf);
        send_message(c->fd, "UPLOAD_ERROR: chdir failed.\n");
        return -1;
    }
    if (recv_all(c->fd, buf, (int)size) == -1) {
        free(buf);
        send_message(c->fd, "UPLOAD_ERROR: connection lost while receiving file.\n");
        return -1;
    }
    FILE *f = fopen(remote_path, "wb");
    if (!f) {
        free(buf);
        send_message(c->fd, "UPLOAD_ERROR: cannot open remote file.\n");
        return -1;
    }
    size_t written = fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);
    if (written != (size_t)size) {
        send_message(c->fd, "UPLOAD_ERROR: write error.\n");
        return -1;
    }
    char msg[256];
    snprintf(msg, sizeof(msg),
             "UPLOAD_OK: wrote %ld bytes to %s\n", size, remote_path);
    send_message(c->fd, msg);
    log_event(c->username, c->role, c->addr_str, c->port, c->cwd, "UPLOAD", (int)size);
    total_commands_executed++;
    c->cmd_count++;
    return 0;
}

int handle_download_data(Client *c, const char *remote_path) {
    if (chdir(c->cwd) == -1) {
        send_message(c->fd, "DOWNLOAD_ERR: chdir failed.\n");
        return -1;
    }
    FILE *f = fopen(remote_path, "rb");
    if (!f) {
        send_message(c->fd, "DOWNLOAD_ERR: cannot open file.\n");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > MAX_FILE_SIZE) {
        fclose(f);
        send_message(c->fd, "DOWNLOAD_ERR: invalid file size.\n");
        return -1;
    }
    char *buf = (char *)malloc(size);
    if (!buf) {
        fclose(f);
        send_message(c->fd, "DOWNLOAD_ERR: malloc failed.\n");
        return -1;
    }
    size_t rd = fread(buf, 1, size, f);
    fclose(f);
    if (rd != (size_t)size) {
        free(buf);
        send_message(c->fd, "DOWNLOAD_ERR: read error.\n");
        return -1;
    }
    char header[128];
    snprintf(header, sizeof(header), "DOWNLOAD_OK %ld", size);
    if (send_message(c->fd, header) == -1) {
        free(buf);
        return -1;
    }
    if (send_all(c->fd, buf, (int)size) == -1) {
        free(buf);
        return -1;
    }
    free(buf);
    log_event(c->username, c->role, c->addr_str, c->port, c->cwd, "DOWNLOAD", (int)size);
    total_commands_executed++;
    c->cmd_count++;
    return 0;
}

/* ------------ Controller: handle main shell command ------------ */
void handle_shell_command(Client *c, const char *cmd_in) {
    char cmd[BUF_SIZE];
    strncpy(cmd, cmd_in, sizeof(cmd));
    cmd[sizeof(cmd) - 1] = '\0';
    trim_end(cmd);

    if (strlen(cmd) == 0) {
        send_message(c->fd, "");
        return;
    }

    // Rate limit check
    if (is_rate_limited(c)) {
        log_event(c->username, c->role, c->addr_str, c->port, c->cwd, "RATE_LIMIT", 0);
        return;
    }

    // exit
    if (strcmp(cmd, "exit") == 0) {
        send_message(c->fd, "Bye.\n");
        log_event(c->username, c->role, c->addr_str, c->port, c->cwd, "exit", 4);
        remove_client(c);
        return;
    }

    // help
    if (strcmp(cmd, "help") == 0) {
        const char *help_msg =
            "Available internal commands:\n"
            "  help                       - show this help\n"
            "  who                        - list connected users\n"
            "  stats                      - show server stats (admin only)\n"
            "  cd <dir>                   - change directory\n"
            "  broadcast <msg>            - broadcast message (admin only)\n"
            "  upload <local> <remote>    - client-side, sends file to server\n"
            "  download <remote> <local>  - client-side, gets file from server\n"
            "  exit                       - disconnect\n"
            "Other text is executed as shell command on server.\n";
        send_message(c->fd, help_msg);
        log_event(c->username, c->role, c->addr_str, c->port, c->cwd, "help",
                  (int)strlen(help_msg));
        return;
    }

    // who
    if (strcmp(cmd, "who") == 0) {
        char out[BUF_SIZE * 2];
        build_who(out, sizeof(out));
        send_message(c->fd, out);
        log_event(c->username, c->role, c->addr_str, c->port, c->cwd, "who",
                  (int)strlen(out));
        return;
    }

    // stats (admin only)
    if (strcmp(cmd, "stats") == 0) {
        if (strcmp(c->role, "admin") != 0) {
            send_message(c->fd, "Permission denied: admin only.\n");
            return;
        }
        char out[BUF_SIZE];
        build_stats(out, sizeof(out));
        send_message(c->fd, out);
        log_event(c->username, c->role, c->addr_str, c->port, c->cwd, "stats",
                  (int)strlen(out));
        return;
    }

    // broadcast (admin only)
    if (strncmp(cmd, "broadcast ", 10) == 0) {
        if (strcmp(c->role, "admin") != 0) {
            send_message(c->fd, "Permission denied: admin only.\n");
            return;
        }
        const char *msg = cmd + 10;
        char full[BUF_SIZE];
        snprintf(full, sizeof(full), "[BROADCAST from %s]: %s\n", c->username, msg);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].in_use && clients[i].auth_stage == 2) {
                send_message(clients[i].fd, full);
            }
        }
        log_event(c->username, c->role, c->addr_str, c->port, c->cwd,
                  "broadcast", (int)strlen(full));
        total_commands_executed++;
        c->cmd_count++;
        return;
    }

    // UPLOAD
    if (strncmp(cmd, "UPLOAD ", 7) == 0) {
        char remote[PATH_MAX];
        long size;
        if (sscanf(cmd + 7, "%s %ld", remote, &size) != 2) {
            send_message(c->fd, "UPLOAD_ERROR: invalid header.\n");
            return;
        }
        handle_upload_data(c, remote, size);
        return;
    }

    // DOWNLOAD
    if (strncmp(cmd, "DOWNLOAD ", 9) == 0) {
        char remote[PATH_MAX];
        if (sscanf(cmd + 9, "%s", remote) != 1) {
            send_message(c->fd, "DOWNLOAD_ERR: invalid header.\n");
            return;
        }
        handle_download_data(c, remote);
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
            log_event(c->username, c->role, c->addr_str, c->port, c->cwd, cmd,
                      (int)strlen(msg));
            chdir(c->cwd);
            return;
        }
        if (!getcwd(c->cwd, sizeof(c->cwd))) {
            strcpy(c->cwd, "/");
        }
        char msg[BUF_SIZE];
        snprintf(msg, sizeof(msg), "Current directory: %s\n", c->cwd);
        send_message(c->fd, msg);
        log_event(c->username, c->role, c->addr_str, c->port, c->cwd, cmd,
                  (int)strlen(msg));
        return;
    }

    //shell
    if (strcmp(c->role, "admin") != 0 && is_dangerous_for_user(cmd)) {
        send_message(c->fd, "Permission denied: dangerous command blocked for this user.\n");
        log_event(c->username, c->role, c->addr_str, c->port, c->cwd,
                  "BLOCKED_CMD", 0);
        return;
    }

    if (chdir(c->cwd) == -1) {
        char out[BUF_SIZE];
        snprintf(out, sizeof(out),
                 "Failed to chdir to %s: %s\n", c->cwd, strerror(errno));
        send_message(c->fd, out);
        log_event(c->username, c->role, c->addr_str, c->port, c->cwd,
                  cmd, (int)strlen(out));
        return;
    }

    char out_buf[BUF_SIZE * 4];
    int out_len = execute_command(cmd, out_buf, sizeof(out_buf));
    send_message(c->fd, out_buf);
    log_event(c->username, c->role, c->addr_str, c->port, c->cwd,
              cmd, out_len);
    total_commands_executed++;
    c->cmd_count++;
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

    printf("Remote shell server MAX listening on port %d...\n", PORT);

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
                // new connection
                addr_len = sizeof(client_addr);
                int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
                if (new_fd == -1) {
                    perror("accept");
                    continue;
                }
                if (!c) {
                    printf("Too many clients, rejecting.\n");
                    close(new_fd);
                    continue;
                }

                FD_SET(new_fd, &master_set);
                if (new_fd > max_fd) max_fd = new_fd;

                printf("New connection fd=%d from %s:%d\n", new_fd, c->addr_str, c->port);

                send_message(new_fd, "Enter username: ");
            } else {
                // existing client
                Client *c = NULL;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].in_use && clients[i].fd == fd) {
                        c = &clients[i];
                        break;
                    }
                }
                if (!c) {
                    close(fd);
                    FD_CLR(fd, &master_set);
                    continue;
                }

                int len = recv_message(fd, buf, sizeof(buf));
                if (len <= 0) {
                    // disconnected
                    remove_client(c);
                    FD_CLR(fd, &master_set);
                    continue;
                }

                if (c->auth_stage < 2) {
                    if (c->auth_stage == 0) {
                        // username
                        strncpy(c->username, buf, sizeof(c->username));
                        c->username[sizeof(c->username) - 1] = '\0';
                        c->auth_stage = 1;
                        send_message(fd, "Enter password: ");
                    } else if (c->auth_stage == 1) {
                        // password
                        char password[64];
                        strncpy(password, buf, sizeof(password));
                        password[sizeof(password) - 1] = '\0';

                        const char *role = get_role_for_credentials(c->username, password);
                        if (role) {
                            strncpy(c->role, role, sizeof(c->role));
                            c->role[sizeof(c->role) - 1] = '\0';
                            c->auth_stage = 2;

                            char welcome[256];
                            snprintf(welcome, sizeof(welcome),
                                     "Authentication successful. Welcome %s (role=%s).\n"
                                     "Type 'help' for commands.\n",
                                     c->username, c->role);
                            send_message(fd, welcome);
                            log_event(c->username, c->role, c->addr_str, c->port,
                                      c->cwd, "LOGIN", (int)strlen(welcome));
                        } else {
                            c->auth_fail++;
                            if (c->auth_fail >= 3) {
                                send_message(fd,
                                             "Authentication failed too many times. Bye.\n");
                                log_event(c->username, c->role, c->addr_str, c->port,
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
                    // shell phase
                    handle_shell_command(c, buf);
                    if (!c->in_use) {
                        FD_CLR(fd, &master_set);
                    }
                }
            }
        }
    }

    if (log_fp) fclose(log_fp);
    close(listen_fd);
    return 0;
}