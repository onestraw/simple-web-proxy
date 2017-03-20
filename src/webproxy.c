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
#define BUF_SIZE            1024*10     /* 10k */
#define POOL_MAX            26          /* 2^26 = 64M */
#define HOST_MAX_LENGTH     128
#define DUMP_INTERVAL       30          /* seconds */
#define FORMAT              "%-24s%-24s%-24s\n"
#define HEADER_FORMAT       "%-16s"FORMAT
#define ITEM_FORMAT         "%-16u"FORMAT

#define LOG_DEBUG           1
#define LOG_INFO            2

#define MAX(a, b)   ((a) > (b) ? (a):(b))
#define MIN(a, b)   ((a) < (b) ? (a):(b))

static void log_core(const char *prompt, const char *format, ...);
#define log_debug(...)  \
    if (log_level <= LOG_DEBUG) log_core("debug", __VA_ARGS__)
#define log_info(...)  \
    if (log_level <= LOG_INFO) log_core("info", __VA_ARGS__)

#define log_error(s)  fprintf(stderr, "[+error] %s (%d) {%s}: %s %s\n",\
        __FILE__, __LINE__, __FUNCTION__, s, strerror(errno))

typedef struct conn {
    struct  sockaddr_in local;
    struct  sockaddr_in remote;
    int     sock;
} conn_t;

typedef struct session {
    conn_t  conn_pc;                /* proxy-client */
    conn_t  conn_ps;                /* proxy-server */
    char    host[HOST_MAX_LENGTH];
    char   *request;
    ssize_t request_len;
} session_t;

KHASH_MAP_INIT_INT(32, session_t *);
#define hash_t  khash_t(32)

typedef struct {
    int          epfd;
    mpool       *pool;
    hash_t      *h;             /* keep established socket */
    hash_t      *hc;            /* keep connecting socket */
    int         (*put)(hash_t *, session_t *);
    session_t*  (*get)(hash_t *, int);
    int         (*del)(hash_t *, int);
    session_t*  (*create)(mpool *, int);
    int         (*close)(session_t *, int);
    void        (*dump)(hash_t *, const char *);
} session_manager_t;


static int log_level = 2;
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
int handle_accept_event(int);
int handle_epollhup_event(int);
int handle_epollin_event(int);
int handle_epollout_event(int);
int parse_http_request(session_t *, char *, size_t);
int cache_http_request(mpool *, session_t *, char *, ssize_t);
int forward_http_request(session_t *, char *, size_t);
int forward_http_response(session_t *, char *, size_t);
int put_session(hash_t *, session_t *);
session_t *get_session(hash_t *, int);
int del_session(hash_t *, int);
session_t *create_session(mpool *, int);
int close_session(session_t *, int);
void dump_session(hash_t *, const char *);
static void epoll_ctl_add(int, int, uint32_t);
static void set_sockaddr(struct sockaddr_in *, const char *, unsigned int);
static int setnonblocking(int);
static int new_connection(session_t *, int);
static int close_socket(int, int);
static int hostname_to_ip(const char *, char *);
static char *sockaddr_to_str(struct sockaddr_in *);
static void INThandler(int);
static void timer_handler(void);
void print_hex_ascii_line(const u_char *, int, int);
void print_payload(const u_char *, int);


static session_manager_t sm = {
    .put    = put_session,
    .get    = get_session,
    .del    = del_session,
    .dump   = dump_session,
    .create = create_session,
    .close  = close_session,
};


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


void proxy_init(void)
{
    sm.epfd = epoll_create(1);
    sm.pool = mpool_init(10, POOL_MAX);
    sm.h = kh_init(32);
    sm.hc = kh_init(32);
}


void proxy_run(void)
{
    int                 i;
    int                 nfds;
    int                 listen_sock;
    struct sockaddr_in  srv_addr;
    struct epoll_event  events[MAX_EVENTS];

    listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
                   &(int) {1}, sizeof(int)) < 0)
    {
        log_error("setsockopt(SO_REUSEADDR)");
    }

    set_sockaddr(&srv_addr, DEFAULT_ADDR, DEFAULT_PORT);
    if (bind(listen_sock, (struct sockaddr *)&srv_addr,
             sizeof(srv_addr)) < 0)
    {
        log_error("bind()");
        exit(1);
    }

    log_info("listen on %s:%d \n", DEFAULT_ADDR, DEFAULT_PORT);
    setnonblocking(listen_sock);
    listen(listen_sock, MAX_CONN);

    epoll_ctl_add(sm.epfd, listen_sock, EPOLLIN | EPOLLET);

    for (;;) {
        nfds = epoll_wait(sm.epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            log_error("epoll_wait()");
            continue;
        }
        log_debug("epoll_wait return: %d\n", nfds);
        for (i = 0; i < nfds; i++) {

            timer_handler();

            if (events[i].data.fd == listen_sock) {
                handle_accept_event(listen_sock);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                handle_epollin_event(events[i].data.fd);
                continue;
            }

            if (events[i].events & EPOLLOUT) {
                handle_epollout_event(events[i].data.fd);
                continue;
            }

            if (events[i].events & EPOLLERR) {
                log_error("EPOLLERR:");
                continue;
            }

            /* check if the connection is closing */
            if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
                handle_epollhup_event(events[i].data.fd);
            }
        }
    }
}


void proxy_exit(void)
{
    mpool_free(sm.pool);
    kh_destroy(32, sm.h);
    kh_destroy(32, sm.hc);
}


/*
 * accept new connection from client
 */
int handle_accept_event(int fd)
{
    int                 conn_sock;
    socklen_t           len;
    struct sockaddr_in  cli_addr;

    len = sizeof(struct sockaddr_in);
    conn_sock = accept(fd, (struct sockaddr *)&cli_addr, &len);

    log_debug("connected with %s\n", sockaddr_to_str(&cli_addr));

    setnonblocking(conn_sock);
    epoll_ctl_add(sm.epfd, conn_sock, EPOLLIN | EPOLLOUT |
                  EPOLLET | EPOLLRDHUP | EPOLLHUP);
    return 0;
}


/*
 * close tcp connection, unregister session and epoll
 */
int handle_epollhup_event(int fd)
{
    log_debug("enter %s ---\n", __FUNCTION__);

    session_t *session = sm.get(sm.h, fd);
    if (session != NULL) {
        sm.close(session, sm.epfd);
        sm.del(sm.h, session->conn_ps.sock);
    } else {
        log_debug("do not find any session in cache \n");
        close_socket(fd, sm.epfd);
    }
    return 0;
}


/*
 * receive http request from client and http response from server
 * @fd: connected socket
 * @epfd: epoll fd
 */
int handle_epollin_event(int fd)
{
    int         ret;
    char        buf[BUF_SIZE];
    ssize_t     bytes_read;
    session_t  *session;

    for (;;) {
        bzero(buf, sizeof(buf));
        bytes_read = read(fd, buf, sizeof(buf));
        if (bytes_read <= 0) {
            if (bytes_read == 0) {    /* read done, shutdown(fd, SHUT_RD); */
            }
            if (bytes_read < 0 && errno != EAGAIN) {
                log_error("read()");
                session = sm.get(sm.h, fd);
                sm.close(session, sm.epfd);
                sm.del(sm.h, session->conn_ps.sock);
            }
            break;
        }

        session = sm.get(sm.h, fd);
        if (session == NULL) {
            session = sm.create(sm.pool, fd);
            if (session == NULL) {
                goto proxy_request_error;
            }

            if (parse_http_request(session, buf, bytes_read) < 0) {
                goto proxy_request_error;
            }

            if ((ret = new_connection(session, sm.epfd)) < 0) {
                goto proxy_request_error;
            } else if (ret == 1) {
                /* connection is still in progress */
                cache_http_request(sm.pool, session, buf, bytes_read);
                put_session(sm.hc, session);
            } else {
                forward_http_request(session, buf, bytes_read);
                put_session(sm.h, session);
            }
            continue;

         proxy_request_error:
            print_payload((u_char *) buf, MIN(bytes_read, 128));
            ret = write(fd, not_found_response, strlen(not_found_response));
        } else {
            log_debug("received {%zd} bytes data from %s\n",
                      bytes_read, sockaddr_to_str(&(session->conn_ps.remote)));
            if (forward_http_response(session, buf, bytes_read) < 0) {
                break;
            }
        }
    }
    return 0;
}


/*
 * mainly handle these async connect event
 */
int handle_epollout_event(int fd)
{
    log_debug("enter %s ---\n", __FUNCTION__);
    session_t   *s;

    s = sm.get(sm.hc, fd);
    if (s != NULL) {
        sm.put(sm.h, s);
        sm.del(sm.hc, fd);
        if (forward_http_request(s, s->request, s->request_len) < 0) {
            print_payload((u_char *) s->request, s->request_len);
            return write(fd, not_found_response, strlen(not_found_response));
        }
    } else {
        log_debug("%s: get_session fail ---\n", __FUNCTION__);
    }
    return 0;
}


int parse_http_request(session_t *session, char *buf, size_t buflen)
{
    int                 pret, minor_version;
    size_t              prevbuflen = 0, method_len, path_len, num_headers, i;
    const char         *method, *path;
    struct phr_header   headers[100];

    num_headers = sizeof(headers) / sizeof(headers[0]);
    pret = phr_parse_request(buf, buflen, &method, &method_len, &path,
                  &path_len, &minor_version, headers, &num_headers, prevbuflen);
    if (pret <= 0) {
        log_debug("parse request fail: %d\n", pret);
        return pret;
    }
    for (i = 0; i != num_headers; ++i) {
        if (strncasecmp(headers[i].name, "host", 4) == 0) {
            strncpy(session->host, headers[i].value,
                    (int)headers[i].value_len);
            return 1;
        }
    }
    return pret;
}


int cache_http_request(mpool *pool, session_t *s,
        char *request, ssize_t request_len)
{
    char    *buf;

    buf = (char *) mpool_alloc(pool, request_len);
    if (buf == NULL) {
        log_error("mpool_alloc() error\n");
        return -1;
    }

    memcpy(buf, request, request_len);
    s->request = buf;
    s->request_len = request_len;

    return 0;
}


int forward_http_request(session_t *session, char *buf, size_t buflen)
{
    ssize_t    nw;

    nw = write(session->conn_ps.sock, buf, buflen);
    if (nw < 0 && errno != EAGAIN) {
        log_error("write()");
        sm.close(session, sm.epfd);
        sm.del(sm.h, session->conn_ps.sock);
        return nw;
    }

    return 0;
}


int forward_http_response(session_t *session, char *buf, size_t buflen)
{
    ssize_t    nw;

    nw = write(session->conn_pc.sock, buf, buflen);
    log_debug("writing {%zd} bytes data to %s\n", nw,
              sockaddr_to_str(&session->conn_pc.remote));
    if (nw == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        log_error("write()");
        print_payload((u_char *) buf, MIN(buflen, 128));
        sm.close(session, sm.epfd);
        sm.del(sm.h, session->conn_ps.sock);
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
        log_error("epoll_ctl()");
        exit(1);
    }
}


static void set_sockaddr(struct sockaddr_in *addr, const char *ipaddr,
             unsigned int port)
{
    bzero((char *)addr, sizeof(struct sockaddr_in));
    addr->sin_family = AF_INET;
    inet_aton(ipaddr, &addr->sin_addr);
    addr->sin_port = htons(port);
}


static int setnonblocking(int sockfd)
{
    if (fcntl(sockfd, F_SETFL,
              fcntl(sockfd, F_GETFD, 0) | O_NONBLOCK) == -1)
    {
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
static int new_connection(session_t *session, int epfd)
{
    int                 ret;
    int                 sockfd;
    unsigned int        port;
    char                ip[16];
    char               *p;
    struct sockaddr_in  srv_addr;
    socklen_t           len = sizeof(struct sockaddr_in);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
                   &(int) {1}, sizeof(int)) < 0)
    {
        log_error("setsockopt(SO_REUSEADDR)");
    }

    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY,
                   &(int) {1}, sizeof(int)) < 0)
    {
        log_error("setsockopt(TCP_NODELAY)");
    }
    /* set non-block and add to epoll */
    setnonblocking(sockfd);
    epoll_ctl_add(epfd, sockfd, EPOLLIN | EPOLLOUT |
                  EPOLLET | EPOLLRDHUP | EPOLLHUP);

    /*
     * host: ip address or domain name (with port number)
     * such as: 1.2.3.4, 1.2.3.4:8080, www.google.com
     */
    for (p = session->host; *p && *p != ':'; p++) ;

    if (*p != 0) {
        port = atoi(p);
        *p = 0;
    } else {
        port = 80;
    }

    if (hostname_to_ip(session->host, ip) < 0) {
        goto error;
    }

    set_sockaddr(&srv_addr, ip, port);

    log_debug("%s: client=%s ===\n", __FUNCTION__,
              sockaddr_to_str(&session->conn_pc.remote));
    log_debug("%s: server=%s(%s) ===\n", __FUNCTION__,
              session->host, sockaddr_to_str(&srv_addr));

    ret = connect(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        log_error("connect()");
        goto error;
    }

    session->conn_ps.sock = sockfd;
    session->conn_ps.remote = srv_addr;
    if (getsockname(sockfd, (struct sockaddr *)&session->conn_ps.local, &len)
        == -1) {
        log_error("getsockname()");
        goto error;
    }
    return (ret == 0 ? 0 : 1);

 error:
    close_socket(sockfd, epfd);
    return -1;
}


static int close_socket(int sockfd, int epfd)
{
    int ret = epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, NULL);
    return (ret == 0 ? close(sockfd) : -1);
}


/*
 * put session_t into hash table with conn_ps.sock as key
 */
int put_session(hash_t *h, session_t *s)
{
    int         ret;
    khint_t     k;

    k = kh_put(32, h, s->conn_ps.sock, &ret);
    if (!ret) {
        log_info("kh_put error, sockfd=%d, ret=%d\n",
                 s->conn_ps.sock, ret);
        return -1;
        //close_session((session_t *) kh_val(h, k), epfd, h);
        //kh_del(32, h, s->conn_ps.sock);
    }
    kh_val(h, k) = s;
    return 0;
}


/*
 * retrieve session_t by key
 */
session_t *get_session(hash_t *h, int fd)
{
    khint_t k;
    k = kh_get(32, h, fd);
    if (k < kh_end(h)) {
        return (session_t *) kh_val(h, k);
    }
    return NULL;
}


int del_session(hash_t *h, int fd)
{
    khint_t k;
    k = kh_get(32, h, fd);
    if (k < kh_end(h)) {
        kh_del(32, h, k);
        return 0;
    }
    return -1;
}


session_t *create_session(mpool *pool, int sockfd)
{
    session_t  *s;
    socklen_t   len = sizeof(struct sockaddr_in);

    s = (session_t *) mpool_alloc(pool, sizeof(session_t));
    if (s == NULL) {
        log_debug("%s: mpool_alloc() error\n", __FUNCTION__);
        return NULL;
    }

    s->conn_pc.sock = sockfd;

    if (getsockname(sockfd, (struct sockaddr *)&s->conn_pc.local, &len)
        == -1) {
        log_error("getsockname()");
        return NULL;
    }

    if (getpeername(sockfd, (struct sockaddr *)&s->conn_pc.remote, &len)
        == -1) {
        log_error("getpeername()");
        return NULL;
    }

    return s;
}


int close_session(session_t *s, int epfd)
{
    char    buf[32];

    if (s == NULL) {
        return 0;
    }

    strcpy(buf, sockaddr_to_str(&s->conn_pc.remote));
    log_debug("closing %-24s%-24s%-24s\n", buf,
              sockaddr_to_str(&s->conn_ps.local), s->host);
    close_socket(s->conn_pc.sock, epfd);
    close_socket(s->conn_ps.sock, epfd);

    return 0;
}


void dump_session(hash_t *table, const char *table_name)
{
    char        buf[32];
    khint_t     k;
    session_t  *s;

    if (kh_size(table) == 0){
        log_info("%s session table is empty\n", table_name);
        return;
    }

    log_info("+------------------------dump_session"
             " %s--------------------+\n", table_name);
    log_info(HEADER_FORMAT, "key", "client", "proxy", "server");

    for (k = kh_begin(table); k != kh_end(table); k++) {
        if (kh_exist(table, k)) {
            s = (session_t *) kh_val(table, k);
            strcpy(buf, sockaddr_to_str(&s->conn_pc.remote));
            log_info(ITEM_FORMAT, k, buf,
                     sockaddr_to_str(&s->conn_ps.local), s->host);
        }
    }
}


/*
 * get ip from domain name
 * @hostname: input argument, such as: www.google.com
 * @ip: output argument, such as: 1.2.3.4
 */
static int hostname_to_ip(const char *hostname, char *ip)
{
    int                 i;
    struct hostent     *he;
    struct in_addr    **addr_list;

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


static char *sockaddr_to_str(struct sockaddr_in *addr)
{
    char        buf[16];
    static char addr_s[32];

    inet_ntop(AF_INET, (char *)&(addr->sin_addr),
              buf, sizeof(struct sockaddr_in));
    memset(addr_s, 0, sizeof(addr_s));
    snprintf(addr_s, 32, "%s:%d", buf, ntohs(addr->sin_port));
    return addr_s;
}


static void log_core(const char *prompt, const char *format, ...)
{
    va_list argList;
    va_start(argList, format);
    printf("[+%s] -- ", prompt);
    vprintf(format, argList);
    va_end(argList);
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


static void timer_handler(void)
{
    static time_t   last_time;
    time_t          now = time(NULL);

    if (difftime(now, last_time) >= DUMP_INTERVAL) {
        last_time = now;
        dump_session(sm.h, "established");
        dump_session(sm.hc, "connecting");
    }
}


/*
 * print data in rows of 16 bytes: offset   hex   ascii
 *
 * 00000   47 45 54 20 2f 20 48 54  54 50 2f 31 2e 31 0d 0a   GET / HTTP/1.1..
 *
 * refer to: http://www.tcpdump.org/sniffex.c
 */
void print_hex_ascii_line(const u_char *payload, int len, int offset)
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
void print_payload(const u_char *payload, int len)
{

    int len_rem = len;
    int line_width = 16;    /* number of bytes per line */
    int line_len;
    int offset = 0;        /* zero-based offset counter */
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
