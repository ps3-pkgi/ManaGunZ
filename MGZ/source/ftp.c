/**
 * PS3 极简 FTP 服务器 (适配 GCC 4.7.4 + newlib)
 * 一次只服务一个客户端，阻塞式
 * 编译: ppu-gcc -o ftpd ftp_minimal.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>

#define FTP_PORT 21

// 全局状态
static int server_sock = -1;
static volatile int server_running = 0;

// 数据连接信息（主动模式）
static unsigned int g_data_ip = 0;   // 网络字节序
static int          g_data_port = 0; // 主机字节序

// 安全发送
static void ftp_send(int sock, const char *msg) {
    if (sock >= 0 && msg) {
        send(sock, msg, strlen(msg), 0);
    }
}

// 解析 PORT 命令
static int parse_port_command(const char *cmd) {
    unsigned int a,b,c,d,e,f;
    if (sscanf(cmd, "PORT %u,%u,%u,%u,%u,%u", &a,&b,&c,&d,&e,&f) == 6) {
        g_data_ip = htonl((a << 24) | (b << 16) | (c << 8) | d);
        g_data_port = (e << 8) | f;
        return 1;
    }
    return 0;
}

// 建立数据连接
static int open_data_connection(void) {
    if (g_data_port == 0 || g_data_ip == 0) return -1;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_data_port);
    addr.sin_addr.s_addr = g_data_ip;

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }

    return s;
}

// 处理 LIST
static void handle_list(int ctrl_sock) {
    int data_sock = open_data_connection();
    if (data_sock < 0) {
        ftp_send(ctrl_sock, "425 Can't open data connection\r\n");
        return;
    }

    ftp_send(ctrl_sock, "150 Opening ASCII mode data connection\r\n");

    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            char line[256];
            snprintf(line, sizeof(line), "%s\r\n", de->d_name);
            send(data_sock, line, strlen(line), 0);
        }
        closedir(dir);
    }

    close(data_sock);
    ftp_send(ctrl_sock, "226 Directory send OK\r\n");
}

// 处理 RETR
static void handle_retr(int ctrl_sock, const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        ftp_send(ctrl_sock, "550 File not found\r\n");
        return;
    }

    int data_sock = open_data_connection();
    if (data_sock < 0) {
        fclose(fp);
        ftp_send(ctrl_sock, "425 Can't open data connection\r\n");
        return;
    }

    ftp_send(ctrl_sock, "150 Opening BINARY mode data connection\r\n");

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        send(data_sock, buf, n, 0);
    }

    fclose(fp);
    close(data_sock);
    ftp_send(ctrl_sock, "226 Transfer complete\r\n");
}

// 处理单个客户端会话
static void handle_session(int client_sock) {
    // 重置全局数据连接状态
    g_data_ip = 0;
    g_data_port = 0;

    ftp_send(client_sock, "220 PS3 FTP Server Ready\r\n");

    char buf[512];
    ssize_t len;

    while ((len = recv(client_sock, buf, sizeof(buf)-1, 0)) > 0) {
        buf[len] = '\0';

        // 去除 \r\n
        char *p = strpbrk(buf, "\r\n");
        if (p) *p = '\0';

        if (strncasecmp(buf, "USER ", 5) == 0) {
            ftp_send(client_sock, "331 OK\r\n");
        }
        else if (strncasecmp(buf, "PASS ", 5) == 0) {
            ftp_send(client_sock, "230 Login OK\r\n");
        }
        else if (strncasecmp(buf, "PORT ", 5) == 0) {
            if (parse_port_command(buf)) {
                ftp_send(client_sock, "200 PORT OK\r\n");
            } else {
                ftp_send(client_sock, "501 Invalid PORT\r\n");
            }
        }
        else if (strcmp(buf, "LIST") == 0) {
            handle_list(client_sock);
        }
        else if (strncasecmp(buf, "RETR ", 5) == 0) {
            handle_retr(client_sock, buf + 5);
        }
        else if (strcmp(buf, "QUIT") == 0) {
            ftp_send(client_sock, "221 Bye\r\n");
            break;
        }
        else {
            ftp_send(client_sock, "502 Not implemented\r\n");
        }
    }

    close(client_sock);
}

// 启动服务器（阻塞式，一次服务一个客户端）
int ftpd_start(void) {
    if (server_running) {
        return -1;
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_sock);
        return -1;
    }

    if (listen(server_sock, 1) < 0) { // backlog=1 足够
        perror("listen");
        close(server_sock);
        return -1;
    }

    server_running = 1;
    return 0;
}

// 服务主循环（在你的 UI 线程中定期调用，或开独立线程）
// 返回值：0=继续，-1=出错或停止
int ftpd_service(void) {
    if (!server_running || server_sock < 0) {
        return -1;
    }

    // 设置为非阻塞 accept
    struct timeval tv = {0, 0}; // 0秒0微秒 → 立即返回
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(server_sock, &fds);

    int ret = select(server_sock + 1, &fds, NULL, NULL, &tv);
    if (ret < 0) {
        perror("select");
        return -1;
    } else if (ret == 0) {
        // 无连接，立即返回，不阻塞
        return 0;
    }

    // 有新连接
    int client_sock = accept(server_sock, NULL, NULL);
    if (client_sock < 0) {
        perror("accept");
        return 0;
    }


    // ⚠️ 阻塞式处理该客户端，直到它断开
    handle_session(client_sock);

    // 继续等待下一个
    return 0;
}

// 停止服务器
int ftpd_stop(void) {
    if (!server_running) {
        return -1;
    }

    server_running = 0;

    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }

    return 0;
}

// 查询状态
int ftpd_is_running(void) {
    return server_running;
}

// ========================
// 示例 main（用于独立测试）
// ========================
void ftp_init() {
    if (ftpd_start() != 0) {
        return;
    }

    while (ftpd_is_running()) {
        ftpd_service(); // 非阻塞，可放在 UI 主循环中
        usleep(100000); // 休眠 100ms，避免空转
    }

    ftpd_stop();
    return;
}
