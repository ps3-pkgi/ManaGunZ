#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cstdio>

#define PORT 21
#define BUFFER_SIZE 1024

class FTPServer {
public:
    FTPServer() : server_sock(-1) {}

    ~FTPServer() {
        if (server_sock >= 0) {
            close(server_sock);
        }
    }

    void start() {
        create_socket();
        bind_socket();
        listen_socket();
        accept_clients();
    }

private:
    int server_sock;

    void create_socket() {
        server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock < 0) {
            perror("Socket creation failed");
            exit(EXIT_FAILURE);
        }
    }

    void bind_socket() {
        struct sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(PORT);

        if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("Socket bind failed");
            exit(EXIT_FAILURE);
        }
    }

    void listen_socket() {
        if (listen(server_sock, 5) < 0) {
            perror("Socket listen failed");
            exit(EXIT_FAILURE);
        }
        std::cout << "FTP server running on port " << PORT << std::endl;
    }

    void accept_clients() {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        char buffer[BUFFER_SIZE];

        while (true) {
            int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
            if (client_sock < 0) {
                perror("Client accept failed");
                continue;
            }

            std::cout << "Client connected." << std::endl;

            // Send welcome message
            send(client_sock, "220 Simple FTP Server\r\n", 23, 0);

            // Handle commands
            while (true) {
                memset(buffer, 0, BUFFER_SIZE);
                int bytes_received = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
                if (bytes_received <= 0) {
                    std::cout << "Client disconnected." << std::endl;
                    break;
                }

                std::string command(buffer);
                command = command.substr(0, command.find("\r\n")); // Remove CRLF
                std::cout << "Command received: " << command << std::endl;

                if (command == "QUIT") {
                    send(client_sock, "221 Goodbye.\r\n", 14, 0);
                    break;
                } else if (command == "PWD") {
                    handle_pwd(client_sock);
                } else if (command.substr(0, 3) == "CWD") {
                    handle_cwd(client_sock, command.substr(4));
                } else if (command == "LIST") {
                    handle_list(client_sock);
                } else {
                    send(client_sock, "502 Command not implemented.\r\n", 30, 0);
                }
            }

            close(client_sock);
        }
    }

    void handle_pwd(int client_sock) {
        char cwd[BUFFER_SIZE];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            std::string response = "257 \"" + std::string(cwd) + "\" is the current directory.\r\n";
            send(client_sock, response.c_str(), response.size(), 0);
        } else {
            send(client_sock, "550 Failed to get current directory.\r\n", 38, 0);
        }
    }

    void handle_cwd(int client_sock, const std::string &path) {
        if (chdir(path.c_str()) == 0) {
            send(client_sock, "250 Directory successfully changed.\r\n", 38, 0);
        } else {
            send(client_sock, "550 Failed to change directory.\r\n", 33, 0);
        }
    }

    void handle_list(int client_sock) {
        DIR *dir;
        struct dirent *entry;
        struct stat file_stat;

        if ((dir = opendir(".")) == NULL) {
            send(client_sock, "550 Failed to list directory.\r\n", 31, 0);
            return;
        }

        std::string response = "150 Here comes the directory listing.\r\n";
        send(client_sock, response.c_str(), response.size(), 0);

        while ((entry = readdir(dir)) != NULL) {
            stat(entry->d_name, &file_stat);
            if (S_ISDIR(file_stat.st_mode)) {
                response = "<DIR> ";
            } else {
                response = "      ";
            }
            response += entry->d_name;
            response += "\r\n";
            send(client_sock, response.c_str(), response.size(), 0);
        }

        closedir(dir);

        response = "226 Directory send okay.\r\n";
        send(client_sock, response.c_str(), response.size(), 0);
    }
};

int ftp_init() {
    try {
        FTPServer server;
        server.start();
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
