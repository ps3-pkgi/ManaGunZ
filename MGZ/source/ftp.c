/**
 * ManaGunZ 内置 FTP 服务器
 *
 * 特点：
 *  - 独立线程运行，不阻塞 UI
 *  - 同时支持被动模式 (PASV/EPSV) 和主动模式 (PORT)
 *  - 支持常用的浏览/下载命令：PWD/CWD/CDUP/LIST/NLST/RETR/SIZE/MDTM
 *  - 按行缓冲，正确处理 TCP 分包/粘包
 *
 * 服务器是只读的，不实现 STOR/DELE 等写操作。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/thread.h>
#include <sysmodule/sysmodule.h>
#include <net/net.h>

#include "ftp.h"

#ifndef FTP_PORT
#define FTP_PORT 21
#endif

#define FTP_PATH_MAX		1024
#define FTP_LINE_MAX		1024
#define FTP_IDLE_TIMEOUT	1800	/* 控制连接空闲超时（秒） */
#define FTP_DATA_TIMEOUT	10	/* 数据连接超时（秒） */

typedef struct {
	int ctrl;
	char cwd[FTP_PATH_MAX];
	int pasv_sock;
	struct sockaddr_in port_addr;
	int has_port;
} ftp_session_t;

static volatile int g_running = 0;
static volatile int g_client_sock = -1;
static volatile int g_data_sock = -1;
static int g_listen_sock = -1;
static sys_ppu_thread_t g_thread;
static int g_thread_started = 0;

/* 网络栈引用计数，避免 FTP 与 HTTP 下载互相 netInitialize/netDeinitialize */
static int g_net_ref = 0;

int ftp_net_acquire(void)
{
	int ret;

	if (g_net_ref > 0) {
		g_net_ref++;
		return 0;
	}

	ret = sysModuleLoad(SYSMODULE_NET);
	if (ret < 0) {
		return ret;
	}

	ret = netInitialize();
	if (ret < 0) {
		sysModuleUnload(SYSMODULE_NET);
		return ret;
	}

	g_net_ref = 1;
	return 0;
}

void ftp_net_release(void)
{
	if (g_net_ref <= 0) {
		return;
	}

	g_net_ref--;
	if (g_net_ref == 0) {
		netDeinitialize();
		sysModuleUnload(SYSMODULE_NET);
	}
}

static int send_all(int sock, const void *data, size_t len)
{
	const char *p = (const char *)data;

	while (len > 0) {
		int n = send(sock, p, len, 0);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		if (n == 0) {
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}

	return 0;
}

static void ftp_send(int sock, const char *msg)
{
	if (sock >= 0 && msg) {
		send_all(sock, msg, strlen(msg));
	}
}

static void ftp_sendf(int sock, const char *fmt, ...)
{
	char buf[FTP_PATH_MAX + 256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	ftp_send(sock, buf);
}

static void set_socket_timeout(int sock, int seconds)
{
	struct timeval tv;

	tv.tv_sec = seconds;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* ------------------------------------------------------------------ */
/* 路径处理：客户端看到的是以 / 为根的绝对路径                         */
/* ------------------------------------------------------------------ */

static void normalize_path(const char *cwd, const char *arg, char *out, size_t outsz)
{
	char combined[FTP_PATH_MAX * 2];
	char *p;
	size_t olen = 0;

	if (arg && arg[0]) {
		if (arg[0] == '/') {
			snprintf(combined, sizeof(combined), "%s", arg);
		} else {
			snprintf(combined, sizeof(combined), "%s/%s", cwd, arg);
		}
	} else {
		snprintf(combined, sizeof(combined), "%s", cwd);
	}

	out[0] = '\0';
	p = combined;

	while (*p) {
		char *start;
		size_t len;

		while (*p == '/') {
			p++;
		}
		if (!*p) {
			break;
		}

		start = p;
		while (*p && *p != '/') {
			p++;
		}
		len = (size_t)(p - start);

		if (len == 1 && start[0] == '.') {
			continue;
		}
		if (len == 2 && start[0] == '.' && start[1] == '.') {
			while (olen > 0 && out[olen - 1] != '/') {
				olen--;
			}
			if (olen > 0) {
				olen--;
			}
			out[olen] = '\0';
			continue;
		}

		if (olen + len + 2 >= outsz) {
			break;
		}

		out[olen++] = '/';
		memcpy(out + olen, start, len);
		olen += len;
		out[olen] = '\0';
	}

	if (olen == 0) {
		out[0] = '/';
		out[1] = '\0';
	}
}

/* ------------------------------------------------------------------ */
/* 数据连接                                                            */
/* ------------------------------------------------------------------ */

static void close_pasv(ftp_session_t *s)
{
	if (s->pasv_sock >= 0) {
		close(s->pasv_sock);
		s->pasv_sock = -1;
	}
}

static int enter_passive(ftp_session_t *s, int extended)
{
	int ls;
	int opt = 1;
	unsigned int port;
	struct sockaddr_in addr;

	close_pasv(s);
	s->has_port = 0;

	ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0) {
		return -1;
	}

	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = 0;

	if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(ls, 1) < 0) {
		close(ls);
		return -1;
	}

	{
		socklen_t alen = sizeof(addr);
		if (getsockname(ls, (struct sockaddr *)&addr, &alen) < 0) {
			close(ls);
			return -1;
		}
	}

	port = ntohs(addr.sin_port);
	s->pasv_sock = ls;

	if (extended) {
		ftp_sendf(s->ctrl, "229 Entering Extended Passive Mode (|||%u|)\r\n", port);
	} else {
		unsigned int ip = 0;
		struct sockaddr_in local;
		socklen_t llen = sizeof(local);

		if (getsockname(s->ctrl, (struct sockaddr *)&local, &llen) == 0) {
			ip = ntohl(local.sin_addr.s_addr);
		}
		if (ip == 0) {
			ip = 0x7F000001; /* 127.0.0.1 */
		}

		ftp_sendf(s->ctrl,
			"227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
			(ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
			(ip >> 8) & 0xFF, ip & 0xFF,
			(port >> 8) & 0xFF, port & 0xFF);
	}

	return 0;
}

static int accept_passive(ftp_session_t *s)
{
	int sock = -1;

	if (s->pasv_sock >= 0) {
		int waited = 0;

		while (g_running && waited < FTP_DATA_TIMEOUT) {
			fd_set fds;
			struct timeval tv;

			FD_ZERO(&fds);
			FD_SET(s->pasv_sock, &fds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;

			if (select(s->pasv_sock + 1, &fds, NULL, NULL, &tv) > 0) {
				sock = accept(s->pasv_sock, NULL, NULL);
				break;
			}
			waited++;
		}
		close_pasv(s);
	}

	return sock;
}

static int open_data_connection(ftp_session_t *s)
{
	int sock;

	if (s->pasv_sock >= 0) {
		sock = accept_passive(s);
		if (sock >= 0) {
			g_data_sock = sock;
			set_socket_timeout(sock, FTP_DATA_TIMEOUT);
		}
		return sock;
	}

	if (s->has_port) {
		s->has_port = 0;

		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0) {
			return -1;
		}

		if (connect(sock, (struct sockaddr *)&s->port_addr,
			    sizeof(s->port_addr)) != 0) {
			close(sock);
			return -1;
		}

		g_data_sock = sock;
		set_socket_timeout(sock, FTP_DATA_TIMEOUT);
		return sock;
	}

	return -1;
}

static void close_data(int sock)
{
	if (sock >= 0) {
		if (g_data_sock == sock) {
			g_data_sock = -1;
		}
		close(sock);
	}
}

static int parse_port_command(const char *arg, struct sockaddr_in *addr)
{
	unsigned int a, b, c, d, p1, p2;

	if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &a, &b, &c, &d, &p1, &p2) != 6) {
		return 0;
	}
	if (a > 255 || b > 255 || c > 255 || d > 255 || p1 > 255 || p2 > 255) {
		return 0;
	}

	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
	addr->sin_port = htons((p1 << 8) | p2);

	return 1;
}

/* ------------------------------------------------------------------ */
/* 目录/文件操作                                                       */
/* ------------------------------------------------------------------ */

static void mode_string(mode_t mode, char *out)
{
	out[0] = S_ISDIR(mode) ? 'd' : (S_ISLNK(mode) ? 'l' : '-');
	out[1] = (mode & S_IRUSR) ? 'r' : '-';
	out[2] = (mode & S_IWUSR) ? 'w' : '-';
	out[3] = (mode & S_IXUSR) ? 'x' : '-';
	out[4] = (mode & S_IRGRP) ? 'r' : '-';
	out[5] = (mode & S_IWGRP) ? 'w' : '-';
	out[6] = (mode & S_IXGRP) ? 'x' : '-';
	out[7] = (mode & S_IROTH) ? 'r' : '-';
	out[8] = (mode & S_IWOTH) ? 'w' : '-';
	out[9] = (mode & S_IXOTH) ? 'x' : '-';
	out[10] = '\0';
}

static void send_list_entry(int data_sock, const char *name,
			    const struct stat *st, int detailed)
{
	char perms[11];
	char datebuf[32];
	struct tm *tm;

	if (!detailed) {
		ftp_sendf(data_sock, "%s\r\n", name);
		return;
	}

	mode_string(st->st_mode, perms);

	tm = localtime(&st->st_mtime);
	if (tm) {
		static const char *months[] = {
			"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
		};
		snprintf(datebuf, sizeof(datebuf), "%s %2d %02d:%02d",
			 months[tm->tm_mon], tm->tm_mday,
			 tm->tm_hour, tm->tm_min);
	} else {
		snprintf(datebuf, sizeof(datebuf), "Jan  1 00:00");
	}

	ftp_sendf(data_sock, "%s %3d ftp      ftp      %12llu %s %s\r\n",
		  perms, 1,
		  (unsigned long long)st->st_size,
		  datebuf, name);
}

static void handle_list(ftp_session_t *s, const char *arg, int detailed)
{
	char vpath[FTP_PATH_MAX];
	char fspath[FTP_PATH_MAX];
	char child[FTP_PATH_MAX * 2];
	struct stat st;
	DIR *dir = NULL;
	const char *p = arg;
	int is_dir;
	int data_sock;
	int ok = 1;

	/* 跳过 LIST -la 这类选项 */
	while (*p == '-') {
		while (*p && *p != ' ') {
			p++;
		}
		while (*p == ' ') {
			p++;
		}
	}

	normalize_path(s->cwd, p, vpath, sizeof(vpath));
	snprintf(fspath, sizeof(fspath), "%s", vpath);

	is_dir = (stat(fspath, &st) == 0 && S_ISDIR(st.st_mode));

	if (is_dir) {
		dir = opendir(fspath);
		if (!dir) {
			ftp_send(s->ctrl, "550 Failed to open directory\r\n");
			return;
		}
	} else if (stat(fspath, &st) != 0) {
		ftp_send(s->ctrl, "550 File or directory not found\r\n");
		return;
	}

	data_sock = open_data_connection(s);
	if (data_sock < 0) {
		if (dir) {
			closedir(dir);
		}
		ftp_send(s->ctrl, "425 Failed to establish data connection\r\n");
		return;
	}

	ftp_send(s->ctrl, "150 Opening ASCII mode data connection for file list\r\n");

	if (dir) {
		struct dirent *de;

		while ((de = readdir(dir)) != NULL) {
			struct stat cst;

			if (strcmp(de->d_name, ".") == 0 ||
			    strcmp(de->d_name, "..") == 0) {
				continue;
			}

			if (strcmp(fspath, "/") == 0) {
				snprintf(child, sizeof(child), "/%s", de->d_name);
			} else {
				snprintf(child, sizeof(child), "%s/%s",
					 fspath, de->d_name);
			}

			if (stat(child, &cst) != 0) {
				continue;
			}

			send_list_entry(data_sock, de->d_name, &cst, detailed);
			if (!g_running) {
				ok = 0;
				break;
			}
		}
		closedir(dir);
	} else {
		const char *base = strrchr(vpath, '/');

		base = base ? base + 1 : vpath;
		send_list_entry(data_sock, base, &st, detailed);
	}

	close_data(data_sock);

	if (!ok) {
		ftp_send(s->ctrl, "426 Connection closed; transfer aborted\r\n");
	} else {
		ftp_send(s->ctrl, "226 Transfer complete\r\n");
	}
}

static void handle_retr(ftp_session_t *s, const char *arg)
{
	char vpath[FTP_PATH_MAX];
	char fspath[FTP_PATH_MAX];
	struct stat st;
	FILE *fp;
	char buf[4096];
	size_t n;
	int data_sock;
	int ok = 1;

	normalize_path(s->cwd, arg, vpath, sizeof(vpath));
	snprintf(fspath, sizeof(fspath), "%s", vpath);

	if (stat(fspath, &st) != 0 || S_ISDIR(st.st_mode)) {
		ftp_send(s->ctrl, "550 File not found\r\n");
		return;
	}

	fp = fopen(fspath, "rb");
	if (!fp) {
		ftp_send(s->ctrl, "550 Failed to open file\r\n");
		return;
	}

	data_sock = open_data_connection(s);
	if (data_sock < 0) {
		fclose(fp);
		ftp_send(s->ctrl, "425 Failed to establish data connection\r\n");
		return;
	}

	ftp_sendf(s->ctrl, "150 Opening BINARY mode data connection for %s (%llu bytes)\r\n",
		  fspath, (unsigned long long)st.st_size);

	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		if (send_all(data_sock, buf, n) < 0) {
			ok = 0;
			break;
		}
		if (!g_running) {
			ok = 0;
			break;
		}
	}

	fclose(fp);
	close_data(data_sock);

	if (ok) {
		ftp_send(s->ctrl, "226 Transfer complete\r\n");
	} else {
		ftp_send(s->ctrl, "426 Connection closed; transfer aborted\r\n");
	}
}

static void handle_cwd(ftp_session_t *s, const char *arg)
{
	char vpath[FTP_PATH_MAX];
	struct stat st;

	normalize_path(s->cwd, arg, vpath, sizeof(vpath));

	if (strlen(vpath) >= sizeof(s->cwd)) {
		ftp_send(s->ctrl, "550 Directory name too long\r\n");
		return;
	}

	if (stat(vpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
		ftp_send(s->ctrl, "550 Failed to change directory\r\n");
		return;
	}

	snprintf(s->cwd, sizeof(s->cwd), "%s", vpath);
	ftp_send(s->ctrl, "250 Directory successfully changed\r\n");
}

static void handle_size(ftp_session_t *s, const char *arg)
{
	char vpath[FTP_PATH_MAX];
	struct stat st;

	normalize_path(s->cwd, arg, vpath, sizeof(vpath));

	if (stat(vpath, &st) != 0 || S_ISDIR(st.st_mode)) {
		ftp_send(s->ctrl, "550 Could not get file size\r\n");
		return;
	}

	ftp_sendf(s->ctrl, "213 %llu\r\n", (unsigned long long)st.st_size);
}

static void handle_mdtm(ftp_session_t *s, const char *arg)
{
	char vpath[FTP_PATH_MAX];
	struct stat st;
	struct tm *tm;

	normalize_path(s->cwd, arg, vpath, sizeof(vpath));

	if (stat(vpath, &st) != 0) {
		ftp_send(s->ctrl, "550 File not found\r\n");
		return;
	}

	tm = gmtime(&st.st_mtime);
	if (!tm) {
		ftp_send(s->ctrl, "550 Failed to get file time\r\n");
		return;
	}

	ftp_sendf(s->ctrl, "213 %04d%02d%02d%02d%02d%02d\r\n",
		  tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		  tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* ------------------------------------------------------------------ */
/* 命令解析                                                            */
/* ------------------------------------------------------------------ */

/* 返回 1 表示关闭会话 */
static int process_command(ftp_session_t *s, char *line)
{
	char *cmd = line;
	char *arg;

	while (*line && *line != ' ') {
		line++;
	}
	if (*line) {
		*line++ = '\0';
		while (*line == ' ') {
			line++;
		}
	}
	arg = line;

	if (strcasecmp(cmd, "USER") == 0) {
		ftp_send(s->ctrl, "331 Please specify the password\r\n");
	} else if (strcasecmp(cmd, "PASS") == 0) {
		ftp_send(s->ctrl, "230 Login successful\r\n");
	} else if (strcasecmp(cmd, "ACCT") == 0) {
		ftp_send(s->ctrl, "230 Login successful\r\n");
	} else if (strcasecmp(cmd, "SYST") == 0) {
		ftp_send(s->ctrl, "215 UNIX Type: L8\r\n");
	} else if (strcasecmp(cmd, "FEAT") == 0) {
		ftp_send(s->ctrl,
			"211-Features:\r\n"
			" EPSV\r\n"
			" MDTM\r\n"
			" PASV\r\n"
			" SIZE\r\n"
			" UTF8\r\n"
			"211 End\r\n");
	} else if (strcasecmp(cmd, "PWD") == 0 ||
		   strcasecmp(cmd, "XPWD") == 0) {
		ftp_sendf(s->ctrl, "257 \"%s\" is the current directory\r\n", s->cwd);
	} else if (strcasecmp(cmd, "CWD") == 0 ||
		   strcasecmp(cmd, "XCWD") == 0) {
		handle_cwd(s, arg);
	} else if (strcasecmp(cmd, "CDUP") == 0 ||
		   strcasecmp(cmd, "XCUP") == 0) {
		handle_cwd(s, "..");
	} else if (strcasecmp(cmd, "TYPE") == 0) {
		ftp_send(s->ctrl, "200 Switching to Binary mode\r\n");
	} else if (strcasecmp(cmd, "MODE") == 0) {
		ftp_send(s->ctrl, "200 Mode set to S\r\n");
	} else if (strcasecmp(cmd, "STRU") == 0) {
		ftp_send(s->ctrl, "200 Structure set to F\r\n");
	} else if (strcasecmp(cmd, "PASV") == 0) {
		if (enter_passive(s, 0) != 0) {
			ftp_send(s->ctrl, "425 Failed to enter passive mode\r\n");
		}
	} else if (strcasecmp(cmd, "EPSV") == 0) {
		if (enter_passive(s, 1) != 0) {
			ftp_send(s->ctrl, "425 Failed to enter passive mode\r\n");
		}
	} else if (strcasecmp(cmd, "PORT") == 0) {
		struct sockaddr_in addr;

		close_pasv(s);
		if (parse_port_command(arg, &addr)) {
			s->port_addr = addr;
			s->has_port = 1;
			ftp_send(s->ctrl, "200 PORT command successful\r\n");
		} else {
			s->has_port = 0;
			ftp_send(s->ctrl, "501 Invalid PORT command\r\n");
		}
	} else if (strcasecmp(cmd, "LIST") == 0) {
		handle_list(s, arg, 1);
	} else if (strcasecmp(cmd, "NLST") == 0) {
		handle_list(s, arg, 0);
	} else if (strcasecmp(cmd, "RETR") == 0) {
		if (arg[0]) {
			handle_retr(s, arg);
		} else {
			ftp_send(s->ctrl, "501 Missing file name\r\n");
		}
	} else if (strcasecmp(cmd, "SIZE") == 0) {
		if (arg[0]) {
			handle_size(s, arg);
		} else {
			ftp_send(s->ctrl, "501 Missing file name\r\n");
		}
	} else if (strcasecmp(cmd, "MDTM") == 0) {
		if (arg[0]) {
			handle_mdtm(s, arg);
		} else {
			ftp_send(s->ctrl, "501 Missing file name\r\n");
		}
	} else if (strcasecmp(cmd, "OPTS") == 0) {
		if (strncasecmp(arg, "UTF8", 4) == 0) {
			ftp_send(s->ctrl, "200 Always in UTF8 mode\r\n");
		} else {
			ftp_send(s->ctrl, "501 Option not understood\r\n");
		}
	} else if (strcasecmp(cmd, "NOOP") == 0) {
		ftp_send(s->ctrl, "200 NOOP ok\r\n");
	} else if (strcasecmp(cmd, "ABOR") == 0) {
		ftp_send(s->ctrl, "226 Abort successful\r\n");
	} else if (strcasecmp(cmd, "REST") == 0) {
		ftp_send(s->ctrl, "502 REST not implemented\r\n");
	} else if (strcasecmp(cmd, "HELP") == 0) {
		ftp_send(s->ctrl, "214-The following commands are recognized\r\n"
				  " USER PASS SYST FEAT PWD CWD CDUP TYPE PASV EPSV PORT\r\n"
				  " LIST NLST RETR SIZE MDTM OPTS NOOP ABOR QUIT\r\n"
				  "214 Help OK\r\n");
	} else if (strcasecmp(cmd, "QUIT") == 0) {
		ftp_send(s->ctrl, "221 Goodbye\r\n");
		return 1;
	} else if (cmd[0] == '\0') {
		ftp_send(s->ctrl, "500 Unknown command\r\n");
	} else {
		ftp_send(s->ctrl, "502 Command not implemented\r\n");
	}

	return 0;
}

static void handle_session(int ctrl)
{
	ftp_session_t s;
	char buf[1024];
	char line[FTP_LINE_MAX];
	size_t line_len = 0;
	int discard_line = 0;
	time_t last_active;

	memset(&s, 0, sizeof(s));
	s.ctrl = ctrl;
	s.pasv_sock = -1;
	snprintf(s.cwd, sizeof(s.cwd), "/");

	g_client_sock = ctrl;
	set_socket_timeout(ctrl, 1);
	ftp_send(ctrl, "220 ManaGunZ FTP Server Ready\r\n");

	last_active = time(NULL);

	while (g_running) {
		int n;
		int i;

		n = recv(ctrl, buf, sizeof(buf), 0);
		if (n == 0) {
			break;
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN
#ifdef EWOULDBLOCK
			    || errno == EWOULDBLOCK
#endif
			    ) {
				if (time(NULL) - last_active > FTP_IDLE_TIMEOUT) {
					ftp_send(ctrl, "421 Idle timeout\r\n");
					break;
				}
				continue;
			}
			break;
		}

		last_active = time(NULL);

		for (i = 0; i < n; i++) {
			char c = buf[i];

			if (c == '\n') {
				if (discard_line) {
					discard_line = 0;
					line_len = 0;
					ftp_send(ctrl, "500 Line too long\r\n");
					continue;
				}

				if (line_len > 0 && line[line_len - 1] == '\r') {
					line_len--;
				}
				line[line_len] = '\0';

				if (line_len > 0) {
					if (process_command(&s, line)) {
						goto done;
					}
				}
				line_len = 0;
			} else if (c != '\r') {
				if (discard_line) {
					continue;
				}
				if (line_len < sizeof(line) - 1) {
					line[line_len++] = c;
				} else {
					discard_line = 1;
					line_len = 0;
				}
			}
		}
	}

done:
	close_pasv(&s);
	g_client_sock = -1;
	close(ctrl);
}

static void ftp_thread(void *unused)
{
	(void)unused;

	while (g_running) {
		fd_set fds;
		struct timeval tv;
		int ret;

		FD_ZERO(&fds);
		FD_SET(g_listen_sock, &fds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		ret = select(g_listen_sock + 1, &fds, NULL, NULL, &tv);
		if (!g_running) {
			break;
		}
		if (ret <= 0) {
			continue;
		}

		{
			int client = accept(g_listen_sock, NULL, NULL);
			if (client >= 0) {
				handle_session(client);
			}
		}
	}

	sysThreadExit(0);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

int ftp_init(void)
{
	int sock;
	int opt = 1;
	struct sockaddr_in addr;
	int ret;

	if (g_running) {
		return 0;
	}

	ret = ftp_net_acquire();
	if (ret < 0) {
		return ret;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		ftp_net_release();
		return -1;
	}

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(FTP_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(sock, 1) < 0) {
		close(sock);
		ftp_net_release();
		return -1;
	}

	g_listen_sock = sock;
	g_running = 1;

	ret = sysThreadCreate(&g_thread, ftp_thread, NULL, 1000, 0x8000,
			      THREAD_JOINABLE, "ManaGunZ_FTP");
	if (ret != 0) {
		g_running = 0;
		close(g_listen_sock);
		g_listen_sock = -1;
		ftp_net_release();
		return ret;
	}

	g_thread_started = 1;

	return 0;
}

void ftpd_stop(void)
{
	uint64_t ret;

	if (!g_running && !g_thread_started) {
		return;
	}

	g_running = 0;

	/* 唤醒可能阻塞在 recv/send 上的会话线程 */
	if (g_client_sock >= 0) {
		shutdown(g_client_sock, SHUT_RDWR);
	}
	if (g_data_sock >= 0) {
		shutdown(g_data_sock, SHUT_RDWR);
	}

	if (g_thread_started) {
		sysThreadJoin(g_thread, &ret);
		g_thread_started = 0;
	}

	if (g_listen_sock >= 0) {
		close(g_listen_sock);
		g_listen_sock = -1;
	}

	ftp_net_release();
}

int ftpd_is_running(void)
{
	return g_running;
}
