/* 
 * Copyright (c) onestraw
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <linux/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include "picohttpparser.h"
#include "mpool.h"
#include "khash.h"

#define DEFAULT_ADDR        "127.0.0.1"
#define DEFAULT_PORT        3128
#define MAX_CONN            2048
#define MAX_EVENTS          2048
#define BUF_SIZE            1024*10	//10k
#define POOL_MAX            26	//2^26 = 64M
#define HOST_MAX_LENGTH     128
#define DUMP_INTERVAL       30	//seconds

#define LOG_DEBUG           1
#define LOG_INFO            2

#define MAX(a, b)   ((a) > (b) ? (a):(b))
#define MIN(a, b)   ((a) < (b) ? (a):(b))
#define _perror(s)  fprintf(stderr, "[+error] %s (%d) {%s}: %s %s\n",\
        __FILE__, __LINE__, __FUNCTION__, s, strerror(errno))

typedef struct conn {
	struct sockaddr_in local;
	struct sockaddr_in remote;
	int sock;
} conn_t;

typedef struct proxy {
	conn_t conn_pc;		//proxy-client
	conn_t conn_ps;		//proxy-server
	char host[HOST_MAX_LENGTH];
	char *request;
	ssize_t request_len;
} proxy_t;

KHASH_MAP_INIT_INT(32, proxy_t *);
#define hash_t  khash_t(32)
static hash_t *h;
static hash_t *hc;		//keep connecting socket

static mpool *pool;
static int log_level = 2;
static int epfd;

static char *not_found_response =
    "HTTP/1.1 404 Not Found\n"
    "Content-type: text/html\n" "\n"
    "<html>\n"
    " <body>\n"
    "  <h1>Not Found</h1>\n"
    "  <p>The requested URL was not found on this server.</p>\n"
    " </body>\n" "</html>\n";

void proxy_run(void);
void proxy_init(void);
void proxy_exit(void);
int handle_accept_event(int, int);
int handle_epollhup_event(int, int);
int handle_epollin_event(int, int);
int handle_epollout_event(int, int);
int handle_http_request(proxy_t *, char *, size_t);
int forward_http_request(proxy_t *, char *, size_t);
int forward_http_response(proxy_t *, char *, size_t);
int put_proxy(hash_t *, proxy_t *);
proxy_t *get_proxy(hash_t *, int);
int del_proxy(hash_t *, int);
int build_proxy(int, int, char *, ssize_t);
int close_proxy(proxy_t *, int, hash_t *);
void dump_proxy(void);
static void epoll_ctl_add(int, int, uint32_t);
static void set_sockaddr(struct sockaddr_in *, const char *, unsigned int);
static int setnonblocking(int);
static int new_connection(proxy_t *, int);
static int close_socket(int, int);
static int hostname_to_ip(const char *, char *);
static char *ap_pair(struct sockaddr_in *);
static void log_debug(char *format, ...);
static void log_info(char *format, ...);
static void INThandler(int);
static void timer_handler(void);
void print_hex_ascii_line(const u_char *, int, int);
void print_payload(const u_char *, int);

int main(int argc, char *argv[])
{
	if (signal(SIGINT, SIG_IGN) != SIG_IGN) {
		signal(SIGINT, INThandler);
	}
	signal(SIGPIPE, SIG_IGN);

	proxy_init();
	proxy_run();
	return 0;
}

/*
 * proxy server
 */
void proxy_run(void)
{
	int i;
	int nfds;
	//int epfd;
	int listen_sock;
	struct sockaddr_in srv_addr;
	struct epoll_event events[MAX_EVENTS];

	listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &(int) {
		       1}, sizeof(int)) < 0) {
		_perror("setsockopt(SO_REUSEADDR)");
	}

	set_sockaddr(&srv_addr, DEFAULT_ADDR, DEFAULT_PORT);
	if (bind(listen_sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) <
	    0) {
		_perror("bind()");
		exit(1);
	}
	log_info("listen on %s:%d \n", DEFAULT_ADDR, DEFAULT_PORT);

	setnonblocking(listen_sock);
	listen(listen_sock, MAX_CONN);

	epfd = epoll_create(1);
	epoll_ctl_add(epfd, listen_sock, EPOLLIN | EPOLLET);

	for (;;) {
		nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (nfds == -1) {
			_perror("epoll_wait()");
			continue;
		}
		log_debug("epoll_wait return: %d ===\n", nfds);
		for (i = 0; i < nfds; i++) {

			timer_handler();

			if (events[i].events & EPOLLERR) {
				_perror("EPOLLERR:");

				continue;
			} else if (events[i].data.fd == listen_sock) {
				handle_accept_event(listen_sock, epfd);
			} else if (events[i].events & EPOLLIN) {
				handle_epollin_event(events[i].data.fd, epfd);
			} else if (events[i].events & EPOLLOUT) {
				handle_epollout_event(events[i].data.fd, epfd);
			} else {
				log_debug("unexpected error\n");
			}
			/* check if the connection is closing */
			if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
				handle_epollhup_event(events[i].data.fd, epfd);
			}
		}
	}
}

/*
 * create memory pool and hash table for proxy
 */
void proxy_init(void)
{
	log_debug("executing enter %s ---\n", __FUNCTION__);
	//1. allocate memory pool
	pool = mpool_init(10, POOL_MAX);
	//2. init hash talbe 
	h = kh_init(32);
	hc = kh_init(32);
}

/*
 * free memory
 */
void proxy_exit(void)
{
	log_debug("executing %s ---\n", __FUNCTION__);
	mpool_free(pool);
	kh_destroy(32, h);
	kh_destroy(32, hc);
}

/*
 * accept new connection from client
 */
int handle_accept_event(int fd, int epfd)
{
	int conn_sock;
	struct sockaddr_in cli_addr;
	socklen_t len = sizeof(struct sockaddr_in);

	conn_sock = accept(fd, (struct sockaddr *)&cli_addr, &len);

	log_debug("connected with %s\n", ap_pair(&cli_addr));

	setnonblocking(conn_sock);
	epoll_ctl_add(epfd, conn_sock,
		      EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLHUP);
	return 0;
}

/*
 * close tcp connection, unregister proxy and epoll
 */
int handle_epollhup_event(int fd, int epfd)
{
	log_debug("enter %s ---\n", __FUNCTION__);

	proxy_t *proxy = get_proxy(h, fd);
	if (proxy != NULL) {
		close_proxy(proxy, epfd, h);
	} else {
		log_debug("do not find any proxy in cache \n");
		close_socket(fd, epfd);
	}
	return 0;
}

/*
 * receive http request from client and http response from server
 * @fd: connected socket
 * @epfd: epoll fd
 */
int handle_epollin_event(int fd, int epfd)
{
	char buf[BUF_SIZE];
	ssize_t bytes_read;
	proxy_t *proxy;

	for (;;) {
		bzero(buf, sizeof(buf));
		bytes_read = read(fd, buf, sizeof(buf));
		if (bytes_read <= 0) {
			if (bytes_read == 0) {	/* read done, shutdown(fd, SHUT_RD); */
			}
			if (bytes_read < 0 && errno != EAGAIN) {
				_perror("read()");
				close_proxy(get_proxy(h, fd), epfd, h);
			}
			break;
		}
		proxy = get_proxy(h, fd);
		if (proxy != NULL) {
			log_debug("received {%zd} bytes data from %s\n",
				  bytes_read,
				  ap_pair(&(proxy->conn_ps.remote)));
			if (forward_http_response(proxy, buf, bytes_read) < 0) {
				break;
			}
		} else {
			build_proxy(fd, epfd, buf, bytes_read);
		}
	}
	return 0;
}

/*
 * mainly handle these async connect event
 */
int handle_epollout_event(int fd, int epfd)
{
	log_debug("enter %s ---\n", __FUNCTION__);
	proxy_t *p;
	p = get_proxy(hc, fd);
	if (p != NULL) {
		put_proxy(h, p);
		del_proxy(hc, fd);
		if (forward_http_request(p, p->request, p->request_len) < 0) {
			print_payload((u_char *) p->request, p->request_len);
			return write(fd, not_found_response,
				     strlen(not_found_response));
		}
	} else {
		log_debug("%s: get_proxy fail ---\n", __FUNCTION__);
	}
	return 0;
}

/*
 * parse http request and get host field
 */
int handle_http_request(proxy_t * proxy, char *buf, size_t buflen)
{
	const char *method, *path;
	int pret, minor_version;
	struct phr_header headers[100];
	size_t prevbuflen = 0, method_len, path_len, num_headers, i;

	/* parse the request */
	num_headers = sizeof(headers) / sizeof(headers[0]);
	pret =
	    phr_parse_request(buf, buflen, &method, &method_len, &path,
			      &path_len, &minor_version, headers, &num_headers,
			      prevbuflen);
	if (pret <= 0) {
		log_debug("parse request fail: %d\n", pret);
		return pret;
	}
	for (i = 0; i != num_headers; ++i) {
		if (strncasecmp(headers[i].name, "host", 4) == 0) {
			strncpy(proxy->host, headers[i].value,
				(int)headers[i].value_len);
			return 1;
		}
	}
	return pret;
}

/*
 * forward http_request of client to target server
 */
int forward_http_request(proxy_t * proxy, char *buf, size_t buflen)
{
	ssize_t nw;
	nw = write(proxy->conn_ps.sock, buf, buflen);
	if (nw < 0 && errno != EAGAIN) {
		_perror("write()");
		close_proxy(proxy, epfd, h);
		return nw;
	}
	return 0;
}

/*
 * forward http_response from server to client
 */
int forward_http_response(proxy_t * proxy, char *buf, size_t buflen)
{
	ssize_t nw;
	nw = write(proxy->conn_pc.sock, buf, buflen);
	log_debug("writing {%zd} bytes data to %s\n", nw,
		  ap_pair(&proxy->conn_pc.remote));
	if (nw == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
		_perror("write()");
		print_payload((u_char *) buf, MIN(buflen, 128));
		close_proxy(proxy, epfd, h);
		return nw;
	}
	return 0;
}

/*
 * register events of fd to epfd
 */
static void epoll_ctl_add(int epfd, int fd, uint32_t events)
{
	struct epoll_event ev;
	ev.events = events | EPOLLERR;
	ev.data.fd = fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		_perror("epoll_ctl()");
		exit(1);
	}
}

/*
 * initialize struct sockaddr_in
 */
static void set_sockaddr(struct sockaddr_in *addr, const char *ipaddr,
			 unsigned int port)
{
	bzero((char *)addr, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	inet_aton(ipaddr, &addr->sin_addr);
	addr->sin_port = htons(port);
}

/*
 * set sockfd non-block
 */
static int setnonblocking(int sockfd)
{
	if (fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFD, 0) | O_NONBLOCK) ==
	    -1) {
		return -1;
	}
	return 0;
}

/*
 * create new connection between proxy and remote server
 * ret == -1: connect error
 * ret == 0: connect success
 * ret == 1: connect attempt is in progress
 */
static int new_connection(proxy_t * proxy, int epfd)
{
	int ret;
	int sockfd;
	unsigned int port;
	char ip[16];
	char *p;
	struct sockaddr_in srv_addr;
	socklen_t len = sizeof(struct sockaddr_in);

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int) {
		       1}, sizeof(int)) < 0) {
		_perror("setsockopt(SO_REUSEADDR)");
	}
	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &(int) {
		       1}, sizeof(int)) < 0) {
		_perror("setsockopt(TCP_NODELAY)");
	}
	// set non-block and add to epoll
	setnonblocking(sockfd);
	epoll_ctl_add(epfd, sockfd,
		      EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP | EPOLLHUP);

	/*
	 * host: ip address or domain name (with port number)
	 * such as: 1.2.3.4, 1.2.3.4:8080, www.google.com
	 */
	for (p = proxy->host; *p && *p != ':'; p++) ;
	if (*p != 0) {
		port = atoi(p);
		*p = 0;
	} else {
		port = 80;
	}
	if (hostname_to_ip(proxy->host, ip) < 0) {
		goto error;
	}
	set_sockaddr(&srv_addr, ip, port);

	log_debug("%s: client=%s ===\n", __FUNCTION__,
		  ap_pair(&proxy->conn_pc.remote));
	log_debug("%s: server=%s(%s) ===\n", __FUNCTION__,
		  proxy->host, ap_pair(&srv_addr));

	ret = connect(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
	if (ret < 0 && errno != EINPROGRESS) {
		_perror("connect()");
		goto error;
	}
	proxy->conn_ps.sock = sockfd;
	proxy->conn_ps.remote = srv_addr;
	if (getsockname(sockfd, (struct sockaddr *)&proxy->conn_ps.local, &len)
	    == -1) {
		_perror("getsockname()");
		goto error;
	}
	return (ret == 0 ? 0 : 1);
 error:
	close_socket(sockfd, epfd);
	return -1;
}

/*
 * execute epoll_del and close on the sockfd
 */
static int close_socket(int sockfd, int epfd)
{
	int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, NULL);
	return (ret == 0 ? 0 : close(sockfd));
}

/*
 * put proxy_t into hash table
 * use conn_ps.sock as key
 */
int put_proxy(hash_t * h, proxy_t * proxy)
{
	int ret;
	khint_t k;
	k = kh_put(32, h, proxy->conn_ps.sock, &ret);
	if (!ret) {
		log_info("kh_put error, sockfd=%d, ret=%d\n",
			 proxy->conn_ps.sock, ret);
		close_proxy((proxy_t *) kh_val(h, k), epfd, h);
		//kh_del(32, h, proxy->conn_ps.sock);
	}
	kh_val(h, k) = proxy;
	return 0;
}

/*
 * retrieve proxy_t by key
 */
proxy_t *get_proxy(hash_t * h, int fd)
{
	khint_t k;
	k = kh_get(32, h, fd);
	if (k < kh_end(h)) {
		return (proxy_t *) kh_val(h, k);
	}
	return NULL;
}

/*
 * remove proxy from hash table
 */
int del_proxy(hash_t * h, int fd)
{
	khint_t k;
	k = kh_get(32, h, fd);
	if (k < kh_end(h)) {
		kh_del(32, h, k);
		return 0;
	}
	return -1;
}

/*
 * build proxy for client
 */
int build_proxy(int sockfd, int epfd, char *request, ssize_t request_len)
{
	int ret;
	socklen_t len = sizeof(struct sockaddr_in);
	proxy_t *proxy;

	proxy = (proxy_t *) mpool_alloc(pool, sizeof(proxy_t));
	if (proxy == NULL) {
		log_debug("%s: mpool_alloc() error\n", __FUNCTION__);
		goto error;
	}
	proxy->conn_pc.sock = sockfd;

	if (getsockname(sockfd, (struct sockaddr *)&proxy->conn_pc.local, &len)
	    == -1) {
		_perror("getsockname()");
		goto error;
	}
	if (getpeername(sockfd, (struct sockaddr *)&proxy->conn_pc.remote, &len)
	    == -1) {
		_perror("getpeername()");
		goto error;
	}
	if (handle_http_request(proxy, request, request_len) < 0) {
		goto proxy_request_error;
	}
	if ((ret = new_connection(proxy, epfd)) < 0) {
		goto proxy_request_error;
	} else if (ret == 1) {
		//connection is still in progress
		char *p = (char *)mpool_alloc(pool, request_len);
		if (p == NULL) {
			log_debug("%s: mpool_alloc() error\n", __FUNCTION__);
			goto error;
		}
		memcpy(p, request, request_len);
		proxy->request = p;
		proxy->request_len = request_len;
		put_proxy(hc, proxy);
		goto exit;
	}
	if (forward_http_request(proxy, request, request_len) < 0) {
		goto proxy_request_error;
	}
	put_proxy(h, proxy);
 exit:
	return 0;
 error:
	return -1;
 proxy_request_error:
	print_payload((u_char *) request, MIN(request_len, 128));
	return write(sockfd, not_found_response, strlen(not_found_response));
}

/*
 * close proxy, close two connections and remove proxy from table
 */
int close_proxy(proxy_t * proxy, int epfd, hash_t * h)
{
	if (proxy == NULL || h == NULL) {
		return 0;
	}
	char buf[32];
	strcpy(buf, ap_pair(&proxy->conn_pc.remote));
	log_debug("closing %-24s%-24s%-24s\n", buf,
		  ap_pair(&proxy->conn_ps.local), proxy->host);
	close_socket(proxy->conn_pc.sock, epfd);
	close_socket(proxy->conn_ps.sock, epfd);
	return del_proxy(h, proxy->conn_ps.sock);
}

/*
 * dump all proxy
 */
void dump_proxy(void)
{
	khint_t k;
	proxy_t *p;
	char buf[32];
#define FORMAT  "%-24s%-24s%-24s\n"
#define HEADER_FORMAT  "%-16s"FORMAT
#define ITEM_FORMAT  "%-16u"FORMAT
#define dump_khash(h, s)                            \
    if (kh_size(h) == 0){                           \
	    log_info("%s proxy table is empty\n", s);   \
	    return;                                     \
    }                                               \
	log_info("+------------------------"            \
		 "dump_proxy %s--------------------+\n", s);\
	log_info(HEADER_FORMAT, "key", "client",        \
            "proxy", "server");                     \
	for (k = kh_begin(h); k != kh_end(h); k++) {    \
		if (kh_exist(h, k)) {                       \
			p = (proxy_t *) kh_val(h, k);           \
		    strcpy(buf, ap_pair(&p->conn_pc.remote));\
			log_info(ITEM_FORMAT, k, buf,    \
				 ap_pair(&p->conn_ps.local), p->host);\
		}\
	}
	dump_khash(h, "established");
	dump_khash(hc, "connecting");
}

/*
 * Get ip from domain name
 * @hostname: input argument, such as: www.google.com
 * @ip: output argument, such as: 1.2.3.4
 */
static int hostname_to_ip(const char *hostname, char *ip)
{
	int i;
	struct hostent *he;
	struct in_addr **addr_list;

	if ((he = gethostbyname(hostname)) == NULL) {
		log_debug("gethostbyname('%s') error\n", hostname);
		return -1;
	}

	addr_list = (struct in_addr **)he->h_addr_list;
	for (i = 0; addr_list[i] != NULL; i++) {
		inet_ntop(AF_INET, (char *)&(*addr_list[i]), ip, 16);
		return 0;
	}

	return 1;
}

/*
 * convert sockaddr_in to <address:port>
 */
static char *ap_pair(struct sockaddr_in *addr)
{
	static char pair[32];
	char buf[16];
	inet_ntop(AF_INET, (char *)&(addr->sin_addr),
		  buf, sizeof(struct sockaddr_in));
	memset(pair, 0, sizeof(pair));
	snprintf(pair, 32, "%s:%d", buf, ntohs(addr->sin_port));
	return pair;
}

/*
 * output debug message
 */
static void log_debug(char *format, ...)
{
	if (log_level <= LOG_DEBUG) {
		va_list argList;
		va_start(argList, format);
		printf("[+debug] -- ");
		vprintf(format, argList);
		va_end(argList);
	}
}

/*
 * output info level message
 */
static void log_info(char *format, ...)
{
	if (log_level <= LOG_INFO) {
		va_list argList;
		va_start(argList, format);
		printf("[+info] -- ");
		vprintf(format, argList);
		va_end(argList);
	}
}

/*
 * handle Ctrl-C signal
 */
static void INThandler(int sig)
{
	char c;
	signal(sig, SIG_IGN);
	printf("Ouch, did you hit Ctrl-C?\n"
	       "Do you really want to quit [y/n]?");
	c = getchar();
	if (c == 'y' || c == 'Y') {
		proxy_exit();
		exit(0);
	} else {
		signal(SIGINT, INThandler);
	}
}

/*
 * call dump_proxy() every certain secords
 */
static void timer_handler(void)
{
	static time_t last_time;
	time_t now = time(NULL);
	if (difftime(now, last_time) >= DUMP_INTERVAL) {
		last_time = now;
		dump_proxy();
	}
}

/*
 * print data in rows of 16 bytes: offset   hex   ascii
 *
 * 00000   47 45 54 20 2f 20 48 54  54 50 2f 31 2e 31 0d 0a   GET / HTTP/1.1..
 *
 * refer to: http://www.tcpdump.org/sniffex.c
 */
void print_hex_ascii_line(const u_char * payload, int len, int offset)
{

	int i;
	int gap;
	const u_char *ch;

	/* offset */
	printf("%05d   ", offset);

	/* hex */
	ch = payload;
	for (i = 0; i < len; i++) {
		printf("%02x ", *ch);
		ch++;
		/* print extra space after 8th byte for visual aid */
		if (i == 7)
			printf(" ");
	}
	/* print space to handle line less than 8 bytes */
	if (len < 8)
		printf(" ");

	/* fill hex gap with spaces if not full line */
	if (len < 16) {
		gap = 16 - len;
		for (i = 0; i < gap; i++) {
			printf("   ");
		}
	}
	printf("   ");

	/* ascii (if printable) */
	ch = payload;
	for (i = 0; i < len; i++) {
		if (isprint(*ch))
			printf("%c", *ch);
		else
			printf(".");
		ch++;
	}

	printf("\n");

	return;
}

/*
 * print packet payload data (avoid printing binary data)
 *
 * refer to: http://www.tcpdump.org/sniffex.c
 */
void print_payload(const u_char * payload, int len)
{

	int len_rem = len;
	int line_width = 16;	/* number of bytes per line */
	int line_len;
	int offset = 0;		/* zero-based offset counter */
	const u_char *ch = payload;

	if (len <= 0)
		return;

	/* data fits on one line */
	if (len <= line_width) {
		print_hex_ascii_line(ch, len, offset);
		return;
	}

	/* data spans multiple lines */
	for (;;) {
		/* compute current line length */
		line_len = line_width % len_rem;
		/* print line */
		print_hex_ascii_line(ch, line_len, offset);
		/* compute total remaining */
		len_rem = len_rem - line_len;
		/* shift pointer to remaining bytes to print */
		ch = ch + line_len;
		/* add offset */
		offset = offset + line_width;
		/* check if we have line width chars or less */
		if (len_rem <= line_width) {
			/* print last line and get out */
			print_hex_ascii_line(ch, len_rem, offset);
			break;
		}
	}

	return;
}
