# vProxy

A simple http proxy at the clent side.


# workflow of web proxy

                                         +--------+
                                         |        |
                                         |  DNS   |
                                         | SERVER |
                                         |        |
                                         +--------+
                                              ^
                                              |
                                             (3) resolve the host name
                                              |
                                              v
    +--------+                            +-------+                            +--------+
    |        | -- (1)TCP connection ----> |       |                            |        |
    |        | -- (2)http request ------> |       | --- (4)TCP connection ---> |        |
    |        |                            |       | --- (5)http request -----> |        |
    | CLIENT |                            | PROXY |                            | SERVER |
    |        | <- (7)http response ------ |       | <-- (6)http response ----- |        |
    |        |         ...                |       |          ...               |        |
    |        |                            |       |                            |        |
    |        | -- (n)close connection --> |       | - (n+1)close connection -> |        |
    +--------+                            +-------+                            +--------+


# key point

* proxy plays two roles: server and client
* we need to know how client interacts with server and every possible exceptional state

* client send TCP SYN to server, pending in SYN queue, waiting for server accept.
    - from the view of programmer, epoll
    - connect() return 0
    - connect() return -1 && errno == EINPROGRESS
    - if the async connect() finish, the sockfd will become writable (POLLOUT is set), check the error with getsockopt(listenfd, SOL_SOCKET, SO_ERROR,...)

    - **errno in this phase:**
    - ECONNREFUSED: remote port is not open, or server connection reaches maximum [listen(fd, backlog)]
    - ETIMEOUT: connection timed out
    - EINPROGRESS: non-blocking connect request is pended to SYN queue


* the connection is established after server received ACK
    - accept(listenfd, ) return success
    - listenfd become readable (POLLIN is set)

    - **errno in this phase:**
    - EAGAIN or EWOULDBLOCK (when fd is set to nonblocking): no pending connection
    - EBADF: listenfd is not an open file descriptor


* client send/write data
* client recv/read data
    - remove the session if the peer close the connection

    - **errno in this phase:**
    - EAGAIN or EWOULDBLOCK: The file descriptor fd refers to a socket and has been marked nonblocking (O_NONBLOCK), and the write/read would block.
    - EBADF: sockfd is not a valid open file descriptor.
    - ECONNRESET: Connection reset by peer.
    - EPIPE: The local end has been shut down on a connection oriented socket.

    - **todo:** check http Content-Length or Transfer-Encoding


* client close the connection
* server close the connection
    - call close() after read/write() finish
    - read() should return 0 on receipt of a FIN from the peer
    - when write() returns EPIPE, it also raises the SIGPIPE signal - you never see the EPIPE error unless you handle or ignore the signal
    - [how to tell if peer close the connection](https://goo.gl/Mi9sgD)


# close vs shutdown

    close() will prevent any more reads and writes to the socket and free it.

    int shutdown(int sockfd, int how);

    The  shutdown()  call  causes  all  or part of a full-duplex connection on the socket associated with sockfd to be shut down.  If how is SHUT_RD, further receptions will be disallowed.  If how is SHUT_WR, further transmissions will be disallowed.  If how is SHUT_RDWR, further receptions and transmissions will be disallowed.
    But shutdown() doesn't free a the socket descriptor.


# read/write vs recv/send

    read() is equivalent to recv() with a flags parameter of 0. Other values for the flags parameter change the behaviour of recv(). Similarly, write() is equivalent to send() with flags == 0.

    MSG_DONTWAIT (since Linux 2.2)
        Enables nonblocking operation; if the operation would block,
        the call fails with the error EAGAIN or EWOULDBLOCK.
        [reference](http://man7.org/linux/man-pages/man2/recv.2.html)
    

# dependencies
- http parser: https://github.com/h2o/picohttpparser
- hash table : https://github.com/attractivechaos/klib
- memory pool: https://github.com/silentbicycle/mpool

# reference
- http://www.cs.princeton.edu/courses/archive/spr08/cos461/web_proxy.html
- https://www.w3.org/Protocols/
- [Beej's Guide to Network Programming](http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html)
- [ssl proxy](https://github.com/libevent/libevent/blob/master/sample/le-proxy.c)
- [how to use epoll](http://man7.org/linux/man-pages/man7/epoll.7.html)
