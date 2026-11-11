#ifndef FTP_H
#define FTP_H

/* 启动/停止内置 FTP 服务器（ftp_init 非阻塞，服务在独立线程运行） */
int ftp_init(void);
void ftpd_stop(void);
int ftpd_is_running(void);

/*
 * 网络栈引用计数。
 * FTP 运行期间会一直占用网络栈；HTTP 下载/上传通过这两个函数共享，
 * 避免重复 netInitialize/netDeinitialize。
 */
int ftp_net_acquire(void);
void ftp_net_release(void);

#endif
