#include "server.h"
#include "../ftp.h"

/* 主函数入口 */
void ftp_init()
{    
    int sock_listen, sock_control, port, pid;
    port = 21;

    /* 创建监听套接字 */
    if ((sock_listen = socket_create(port)) < 0 )
    {
        perror("Error creating socket");
        exit(1);
    }        
    
    /* 循环接受不同的客户机请求 */
    while(1) 
    {    
        /* 监听套接字接受连接请求，得到控制套接字，用于传递控制信息 */
        if ((sock_control = socket_accept(sock_listen))    < 0 )
            break;            
        
        /* 创建子进程处理用户请求 */
        if ((pid = fork()) < 0) 
            perror("Error forking child process");
    
        /* 子进程调用 ftserve_process 函数与客户端交互 */
        else if (pid == 0)
        { 
            close(sock_listen);  // 子进程关闭父进程的监听套接字
            ftserve_process(sock_control);        
            close(sock_control); //用户请求处理完毕，关闭该套接字
            exit(0);
        }
            
        close(sock_control); // 父进程关闭子进程的控制套接字
    }

    close(sock_listen);    

    return 0;
}

/**
 * 通过数据套接字发送特定的文件
 * 控制信息交互通过控制套接字
 * 处理无效的或者不存在的文件名
 */
void ftserve_retr(int sock_control, int sock_data, char* filename)
{    
    FILE* fd = NULL;
    char data[MAXSIZE];
    size_t num_read;                                    
    fd = fopen(filename, "r"); // 打开文件

    if (!fd)
        send_response(sock_control, 550); // 发送错误码 (550 Requested action not taken)
    
    else
    {    
        send_response(sock_control, 150); // 发送 okay (150 File status okay)
        do 
        {
            num_read = fread(data, 1, MAXSIZE, fd); // 读文件内容
            if (num_read < 0) 
                printf("error in fread()\n");

            if (send(sock_data, data, num_read, 0) < 0) // 发送数据（文件内容）
                perror("error sending file\n");

        }
        while (num_read > 0);                                                    
            
        send_response(sock_control, 226); // 发送消息：226: closing conn, file transfer successful

        fclose(fd);
    }
}

/**
 * 响应请求：发送当前所在目录的目录项列表
 * 关闭数据连接
 * 错误返回 -1，正确返回 0
 */
int ftserve_list(int sock_data, int sock_control)
{
    char data[MAXSIZE];
    size_t num_read;                                    
    FILE* fd;

    int rs = system("ls -l | tail -n+2 > tmp.txt"); //利用系统调用函数 system 执行命令，并重定向到 tmp.txt 文件
    if ( rs < 0)
    {
        exit(1);
    }
    
    fd = fopen("tmp.txt", "r");    
    if (!fd) 
        exit(1); 
    
    /* 定位到文件的开始处 */
    fseek(fd, SEEK_SET, 0);

    send_response(sock_control, 1); 

    memset(data, 0, MAXSIZE);

    /* 通过数据连接，发送tmp.txt 文件的内容 */
    while ((num_read = fread(data, 1, MAXSIZE, fd)) > 0) 
    {
        if (send(sock_data, data, num_read, 0) < 0) 
            perror("err");
    
        memset(data, 0, MAXSIZE);
    }

    fclose(fd);

    send_response(sock_control, 226);    // 发送应答码 226（关闭数据连接，请求的文件操作成功）

    return 0;    
}

/**
 * 创建到客户机的一条数据连接
 * 成功返回数据连接的套接字
 * 失败返回 -1
 */
int ftserve_start_data_conn(int sock_control)
{
    char buf[1024];    
    int wait, sock_data;

    if (recv(sock_control, &wait, sizeof wait, 0) < 0 ) 
    {
        perror("Error while waiting");
        return -1;
    }

    
    struct sockaddr_in client_addr;
    socklen_t len = sizeof client_addr;
    getpeername(sock_control, (struct sockaddr*)&client_addr, &len); // 获得与控制套接字关联的外部地址（客户端地址）
    inet_ntop(AF_INET, &client_addr.sin_addr, buf, sizeof(buf));

    /* 创建到客户机的数据连接 */
    if ((sock_data = socket_connect(CLIENT_PORT_ID, buf)) < 0)
        return -1;

    return sock_data;        
}

/**
 * 用户资格认证
 * 认证成功返回 1，否则返回 0 
 */
int ftserve_check_user(char*user, char*pass)
{
    char username[MAXSIZE];
    char password[MAXSIZE];
    char *pch;
    char buf[MAXSIZE];
    char *line = NULL;
    size_t num_read;                                    
    size_t len = 0;
    FILE* fd;
    int auth = 0;
    
    fd = fopen(".auth", "r"); //打开认证文件（记录用户名和密码）
    if (fd == NULL) 
    {
        perror("file not found");
        exit(1);
    }    

    /* 读取".auth" 文件中的用户名和密码，验证用户身份的合法性 */
    while ((num_read = getline(&line, &len, fd)) != -1) 
    {
        memset(buf, 0, MAXSIZE);
        strcpy(buf, line);
        
        pch = strtok (buf," ");
        strcpy(username, pch);

        if (pch != NULL)
        {
            pch = strtok (NULL, " ");
            strcpy(password, pch);
        }

        /* 去除字符串中的空格和换行符 */
        trimstr(password, (int)strlen(password));

        if ((strcmp(user,username)==0) && (strcmp(pass,password)==0)) 
        {
            auth = 1; // 匹配成功，标志变量 auth = 1，并返回
            break;
        }        
    }
    free(line);    
    fclose(fd);    
    return auth;
}

/* 用户登录*/
int ftserve_login(int sock_control)
{    
    char buf[MAXSIZE];
    char user[MAXSIZE];
    char pass[MAXSIZE];    
    memset(user, 0, MAXSIZE);
    memset(pass, 0, MAXSIZE);
    memset(buf, 0, MAXSIZE);
    
    /* 获得客户端传来的用户名 */
    if ( (recv_data(sock_control, buf, sizeof(buf)) ) == -1) 
    {
        perror("recv error\n"); 
        exit(1);
    }    

    int i = 5;
    int n = 0;
    while (buf[i] != 0) //buf[0-4]="USER"
        user[n++] = buf[i++];
    
    /* 用户名正确，通知用户输入密码 */
    send_response(sock_control, 331);                    
    
    /* 获得客户端传来的密码 */
    memset(buf, 0, MAXSIZE);
    if ( (recv_data(sock_control, buf, sizeof(buf)) ) == -1) 
    {
        perror("recv error\n"); 
        exit(1);
    }
    
    i = 5;
    n = 0;
    while (buf[i] != 0) // buf[0 - 4] = "PASS"
        pass[n++] = buf[i++];
    
    return (ftserve_check_user(user, pass)); // 用户名和密码验证，并返回
}

/* 接收客户端的命令并响应，返回响应码 */
int ftserve_recv_cmd(int sock_control, char*cmd, char*arg)
{    
    int rc = 200;
    char buffer[MAXSIZE];
    
    memset(buffer, 0, MAXSIZE);
    memset(cmd, 0, 5);
    memset(arg, 0, MAXSIZE);
        
    /* 接受客户端的命令 */
    if ((recv_data(sock_control, buffer, sizeof(buffer)) ) == -1) 
    {
        perror("recv error\n"); 
        return -1;
    }
    
    /* 解析出用户的命令和参数 */
    strncpy(cmd, buffer, 4);
    char *tmp = buffer + 5;
    strcpy(arg, tmp);
    
    if (strcmp(cmd, "QUIT")==0) 
        rc = 221;

    else if ((strcmp(cmd, "USER") == 0) || (strcmp(cmd, "PASS") == 0) || (strcmp(cmd, "LIST") == 0) || (strcmp(cmd, "RETR") == 0))
        rc = 200;

    else 
        rc = 502; // 无效的命令

    send_response(sock_control, rc);    
    return rc;
}

/* 处理客户端请求 */
void ftserve_process(int sock_control)
{
    int sock_data;
    char cmd[5];
    char arg[MAXSIZE];

    send_response(sock_control, 220); // 发送欢迎应答码

    /* 用户认证 */
    if (ftserve_login(sock_control) == 1)  // 认证成功
        send_response(sock_control, 230);
    else 
    {
        send_response(sock_control, 430);    // 认证失败
        exit(0);
    }    
    
    /* 处理用户的请求 */
    while (1) 
    {
        /* 接收命令，并解析,获得命令和参数 */
        int rc = ftserve_recv_cmd(sock_control, cmd, arg);
        
        if ((rc < 0) || (rc == 221))  // 用户输入命令 "QUIT"
            break;
        
        if (rc == 200 ) 
        {
            /* 创建和客户端的数据连接 */
            if ((sock_data = ftserve_start_data_conn(sock_control)) < 0) 
            {
                close(sock_control);
                exit(1); 
            }

            /* 执行指令 */
            if (strcmp(cmd, "LIST")==0) 
                ftserve_list(sock_data, sock_control);
            
            else if (strcmp(cmd, "RETR")==0) 
                ftserve_retr(sock_control, sock_data, arg);

            close(sock_data);// 关闭连接
        } 
    }
}
