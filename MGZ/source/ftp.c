#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>

#include "ftp.h"

#define BUFFER_SIZE 1024
#define CONTROL_PORT 2121
#define DATA_PORT_MIN 50000
#define DATA_PORT_MAX 50100
#define MAX_CLIENTS 5

typedef struct {
    char username[32];
    char password[32];
    int is_authenticated;
    int control_fd;
    int data_fd;
    int pasv_fd;
    char current_dir[256];
    int passive_mode;
    struct sockaddr_in data_addr;
} client_info;

// 发送响应到控制连接
void send_response(int sockfd, int code, const char* message) {
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response), "%d %s\r\n", code, message);
    send(sockfd, response, strlen(response), 0);
}

// 获取本地IP地址
void get_local_ip(char* ip) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);  // DNS port
    addr.sin_addr.s_addr = inet_addr("8.8.8.8");  // Google's DNS
    
    connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    getsockname(fd, (struct sockaddr*)&addr, &addr_len);
    strcpy(ip, inet_ntoa(addr.sin_addr));
    
    close(fd);
}

// 创建PASV模式的监听socket
int create_pasv_socket(client_info* client) {
    static int last_port = DATA_PORT_MIN;
    struct sockaddr_in addr;
    int sockfd;
    int port;
    char local_ip[16];
    
    get_local_ip(local_ip);
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }
    
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    for (port = last_port + 1; port <= DATA_PORT_MAX; port++) {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        
        if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            last_port = port;
            break;
        }
    }
    
    if (port > DATA_PORT_MAX) {
        close(sockfd);
        return -1;
    }
    
    if (listen(sockfd, 1) < 0) {
        close(sockfd);
        return -1;
    }
    
    int ip[4];
    sscanf(local_ip, "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]);
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response), 
             "Entering Passive Mode (%d,%d,%d,%d,%d,%d)",
             ip[0], ip[1], ip[2], ip[3],
             port >> 8, port & 0xFF);
    send_response(client->control_fd, 227, response);
    
    client->pasv_fd = sockfd;
    client->passive_mode = 1;
    
    return sockfd;
}

// 接受PASV模式的数据连接
int accept_pasv_connection(client_info* client) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    
    client->data_fd = accept(client->pasv_fd, (struct sockaddr*)&addr, &addr_len);
    if (client->data_fd < 0) {
        return -1;
    }
    
    close(client->pasv_fd);
    client->pasv_fd = -1;
    
    return client->data_fd;
}

// 处理USER命令
void handle_user(client_info* client, const char* username) {
    strncpy(client->username, username, sizeof(client->username) - 1);
    send_response(client->control_fd, 331, "Username OK, password required");
}

// 处理PASS命令
void handle_pass(client_info* client, const char* password) {
    if (strcmp(client->username, "anonymous") == 0 || 
        (strcmp(client->username, "user") == 0 && strcmp(password, "pass") == 0)) {
        client->is_authenticated = 1;
        send_response(client->control_fd, 230, "User logged in");
    } else {
        send_response(client->control_fd, 530, "Login incorrect");
    }
}

// 处理PWD命令
void handle_pwd(client_info* client) {
    if (!client->is_authenticated) {
        send_response(client->control_fd, 530, "Not logged in");
        return;
    }
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response), "\"%s\" is current directory", client->current_dir);
    send_response(client->control_fd, 257, response);
}

// 处理PASV命令
void handle_pasv(client_info* client) {
    if (!client->is_authenticated) {
        send_response(client->control_fd, 530, "Not logged in");
        return;
    }
    
    if (client->pasv_fd > 0) {
        close(client->pasv_fd);
    }
    
    if (create_pasv_socket(client) < 0) {
        send_response(client->control_fd, 425, "Can't open passive connection");
        return;
    }
}

// 处理LIST命令
void handle_list(client_info* client) {
    if (!client->is_authenticated) {
        send_response(client->control_fd, 530, "Not logged in");
        return;
    }
    
    if (!client->passive_mode) {
        send_response(client->control_fd, 425, "Use PASV first");
        return;
    }
    
    send_response(client->control_fd, 150, "Opening ASCII mode data connection for file list");
    
    if (accept_pasv_connection(client) < 0) {
        send_response(client->control_fd, 425, "Can't open data connection");
        return;
    }
    
    DIR* dir = opendir(client->current_dir);
    if (dir == NULL) {
        send_response(client->control_fd, 550, "Failed to open directory");
        close(client->data_fd);
        client->data_fd = -1;
        client->passive_mode = 0;
        return;
    }
    
    struct dirent* entry;
    char list_buffer[BUFFER_SIZE];
    
    while ((entry = readdir(dir)) != NULL) {
        struct stat file_stat;
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", client->current_dir, entry->d_name);
        
        if (stat(full_path, &file_stat) == 0) {
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%b %d %H:%M", localtime(&file_stat.st_mtime));
            
            snprintf(list_buffer, sizeof(list_buffer),
                    "%c%s %8ld %s %s\r\n",
                    (S_ISDIR(file_stat.st_mode)) ? 'd' : '-',
                    (S_ISDIR(file_stat.st_mode)) ? "rwxr-xr-x" : "rw-r--r--",
                    file_stat.st_size,
                    time_str,
                    entry->d_name);
            
            send(client->data_fd, list_buffer, strlen(list_buffer), 0);
        }
    }
    
    closedir(dir);
    close(client->data_fd);
    client->data_fd = -1;
    client->passive_mode = 0;
    
    send_response(client->control_fd, 226, "Transfer complete");
}

// 处理RETR命令
void handle_retr(client_info* client, const char* filename) {
    if (!client->is_authenticated) {
        send_response(client->control_fd, 530, "Not logged in");
        return;
    }
    
    if (!client->passive_mode) {
        send_response(client->control_fd, 425, "Use PASV first");
        return;
    }
    
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", client->current_dir, filename);
    
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        send_response(client->control_fd, 550, "Failed to open file");
        return;
    }
    
    send_response(client->control_fd, 150, "Opening BINARY mode data connection");
    
    if (accept_pasv_connection(client) < 0) {
        send_response(client->control_fd, 425, "Can't open data connection");
        fclose(file);
        return;
    }
    
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        send(client->data_fd, buffer, bytes_read, 0);
    }
    
    fclose(file);
    close(client->data_fd);
    client->data_fd = -1;
    client->passive_mode = 0;
    
    send_response(client->control_fd, 226, "Transfer complete");
}

// 处理STOR命令
void handle_stor(client_info* client, const char* filename) {
    if (!client->is_authenticated) {
        send_response(client->control_fd, 530, "Not logged in");
        return;
    }
    
    if (!client->passive_mode) {
        send_response(client->control_fd, 425, "Use PASV first");
        return;
    }
    
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", client->current_dir, filename);
    
    FILE* file = fopen(filepath, "wb");
    if (!file) {
        send_response(client->control_fd, 550, "Failed to create file");
        return;
    }
    
    send_response(client->control_fd, 150, "Ok to send data");
    
    if (accept_pasv_connection(client) < 0) {
        send_response(client->control_fd, 425, "Can't open data connection");
        fclose(file);
        return;
    }
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = recv(client->data_fd, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytes_read, file);
    }
    
    fclose(file);
    close(client->data_fd);
    client->data_fd = -1;
    client->passive_mode = 0;
    
    send_response(client->control_fd, 226, "Transfer complete");
}

// 处理客户端连接
void handle_client(client_info* client) {
    char buffer[BUFFER_SIZE];
    char cmd[32];
    char arg[BUFFER_SIZE];
    
    send_response(client->control_fd, 220, "FTP Server Ready");
    
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        memset(cmd, 0, sizeof(cmd));
        memset(arg, 0, sizeof(arg));
        
        ssize_t n = recv(client->control_fd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            break;
        }
        
        buffer[strcspn(buffer, "\r\n")] = 0;
        sscanf(buffer, "%s %[^\n]", cmd, arg);
        
        printf("Received command: %s, arg: %s\n", cmd, arg);
        
        if (strcmp(cmd, "USER") == 0) {
            handle_user(client, arg);
        }
        else if (strcmp(cmd, "PASS") == 0) {
            handle_pass(client, arg);
        }
        else if (strcmp(cmd, "PASV") == 0) {
            handle_pasv(client);
        }
        else if (strcmp(cmd, "LIST") == 0) {
            handle_list(client);
        }
        else if (strcmp(cmd, "PWD") == 0) {
            handle_pwd(client);
        }
        else if (strcmp(cmd, "STOR") == 0) {
            handle_stor(client, arg);
        }
        else if (strcmp(cmd, "RETR") == 0) {
            handle_retr(client, arg);
        }
        else if (strcmp(cmd, "QUIT") == 0) {
            send_response(client->control_fd, 221, "Goodbye");
            break;
        }
        else if (strcmp(cmd, "SYST") == 0) {
            send_response(client->control_fd, 215, "UNIX Type: L8");
        }
        else if (strcmp(cmd, "TYPE") == 0) {
            send_response(client->control_fd, 200, "Type set to I");
        }
        else {
            send_response(client->control_fd, 500, "Unknown command");
        }
    }
    
    if (client->data_fd > 0) close(client->data_fd);
    if (client->pasv_fd > 0) close(client->pasv_fd);
    close(client->control_fd);
    free(client);
}

void ftp_init() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(CONTROL_PORT);
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    printf("FTP Server listening on port %d...\n", CONTROL_PORT);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }
        
        // 为新客户端创建信息结构
        client_info* client = (client_info*)malloc(sizeof(client_info));
        memset(client, 0, sizeof(client_info));
        client->control_fd = client_fd;
        getcwd(client->current_dir, sizeof(client->current_dir));
        
        printf("New client connected from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), 
               ntohs(client_addr.sin_port));
        
        // 创建子进程处理客户端
        pid_t pid = fork();
        if (pid == 0) {
            close(server_fd);
            handle_client(client);
            exit(0);
        } else {
            close(client_fd);
        }
    }
    
    close(server_fd);
    return 0;
}
