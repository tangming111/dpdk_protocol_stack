/* Run under LD_PRELOAD against a namespace peer: validates public libc APIs. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
static void readable(int fd)
{
    struct pollfd poll_fds = {fd, POLLIN, 0};
    assert(poll(&poll_fds, 1, 5000) == 1 && (poll_fds.revents & POLLIN));
}

static struct sockaddr_in addr(const char *ip, int port)
{
    struct sockaddr_in socket_addr;
    memset(&socket_addr, 0, sizeof(socket_addr));
    socket_addr.sin_family = AF_INET;
    socket_addr.sin_port = htons(port);
    assert(inet_pton(AF_INET, ip, &socket_addr.sin_addr) == 1);
    return socket_addr;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    int server = !strcmp(argv[1], "server");
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    int (*isudp)(int) = dlsym(RTLD_DEFAULT, "nisdgram");
    assert(isudp && isudp(fd));
    int type = 0;
    socklen_t len = sizeof(type);
    assert(getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_DGRAM);
    assert((fcntl(fd, F_GETFL) & O_NONBLOCK) && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    struct sockaddr_in socket_addr = addr(argv[2], 19090), source;
    unsigned char data[1600];
    memset(data, 0x5a, sizeof(data));
    if (server) {
        assert(bind(fd, (void *)&socket_addr, sizeof(socket_addr)) == 0);
        int ep = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
        assert(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0);
        for (;;) {
            assert(epoll_wait(ep, &ev, 1, 5000) == 1 && ev.data.fd == fd);
            unsigned char first;
            len = sizeof(source);
            assert(recvfrom(fd, &first, 1, MSG_PEEK, (void *)&source, &len) >= 0);
            struct iovec iovecs[2] = {{data, 7}, {data + 7, sizeof(data) - 7}};
            struct msghdr message;
            memset(&message, 0, sizeof(message));
            message.msg_iov = iovecs;
            message.msg_iovlen = 2;
            message.msg_name = &source;
            message.msg_namelen = sizeof(source);
            ssize_t n = recvmsg(fd, &message, 0);
            assert(n >= 0 && !(message.msg_flags & MSG_TRUNC));
            iovecs[0].iov_len = n < 7 ? (size_t)n : 7;
            iovecs[1].iov_len = n > 7 ? (size_t)n - 7 : 0;
            assert(sendmsg(fd, &message, 0) == n);
            if (n == 4 && !memcmp(data, "quit", 4))
                break;
        }
        close(ep);
        close(fd);
        puts("PASS: UDP stack server recvfrom/peek, recvmsg/sendmsg, epoll, zero-length datagrams");
        return 0;
    }
    assert(recvfrom(fd, data, sizeof(data), 0, NULL, NULL) == -1 && errno == EAGAIN);
    assert(send(fd, data, 1, 0) == -1 && errno == EDESTADDRREQ);
    assert(sendto(fd, data, 1473, 0, (void *)&socket_addr, sizeof(socket_addr)) == -1 &&
           errno == EMSGSIZE);
    assert(sendto(fd, "abcdef", 6, 0, (void *)&socket_addr, sizeof(socket_addr)) == 6);
    readable(fd);
    int available = -1;
    assert(ioctl(fd, FIONREAD, &available) == 0 && available == 6);
    len = sizeof(source);
    assert(recvfrom(fd, data, 2, MSG_PEEK, (void *)&source, &len) == 2 && !memcmp(data, "ab", 2));
    assert(source.sin_family == AF_INET && source.sin_port == socket_addr.sin_port &&
           source.sin_addr.s_addr == socket_addr.sin_addr.s_addr);
    assert(recv(fd, data, 3, 0) == 3 && !memcmp(data, "abc", 3));
    assert(recv(fd, data, sizeof(data), 0) == -1 && errno == EAGAIN);
    assert(connect(fd, (void *)&socket_addr, sizeof(socket_addr)) == 0);
    assert(send(fd, "abcdefgh", 8, 0) == 8);
    readable(fd);
    struct iovec iovecs[2] = {{data, 2}, {data + 8, 2}};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = iovecs;
    message.msg_iovlen = 2;
    message.msg_name = &source;
    message.msg_namelen = sizeof(source);
    assert(recvmsg(fd, &message, MSG_TRUNC) == 8 && (message.msg_flags & MSG_TRUNC));
    assert(!memcmp(data, "ab", 2) && !memcmp(data + 8, "cd", 2));
    assert(writev(fd, iovecs, 2) == 4);
    readable(fd);
    assert(readv(fd, iovecs, 2) == 4 && !memcmp(data, "ab", 2) && !memcmp(data + 8, "cd", 2));
    assert(send(fd, NULL, 0, 0) == 0);
    readable(fd);
    assert(recv(fd, NULL, 0, 0) == 0);
    assert(send(fd, "after-zero", 10, 0) == 10);
    readable(fd);
    assert(recv(fd, data, sizeof(data), 0) == 10);
    assert(fcntl(fd, F_SETFL, 0) == 0);
    assert(send(fd, "blocking", 8, 0) == 8);
    assert(recv(fd, data, sizeof(data), 0) == 8 && !memcmp(data, "blocking", 8));
    assert(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    len = sizeof(source);
    assert(getsockname(fd, (void *)&source, &len) == 0 && source.sin_port != 0);
    struct sockaddr other_socket_addr;
    memset(&other_socket_addr, 0, sizeof(other_socket_addr));
    other_socket_addr.sa_family = AF_UNSPEC;
    assert(connect(fd, &other_socket_addr, sizeof(other_socket_addr)) == 0);
    assert(getpeername(fd, (void *)&source, &len) == -1 && errno == ENOTCONN);
    assert(sendto(fd, "quit", 4, 0, (void *)&socket_addr, sizeof(socket_addr)) == 4);
    readable(fd);
    assert(recv(fd, data, sizeof(data), 0) == 4);
    close(fd);
    puts("PASS: UDP stack client sendto/connect, datagram boundaries, MSG_PEEK/TRUNC, "
         "readv/writev, zero-length, FIONREAD, EMSGSIZE, disconnect");
    return 0;
}
