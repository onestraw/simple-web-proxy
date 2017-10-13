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

#define VERSION             "beta-0.1"
#define DEFAULT_ADDR        "127.0.0.1"
#define DEFAULT_PORT        3128
#define MAX_CONN            4096
#define MAX_EVENTS          4096
#define BUF_SIZE            1024*10     /* 10k */
#define POOL_MAX            26          /* 2^26 = 64M */
#define HOST_MAX_LENGTH     128
#define PAYLOAD_MAX_LENGTH  128
#define DUMP_INTERVAL       30          /* seconds */
#define FORMAT              "%-24s%-24s%-s\n"
#define HEADER_FORMAT       "%-16s"FORMAT
#define ITEM_FORMAT         "%-16u"FORMAT
#define LOG_TIME_FORMAT     "%4d/%02d/%02d %02d:%02d:%02d"
#define LOG_TIME_STR_LEN    sizeof("1970/09/28 12:00:00")
#define LOG_DEBUG           1
#define LOG_INFO            2
#define EPOLL_PROXY_CLIENT  1
#define EPOLL_PROXY_SERVER  2
#define TO_CLIENT           1
#define TO_SERVER           2

#define MIN(a, b)   ((a) < (b) ? (a):(b))

static void log_core(const char *prompt, const char *format, ...);
#define log_debug(...)  \
    if (proxy.log_level <= LOG_DEBUG) log_core("debug", __VA_ARGS__)
#define log_info(...)  \
    if (proxy.log_level <= LOG_INFO) log_core("info", __VA_ARGS__)

#define log_error(s)  fprintf(stderr, "%s [+error] %s:%d %s -- %s %s\n",\
        cached_log_time, __FILE__, __LINE__, __FUNCTION__, s, strerror(errno))

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

typedef struct _epoll_data{
    int fd;
    int flag;
    session_t *session;
} epoll_data;

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


typedef struct {
    char            addr[32];
    unsigned int    port;
    int             log_level;
    int             is_dump;
} proxy_t;


void proxy_run(void);
void proxy_init(void);
void proxy_exit(void);
int handle_accept_event(int);
int handle_epollhup_event(int);
int handle_epollin_event(epoll_data *);
int handle_epollout_event(epoll_data *);
int parse_http_request(session_t *, char *, size_t);
int cache_http_request(mpool *, session_t *, char *, ssize_t);
int sec_send(session_t *, int, char *, size_t);
int put_session(hash_t *, session_t *);
session_t *get_session(hash_t *, int);
int del_session(hash_t *, int);
session_t *create_session(mpool *, int);
int close_session(session_t *, int);
void dump_one_session(session_t *);
void dump_session(hash_t *, const char *);
static int open_listening_socket(const char *, unsigned int, int);
static void epoll_ctl_add(int, epoll_data *, uint32_t);
static void set_sockaddr(struct sockaddr_in *, const char *, unsigned int);
static int set_nonblocking(int);
static int new_connection(session_t *, int);
static int close_socket(int, int);
static int parse_host_field(char *, char *, unsigned int *);
static char *sockaddr_to_str(struct sockaddr_in *);
static void INT_handler(int);
static void timer_handler(void);
void print_hex_ascii_line(const u_char *, int, int);
void print_payload(const u_char *, int);


static char *not_found_response =
    "HTTP/1.1 404 Not Found\n"
    "Content-type: text/html\n" "\n"
    "<html>\n"
    " <body>\n"
    "  <h1>Not Found</h1>\n"
    "  <p>The requested URL was not found on this server.</p>\n"
    " </body>\n" "</html>\n";

static char cached_log_time[LOG_TIME_STR_LEN];
static proxy_t proxy;

static session_manager_t sm = {
    .put    = put_session,
    .get    = get_session,
    .del    = del_session,
    .dump   = dump_session,
    .create = create_session,
    .close  = close_session,
};


void show_help(char *cmd)
{
    fprintf(stderr, "Usage: %s [-?hvVd] [-s address] [-p port]\n"
            "Options:\n"
            " -?, -h    :print this help\n"
            " -v        :print version number\n"
            " -V        :verbose mode, dump session table\n"
            " -d        :enable debug mode\n"
            " -s        :proxy listening address\n"
            " -p        :proxy listening port\n"
            , cmd);
}


void get_options(int argc, char *argv[])
{
    int     i;
    u_char *p;

    for (i = 1; i < argc; i++) {
        p = (u_char *) argv[i];

        if (*p++ != '-') {
            fprintf(stderr, "invalid options \"%s\"\n", argv[i]);
            exit(1);
        }

        while (*p) {
            switch (*p++) {
                case '?':
                case 'h':
                    show_help(argv[0]);
                    exit(0);
                    break;
                case 'v':
                    fprintf(stderr, "%s %s\n", argv[0], VERSION);
                    exit(0);
                    break;
                case 'V':
                    proxy.is_dump = 1;
                    break;
                case 'd':
                    proxy.log_level = LOG_DEBUG;
                    break;
                case 's':
                    if (*p && strlen((char *) p) < 32) {
                        strcpy(proxy.addr, (char *) p);
                    } else if (argv[++i] && strlen(argv[i]) < 32) {
                        strcpy(proxy.addr, argv[i]);
                    } else {
                        fprintf(stderr, "invalid option: %s\n", argv[i]);
                        exit(0);
                    }
                    break;
                case 'p':
                    if (*p) {
                        proxy.port = atoi((char *) p);
                    } else if (argv[++i]) {
                        proxy.port = atoi(argv[i]);
                    } else {
                        fprintf(stderr, "invalid option: %s\n", argv[i]);
                        exit(0);
                    }
                    break;
                default:
                    break;
            }
        }
    }
}


int main(int argc, char *argv[])
{
    proxy_init();
    get_options(argc, argv);

    if (signal(SIGINT, SIG_IGN) != SIG_IGN) {
        signal(SIGINT, INT_handler);
    }
    signal(SIGPIPE, SIG_IGN);

    proxy_run();
    return 0;
}


void proxy_init(void)
{
    strcpy(proxy.addr, DEFAULT_ADDR);
    proxy.port = DEFAULT_PORT;
    proxy.log_level = LOG_INFO;
    proxy.is_dump = 0;

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
    epoll_data          ed;
    epoll_data         *edp;
    struct epoll_event  events[MAX_EVENTS];

    listen_sock = open_listening_socket(proxy.addr, proxy.port, MAX_CONN);
    if (listen_sock == -1) {
        exit(1);
    }
    ed.fd = listen_sock;
    epoll_ctl_add(sm.epfd, &ed, EPOLLIN | EPOLLET);

    for (;;) {
        nfds = epoll_wait(sm.epfd, events, MAX_EVENTS, -1);

        /* hook function, such as dump session */
        timer_handler();

        log_debug("epoll_wait return: %d\n", nfds);
        if (nfds == -1) {
            log_error("epoll_wait()");
            continue;
        }

        for (i = 0; i < nfds; i++) {
            edp = events[i].data.ptr;

            if (edp->fd == listen_sock) {
                handle_accept_event(listen_sock);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                handle_epollin_event(edp);
            }

            if (events[i].events & EPOLLOUT) {
                handle_epollout_event(edp);
            }

            if (events[i].events & EPOLLERR) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    log_error("EPOLLERR:");
                }
            }

            /* check if the connection is closing */
            if (events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
                handle_epollhup_event(edp->fd);
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
int handle_accept_event(int listen_sock)
{
    int                 conn_sock;
    socklen_t           len;
    epoll_data         *ed;
    struct sockaddr_in  cli_addr;

    /* one or more incoming connection */
    while (1) {
        len = sizeof(struct sockaddr_in);
        conn_sock = accept(listen_sock, (struct sockaddr *)&cli_addr, &len);
        if (conn_sock == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            log_error("accept()");
            return -1;
        }

        set_nonblocking(conn_sock);

        ed = (epoll_data *) mpool_alloc(sm.pool, sizeof(epoll_data));
        ed->fd = conn_sock;
        ed->flag = EPOLL_PROXY_CLIENT;
        ed->session = NULL;
        epoll_ctl_add(sm.epfd, ed, EPOLLIN | EPOLLOUT |
                      EPOLLET | EPOLLRDHUP | EPOLLHUP);

        log_info("accept connection from %s\n", sockaddr_to_str(&cli_addr));
    }

    return 0;
}


/*
 * close tcp connection, unregister session and epoll
 */
int handle_epollhup_event(int fd)
{
    log_debug("enter %s\n", __FUNCTION__);

    session_t *session = sm.get(sm.h, fd);
    if (session != NULL) {
        sm.close(session, sm.epfd);
        sm.del(sm.h, session->conn_ps.sock);
    } else {
        log_debug("do not find any session in cache \n");
        //close_socket(fd, sm.epfd);
    }
    return 0;
}


/*
 * receive http request from client and http response from server
 */
int handle_epollin_event(epoll_data *edp)
{
    int         ret;
    char        buf[BUF_SIZE];
    ssize_t     nread;
    ssize_t     bytes_read;
    session_t  *session;

    log_debug("enter %s\n", __FUNCTION__);

    bzero(buf, sizeof(buf));
    bytes_read = 0;

    while ((nread = recv(edp->fd, buf + bytes_read, BUF_SIZE - bytes_read, MSG_DONTWAIT)) > 0) {
        bytes_read += nread;
    }

    if (nread == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            log_error("recv()");
            /* EBADF, EPIPE, ECONNRESET */
            if ((session = edp->session)) {
                sm.close(session, sm.epfd);
                sm.del(sm.h, session->conn_ps.sock);
            }
        }
    }

    /*
     * the fd is proxy-server, forward the buf to client
     */
    if (edp->flag == EPOLL_PROXY_SERVER) {
        session = sm.get(sm.h, edp->fd);
        if (session) {
            ret = sec_send(session, TO_CLIENT, buf, bytes_read);
            if (ret < 0) {
                return -1;
            }
        } else {
            log_error("unexpected error");
        }
    } else if (edp->flag == EPOLL_PROXY_CLIENT) {
        /*
         * check if there exists session for the client
         * maybe keepalive, reuse the old connection
         */
        if (edp->session) {
            return sec_send(edp->session, TO_SERVER, buf, bytes_read);
        }

        session = sm.create(sm.pool, edp->fd);
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
            sm.put(sm.hc, session);
        } else {
            /* connection is ready */
            sm.put(sm.h, session);
            ret = sec_send(session, TO_SERVER, buf, bytes_read);
            if (ret < 0) {
                goto proxy_request_error;
            }
        }
    }

    return 0;

proxy_request_error:
    log_error("proxy_request_error");
    print_payload((u_char *) buf, MIN(bytes_read, PAYLOAD_MAX_LENGTH));
    ret = write(edp->fd, not_found_response, strlen(not_found_response));
    if (session) {
        sm.close(session, sm.epfd);
        sm.del(sm.h, session->conn_ps.sock);
    }
    return -1;
}


/*
 * mainly handle these async connect event
 */
int handle_epollout_event(epoll_data *edp)
{
    session_t   *s;

    log_debug("enter %s\n", __FUNCTION__);

    s = sm.get(sm.hc, edp->fd);
    if (edp->flag == EPOLL_PROXY_SERVER && s != NULL) {
        sm.put(sm.h, s);
        sm.del(sm.hc, edp->fd);
        if (sec_send(s, TO_SERVER, s->request, s->request_len) < 0) {
            log_error("sec_send");
            print_payload((u_char *) s->request,
                          MIN(s->request_len, PAYLOAD_MAX_LENGTH));
            return sec_send(s, TO_CLIENT, not_found_response,
                            strlen(not_found_response));
        }
    } else {
        log_debug("%s: get_session fail\n", __FUNCTION__);
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


int sec_send(session_t *s, int direction, char *buf, size_t len)
{
    int        sk;
    ssize_t    nw;
    ssize_t    bytes_send;

    if (direction != TO_CLIENT && direction != TO_SERVER) {
        log_error("unexpected direction");
        return -1;
    }

    if (direction == TO_CLIENT) {
        sk = s->conn_pc.sock;
    } else {
        sk = s->conn_ps.sock;
    }

    bytes_send = 0;

    while ((nw = send(sk, buf + bytes_send, len - bytes_send, MSG_NOSIGNAL)) > 0) {
        bytes_send += nw;
    }

    if (nw == -1 && errno != EAGAIN) {
        log_error("send()");
        dump_one_session(s);
        print_payload((u_char *) buf, MIN(len, PAYLOAD_MAX_LENGTH));
        /* EBADF, EPIPE, ECONNRESET */
        sm.close(s, sm.epfd);
        sm.del(sm.h, s->conn_ps.sock);

        return -1;
    }

    return 0;
}


static int open_listening_socket(const char *addr, unsigned int port, int max_conn)
{
    int                 sock;
    struct sockaddr_in  srv_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        log_error("scoket()");
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   &(int) {1}, sizeof(int)) < 0)
    {
        log_error("setsockopt(SO_REUSEADDR)");
        return -1;
    }

    set_sockaddr(&srv_addr, addr, port);
    if (bind(sock, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        log_error("bind()");
        return -1;
    }

    if (set_nonblocking(sock) < 0) {
        log_error("set_nonblocking()");
        return -1;
    }

    if (listen(sock, max_conn) < 0) {
        log_error("listen()");
        return -1;
    }

    log_info("listen on %s:%d \n", addr, port);

    return sock;
}


/*
 * register events of fd to epfd
 */
static void epoll_ctl_add(int epfd, epoll_data *data, uint32_t events)
{
    struct epoll_event ev;
    ev.events = events | EPOLLERR;
    ev.data.ptr = data;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, data->fd, &ev) == -1) {
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


static int set_nonblocking(int sockfd)
{
    if (fcntl(sockfd, F_SETFL,
              fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK) == -1)
    {
        return -1;
    }
    return 0;
}


/*
 * create new connection between proxy and remote server
 * return value:
 *      -1: connect error
 *      0: connect success
 *      1: connect attempt is in progress
 */
static int new_connection(session_t *session, int epfd)
{
    int                 connect_flag;
    int                 sockfd;
    char                ip[16];
    socklen_t           len;
    epoll_data         *ed;
    unsigned int        port;
    struct sockaddr_in  srv_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        log_error("socket()");
        goto error;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
                   &(int) {1}, sizeof(int)) < 0)
    {
        log_error("setsockopt(SO_REUSEADDR)");
        goto error;
    }

    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY,
                   &(int) {1}, sizeof(int)) < 0)
    {
        log_error("setsockopt(TCP_NODELAY)");
        goto error;
    }

    if (set_nonblocking(sockfd) < 0) {
        log_error("set_nonblocking()");
        goto error;
    }

    if (parse_host_field(session->host, ip, &port) < 0) {
        goto error;
    }

    set_sockaddr(&srv_addr, ip, port);
    connect_flag = connect(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    if (connect_flag < 0 && errno != EINPROGRESS) {
        log_error("connect()");
        goto error;
    }

    session->conn_ps.sock = sockfd;
    session->conn_ps.remote = srv_addr;
    len = sizeof(struct sockaddr_in);
    if (getsockname(sockfd, (struct sockaddr *)&session->conn_ps.local, &len)
        == -1) {
        log_error("getsockname()");
        goto error;
    }

    ed = (epoll_data *) mpool_alloc(sm.pool, sizeof(epoll_data));
    ed->fd = sockfd;
    ed->flag = EPOLL_PROXY_SERVER;
    ed->session = session;
    epoll_ctl_add(epfd, ed, EPOLLIN | EPOLLOUT |
                  EPOLLET | EPOLLRDHUP | EPOLLHUP);

    log_debug("client: %s === server: %s\n",
              sockaddr_to_str(&session->conn_pc.remote), session->host);

    return (errno == EINPROGRESS ? 1 : 0);

 error:
    close(sockfd);
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
    if (ret == -1) {
        log_info("kh_put error, sockfd=%d, ret=%d\n",
                 s->conn_ps.sock, ret);
        return -1;
    }
    if (ret == 0 && k < kh_end(h)) {
        log_info("key conflict, following two line are new and old:\n");
        dump_one_session(s);
        dump_one_session((session_t *) kh_val(h, k));
        //return -1;
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

    bzero((u_char *)s, sizeof(session_t));
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


void dump_one_session(session_t *s)
{
    char    buf[32];

    strcpy(buf, sockaddr_to_str(&s->conn_pc.remote));
    printf(ITEM_FORMAT, s->conn_ps.sock, buf,
           sockaddr_to_str(&s->conn_ps.local), s->host);
}


void dump_session(hash_t *table, const char *table_name)
{
    khint_t     k;

    if (kh_size(table) == 0){
        log_info("%s session table is empty\n", table_name);
        return;
    }

    printf("+------------------------dump_session"
           " %s size:%d--------------------+\n", table_name, kh_size(table));
    printf(HEADER_FORMAT, "key", "client", "proxy", "server");

    for (k = kh_begin(table); k != kh_end(table); k++) {
        if (kh_exist(table, k)) {
            dump_one_session((session_t *) kh_val(table, k));
        }
    }
}


/*
 * parse host field in http request header to get ip and port
 */
static int parse_host_field(char *hostname, char *ip, unsigned int *port)
{
    int                 i;
    char               *p;
    struct hostent     *he;
    struct in_addr    **addr_list;

    for (p = hostname; *p && *p != ':'; p++) ;

    if (*p != 0) {
        *port = atoi(p);
        *p = 0;
    } else {
        *port = 80;
    }

    if ((he = gethostbyname(hostname)) == NULL) {
        log_debug("gethostbyname(%s)", hostname);
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
    printf("%s [+%s] ", cached_log_time, prompt);
    vprintf(format, argList);
    va_end(argList);
}


/*
 * handle Ctrl-C signal
 */
static void INT_handler(int sig)
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
        signal(SIGINT, INT_handler);
    }
}


static void timer_handler(void)
{
    static time_t   last_time;
    time_t          now = time(NULL);
    struct tm      *tm;

    tm = localtime(&now);
    sprintf(cached_log_time, LOG_TIME_FORMAT,
            tm->tm_year + 1900, tm->tm_mon, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);

    if (proxy.is_dump == 1 && difftime(now, last_time) >= DUMP_INTERVAL) {
        last_time = now;
        sm.dump(sm.h, "established");
        sm.dump(sm.hc, "connecting");
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
