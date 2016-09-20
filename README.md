# vProxy
    A simple http proxy that deploy in clent end.

# workflow of web proxy

    CLIENT --- (1)http request ---> PROXY --- (2)http request ---> SERVER
    CLIENT <-- (4)http response --- PROXY <-- (3)http response --- SERVER
    
    +--------+                            +-------+                           +--------+
    |        | -- (1)TCP connection ----> |       |                           |        |
    |        | -- (2)http request ------> |       | --- (3)TCP connection --> |        |
    |        |                            |       | --- (4)http request ----> |        |
    | CLIENT |                            | PROXY |                           | SERVER |
    |        | <- (6)http response ------ |       | <-- (5)http response ---  |        |
    |        |         ...                |       |          ...              |        |
    |        |                            |       |                           |        |
    |        | <- (n+1)close connection - |       | <-- (n)close connection - |        |
    +--------+                            +-------+                           +--------+

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
    
# errno

### read
    
    EAGAIN or EWOULDBLOCK: The file descriptor fd refers to a socket and has been marked 
                           nonblocking (O_NONBLOCK), and the read would block.
    EBADF: fd is not a valid file descriptor or is not open for reading.

### write

    EAGAIN or EWOULDBLOCK: The file descriptor fd refers to a socket and has been marked
                           nonblocking (O_NONBLOCK), and the write would block.
    EBADF: fd is not a valid file descriptor or is not open for writing.
    EPIPE: fd is connected to a pipe or socket whose reading end is closed.

# dependencies
- http parser: https://github.com/h2o/picohttpparser
- hash table : https://github.com/attractivechaos/klib
- memory pool: https://github.com/silentbicycle/mpool

# reference
- http://www.cs.princeton.edu/courses/archive/spr08/cos461/web_proxy.html
- https://www.w3.org/Protocols/
- [Beej's Guide to Network Programming](http://beej.us/guide/bgnet/output/html/singlepage/bgnet.html)
- [ssl proxy](https://github.com/libevent/libevent/blob/master/sample/le-proxy.c)
