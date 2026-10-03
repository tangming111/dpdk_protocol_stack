/* Fake stack backend exercises hook.c against real Linux descriptors. */
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include "../net.h"
#include "../epoll_core.h"
static int used[65536], ready[65536], status[65536];
void hook_test_ready(int fd, int events)
{
    __atomic_store_n(&ready[fd], events, __ATOMIC_RELEASE);
    nepoll_notify();
}

int netstack_init(void)
{
    return 0;
}

int netstack_fd_is_managed(int fd)
{
    return fd >= 0 && fd < 65536 && used[fd];
}

int nsocket(int d, int t, int p)
{
    (void)d;
    (void)p;
    int fd = syscall(SYS_eventfd2, 0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd >= 0 && fd < 65536) {
        used[fd] = 1;
        ready[fd] = EPOLLIN | EPOLLOUT;
        status[fd] = (t & SOCK_NONBLOCK) ? O_NONBLOCK : 0;
    }
    return fd;
}

int nclose(int fd)
{
    used[fd] = 0;
    return syscall(SYS_close, fd);
}

int nbind(int fd, const struct sockaddr *socket_addr, socklen_t n)
{
    (void)fd;
    (void)socket_addr;
    (void)n;
    return 0;
}

int nconnect(int fd, const struct sockaddr *socket_addr, socklen_t n)
{
    (void)fd;
    (void)socket_addr;
    (void)n;
    return 0;
}

int nlisten(int fd, int n)
{
    (void)fd;
    (void)n;
    return 0;
}

int naccept(int fd, struct sockaddr *socket_addr, socklen_t *n)
{
    (void)fd;
    (void)socket_addr;
    (void)n;
    return nsocket(AF_INET, SOCK_STREAM, 0);
}

ssize_t nrecv(int fd, void *b, size_t n, int flags)
{
    (void)flags;
    if (!(ready[fd] & EPOLLIN)) {
        errno = EAGAIN;
        return -1;
    }
    if (n > 6)
        n = 6;
    memcpy(b, "abcdef", n);
    return n;
}

ssize_t nsend(int fd, const void *b, size_t n, int flags)
{
    (void)fd;
    (void)b;
    (void)flags;
    return n;
}

int nready(int fd)
{
    return __atomic_load_n(&ready[fd], __ATOMIC_ACQUIRE);
}

int nfcntl(int fd, int cmd, long arg)
{
    if (cmd == F_GETFL)
        return status[fd];
    if (cmd == F_SETFL) {
        status[fd] = arg;
        return 0;
    }
    return syscall(SYS_fcntl, fd, cmd, arg);
}

int nioctl(int fd, unsigned long request, void *v)
{
    (void)fd;
    if (request == FIONREAD)
        *(int *)v = 6;
    return 0;
}

int nshutdown(int fd, int how)
{
    (void)how;
    ready[fd] = EPOLLIN | EPOLLRDHUP;
    return 0;
}

int nsetsockopt(int fd, int l, int o, const void *v, socklen_t n)
{
    (void)fd;
    (void)l;
    (void)o;
    (void)v;
    (void)n;
    return 0;
}

int ngetsockopt(int fd, int l, int o, void *v, socklen_t *n)
{
    (void)fd;
    (void)l;
    (void)o;
    int x = 0;
    if (*n > sizeof(x))
        *n = sizeof(x);
    memcpy(v, &x, *n);
    return 0;
}

int ngetsockname(int fd, struct sockaddr *socket_addr, socklen_t *n)
{
    (void)fd;
    (void)socket_addr;
    (void)n;
    return 0;
}

int ngetpeername(int fd, struct sockaddr *socket_addr, socklen_t *n)
{
    (void)fd;
    (void)socket_addr;
    (void)n;
    return 0;
}

void nfork_prepare(void)
{
}

void nfork_parent(void)
{
}

void nfork_child(void)
{
    memset(used, 0, sizeof(used));
}

void netstack_fini(void)
{
}

int nisdgram(int fd)
{
    (void)fd;
    return 0;
}

ssize_t nsendto(int fd, const void *b, size_t n, int flags, const struct sockaddr *socket_addr,
                socklen_t l)
{
    (void)socket_addr;
    (void)l;
    return nsend(fd, b, n, flags);
}

ssize_t nrecvfrom(int fd, void *b, size_t n, int flags, struct sockaddr *socket_addr, socklen_t *l)
{
    (void)socket_addr;
    (void)l;
    return nrecv(fd, b, n, flags);
}

ssize_t ndgram_sendmsg(int fd, const struct msghdr *message, int flags)
{
    (void)fd;
    (void)message;
    (void)flags;
    errno = EOPNOTSUPP;
    return -1;
}

ssize_t ndgram_recvmsg(int fd, struct msghdr *message, int flags)
{
    (void)fd;
    (void)message;
    (void)flags;
    errno = EOPNOTSUPP;
    return -1;
}
