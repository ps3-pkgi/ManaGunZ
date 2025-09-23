/**
 * PS3 简易 FTP 服务器 (for PSL1GHT + PS3Toolchain)
 * 麻雀虽小，五脏俱全，支持后台运行
 * 编译: ppu-gcc -o ps3_ftpd ps3_ftpd.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/select.h>
#include <netdb.h>

#define FTP_PORT    21
#define MAX_CLIENTS 8

// 全局控制
static volatile int server_running = 0;
static pthread_t server_thread;
static int server_sock = -1;

// 获取 PS3 本机 IP（用于提示用户）
void get_local_ip(char *ip_buf, size_t len) {
    struct hostent *h;
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        h = gethostbyname(hostname);
        if (h && h->h_addr_list[0]) {
            strcpy(ip_buf, inet_ntoa(*(struct in_addr*)h->h_addr_list[0]));
            return;
        }
    }
    strncpy(ip_buf, "127.0.0.1", len);
}

// 数据连接结构（主动模式 PORT）
typedef struct {
    unsigned char ip[4];
    int port;
} data_conn_t;

// 客户端会话处理
void *handle_client(void *arg) {
    int client_sock = *(int*)arg;
    free(arg); // 释放传入的堆内存

    data_conn_t data_conn = {0};

    // 发送欢迎信息
    send(client_sock, "220 PS3 FTP Server Ready\r\n", 26, 0);

    char buf[512];
    ssize_t n;

    while ((n = recv(client_sock, buf, sizeof(buf)-1, 0)) > 0) {
        buf[n] = '\0';
        char *cmd = strtok(buf, "\r\n");
        if (!cmd) continue;

        if (strncasecmp(cmd, "USER ", 5) == 0) {
            send(client_sock, "331 User OK, need password\r\n", 30, 0);
        }
        else if (strncasecmp(cmd, "PASS ", 5) == 0) {
            send(client_sock, "230 Login successful\r\n", 24, 0);
        }
        else if (strncasecmp(cmd, "PORT ", 5) == 0) {
            // 解析 PORT h1,h2,h3,h4,p1,p2
            unsigned int a,b,c,d,e,f;
            if (sscanf(cmd+5, "%u,%u,%u,%u,%u,%u", &a,&b,&c,&d,&e,&f) == 6) {
                data_conn.ip[0] = a; data_conn.ip[1] = b;
                data_conn.ip[2] = c; data_conn.ip[3] = d;
                data_conn.port = (e << 8) | f;
                send(client_sock, "200 PORT command successful\r\n", 30, 0);
            } else {
                send(client_sock, "501 Invalid PORT format\r\n", 25, 0);
            }
        }
        else if (strcasecmp(cmd, "LIST") == 0) {
            if (data_conn.port == 0) {
                send(client_sock, "425 Use PORT or PASV first\r\n", 29, 0);
                continue;
            }

            send(client_sock, "150 Opening ASCII mode data connection\r\n", 42, 0);

            // 建立数据连接
            int data_sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in data_addr = {0};
            data_addr.sin_family = AF_INET;
            data_addr.sin_port = htons(data_conn.port);
            data_addr.sin_addr.s_addr = (data_conn.ip[0] << 24) |
                                        (data_conn.ip[1] << 16) |
                                        (data_conn.ip[2] << 8)  |
                                         data_conn.ip[3];

            if (connect(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) == 0) {
                DIR *dir = opendir(".");
                struct dirent *entry;
                if (dir) {
                    while ((entry = readdir(dir)) != NULL) {
                        char line[512];
                        //snprintf(line, sizeof(line), "-rw-r--r-- 1 root root 1024 Jan 01 00:00 %s\r\n", entry->d_name);
                        send(data_sock, line, strlen(line), 0);
                    }
                    closedir(dir);
                }
                close(data_sock);
                send(client_sock, "226 Directory send OK\r\n", 24, 0);
            } else {
                send(client_sock, "425 Can't open data connection\r\n", 33, 0);
            }
        }
        else if (strncasecmp(cmd, "RETR ", 5) == 0) {
            if (data_conn.port == 0) {
                send(client_sock, "425 Use PORT or PASV first\r\n", 29, 0);
                continue;
            }

            char *filename = cmd + 5;
            FILE *fp = fopen(filename, "rb");
            if (!fp) {
                send(client_sock, "550 File not found\r\n", 20, 0);
                continue;
            }

            send(client_sock, "150 Opening BINARY mode data connection\r\n", 42, 0);

            int data_sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in data_addr = {0};
            data_addr.sin_family = AF_INET;
            data_addr.sin_port = htons(data_conn.port);
            data_addr.sin_addr.s_addr = (data_conn.ip[0] << 24) |
                                        (data_conn.ip[1] << 16) |
                                        (data_conn.ip[2] << 8)  |
                                         data_conn.ip[3];

            if (connect(data_sock, (struct sockaddr*)&data_addr, sizeof(data_addr)) == 0) {
                char chunk[1024];
                size_t bytes_read;
                while ((bytes_read = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
                    send(data_sock, chunk, bytes_read, 0);
                }
                fclose(fp);
                close(data_sock);
                send(client_sock, "226 Transfer complete\r\n", 24, 0);
            } else {
                fclose(fp);
                send(client_sock, "425 Can't open data connection\r\n", 33, 0);
            }
        }
        else if (strcasecmp(cmd, "QUIT") == 0) {
            send(client_sock, "221 Goodbye\r\n", 13, 0);
            break;
        }
        else {
            send(client_sock, "502 Command not implemented\r\n", 30, 0);
        }
    }

    close(client_sock);
    pthread_exit(NULL);
}

// 主监听循环（运行在独立线程）
void *ftp_server_loop(void *arg) {
    server_running = 1;

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        server_running = 0;
        pthread_exit(NULL);
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
        server_running = 0;
        pthread_exit(NULL);
    }

    if (listen(server_sock, MAX_CLIENTS) < 0) {
        perror("listen");
        close(server_sock);
        server_running = 0;
        pthread_exit(NULL);
    }

    char local_ip[32];
    get_local_ip(local_ip, sizeof(local_ip));

    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);

        if (client_sock < 0) {
            if (server_running) perror("accept");
            continue;
        }

        // 为每个客户端创建线程
        int *pclient = malloc(sizeof(int));
        *pclient = client_sock;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, pclient) != 0) {
            free(pclient);
            close(client_sock);
            continue;
        }
        pthread_detach(tid); // 自动回收线程资源
    }

    close(server_sock);
    server_sock = -1;
    pthread_exit(NULL);
}

// 启动 FTP 服务器（非阻塞，后台运行）
void ftp_init() {
    if (server_running) {
        return;
    }

    if (pthread_create(&server_thread, NULL, ftp_server_loop, NULL) != 0) {
        return;
    }

    return;
}

// 停止 FTP 服务器
int ftpd_stop(void) {
    if (!server_running) {
        return;
    }

    server_running = 0;

    // 唤醒 accept() 以便优雅退出
    if (server_sock >= 0) {
        close(server_sock); // 强制中断 accept
    }

    // 等待线程结束（最多5秒）
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;

    if (pthread_timedjoin_np(server_thread, NULL, &ts) != 0) {
        pthread_detach(server_thread);
    }

    return;
}

// 查询运行状态
int ftpd_is_running(void) {
    return server_running;
}
