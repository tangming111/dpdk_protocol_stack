#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include "net.h"
#include "epoll_core.h"
#ifndef EPOLLEXCLUSIVE
#define EPOLLEXCLUSIVE (1u << 28)
#endif
/* Real kernel descriptors back stack sockets. Their readiness is computed by
 * nready(); file descriptors and unsupported families continue through libc. */
static __thread int initializing;
static int fork_child_mode;
static void *sym(const char *name)
{
    return dlsym(RTLD_NEXT, name);
}

#define REAL(name, ...) ((__typeof__(&name))sym(#name))(__VA_ARGS__)
static int managed(int fd)
{
    return netstack_fd_is_managed(fd);
}

static int64_t now_ms(void)
{
    struct timespec timestamp;
    clock_gettime(CLOCK_MONOTONIC, &timestamp);
    return (int64_t)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

int socket(int domain, int type, int protocol)
{
    if (!initializing && !fork_child_mode && domain == AF_INET &&
        (((type & 15) == SOCK_STREAM && (!protocol || protocol == IPPROTO_TCP)) ||
         ((type & 15) == SOCK_DGRAM && (!protocol || protocol == IPPROTO_UDP)))) {
        initializing = 1;
        int result = netstack_init();
        initializing = 0;
        if (result) {
            errno = ENODEV;
            return -1;
        }
        return nsocket(domain, type, protocol);
    }
    return REAL(socket, domain, type, protocol);
}

int bind(int fd, const struct sockaddr *socket_addr, socklen_t address_len)
{
    return managed(fd) ? nbind(fd, socket_addr, address_len)
                       : REAL(bind, fd, socket_addr, address_len);
}

int listen(int fd, int backlog)
{
    return managed(fd) ? nlisten(fd, backlog) : REAL(listen, fd, backlog);
}

int connect(int fd, const struct sockaddr *socket_addr, socklen_t address_len)
{
    return managed(fd) ? nconnect(fd, socket_addr, address_len)
                       : REAL(connect, fd, socket_addr, address_len);
}

int accept(int fd, struct sockaddr *socket_addr, socklen_t *address_len)
{
    return managed(fd) ? naccept(fd, socket_addr, address_len)
                       : REAL(accept, fd, socket_addr, address_len);
}

int accept4(int fd, struct sockaddr *socket_addr, socklen_t *address_len, int flags)
{
    if (!managed(fd))
        return REAL(accept4, fd, socket_addr, address_len, flags);
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    int accepted_fd = naccept(fd, socket_addr, address_len);
    if (accepted_fd >= 0) {
        nfcntl(accepted_fd, F_SETFL, (flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0);
        nfcntl(accepted_fd, F_SETFD, (flags & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
    }
    return accepted_fd;
}

ssize_t recv(int fd, void *buffer, size_t buffer_len, int flags)
{
    return managed(fd) ? nrecv(fd, buffer, buffer_len, flags)
                       : REAL(recv, fd, buffer, buffer_len, flags);
}

ssize_t send(int fd, const void *buffer, size_t buffer_len, int flags)
{
    return managed(fd) ? nsend(fd, buffer, buffer_len, flags)
                       : REAL(send, fd, buffer, buffer_len, flags);
}

ssize_t read(int fd, void *buffer, size_t buffer_len)
{
    if (nepoll_owned(fd)) {
        errno = EINVAL;
        return -1;
    }
    return managed(fd) ? nrecv(fd, buffer, buffer_len, 0) : REAL(read, fd, buffer, buffer_len);
}

ssize_t write(int fd, const void *buffer, size_t buffer_len)
{
    if (nepoll_owned(fd)) {
        errno = EINVAL;
        return -1;
    }
    return managed(fd) ? nsend(fd, buffer, buffer_len, 0) : REAL(write, fd, buffer, buffer_len);
}

ssize_t recvfrom(int fd, void *buffer, size_t buffer_len, int flags, struct sockaddr *peer_addr,
                 socklen_t *peer_addr_len)
{
    if (!managed(fd))
        return REAL(recvfrom, fd, buffer, buffer_len, flags, peer_addr, peer_addr_len);
    return nrecvfrom(fd, buffer, buffer_len, flags, peer_addr, peer_addr_len);
}

ssize_t sendto(int fd, const void *buffer, size_t buffer_len, int flags,
               const struct sockaddr *peer_addr, socklen_t peer_addr_len)
{
    if (!managed(fd))
        return REAL(sendto, fd, buffer, buffer_len, flags, peer_addr, peer_addr_len);
    return nsendto(fd, buffer, buffer_len, flags, peer_addr, peer_addr_len);
}

static ssize_t vector_io(int fd, const struct iovec *iovecs, int iov_count, int flags, int is_write)
{
    if (nisdgram(fd)) {
        if (iov_count < 0 || iov_count > IOV_MAX || (iov_count && !iovecs)) {
            errno = EINVAL;
            return -1;
        }
        struct msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = (struct iovec *)iovecs;
        message.msg_iovlen = iov_count;
        return is_write ? ndgram_sendmsg(fd, &message, flags) : ndgram_recvmsg(fd, &message, flags);
    }
    if (iov_count < 0 || iov_count > IOV_MAX || (iov_count && !iovecs)) {
        errno = EINVAL;
        return -1;
    }
    size_t total_bytes = 0;
    for (int i = 0; i < iov_count; ++i) {
        if (iovecs[i].iov_len > (size_t)SSIZE_MAX - total_bytes) {
            errno = EINVAL;
            return -1;
        }
        total_bytes += iovecs[i].iov_len;
    }
    if (!total_bytes)
        return is_write ? nsend(fd, "", 0, flags) : nrecv(fd, NULL, 0, flags);
    /* A single gather/scatter preserves stream order and MSG_PEEK semantics. */
    if (total_bytes > 1024 * 1024)
        total_bytes = 1024 * 1024;
    unsigned char *io_buffer = malloc(total_bytes);
    if (!io_buffer) {
        errno = ENOMEM;
        return -1;
    }
    size_t copied_bytes = 0;
    if (is_write)
        for (int i = 0; i < iov_count && copied_bytes < total_bytes; ++i) {
            size_t chunk_bytes = iovecs[i].iov_len;
            if (chunk_bytes > total_bytes - copied_bytes)
                chunk_bytes = total_bytes - copied_bytes;
            memcpy(io_buffer + copied_bytes, iovecs[i].iov_base, chunk_bytes);
            copied_bytes += chunk_bytes;
        }
    ssize_t result = is_write ? nsend(fd, io_buffer, total_bytes, flags)
                              : nrecv(fd, io_buffer, total_bytes, flags);
    if (!is_write && result > 0) {
        copied_bytes = 0;
        for (int i = 0; i < iov_count && copied_bytes < (size_t)result; ++i) {
            size_t chunk_bytes = iovecs[i].iov_len;
            if (chunk_bytes > (size_t)result - copied_bytes)
                chunk_bytes = result - copied_bytes;
            memcpy(iovecs[i].iov_base, io_buffer + copied_bytes, chunk_bytes);
            copied_bytes += chunk_bytes;
        }
    }
    free(io_buffer);
    return result;
}

ssize_t readv(int fd, const struct iovec *iovecs, int iov_count)
{
    if (nepoll_owned(fd)) {
        errno = EINVAL;
        return -1;
    }
    return managed(fd) ? vector_io(fd, iovecs, iov_count, 0, 0)
                       : REAL(readv, fd, iovecs, iov_count);
}

ssize_t writev(int fd, const struct iovec *iovecs, int iov_count)
{
    if (nepoll_owned(fd)) {
        errno = EINVAL;
        return -1;
    }
    return managed(fd) ? vector_io(fd, iovecs, iov_count, 0, 1)
                       : REAL(writev, fd, iovecs, iov_count);
}

ssize_t sendmsg(int fd, const struct msghdr *message, int flags)
{
    if (!managed(fd))
        return REAL(sendmsg, fd, message, flags);
    if (nisdgram(fd))
        return ndgram_sendmsg(fd, message, flags);
    if (!message) {
        errno = EFAULT;
        return -1;
    }
    if (message->msg_controllen || message->msg_name) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (message->msg_iovlen > IOV_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    return vector_io(fd, message->msg_iov, message->msg_iovlen, flags, 1);
}

ssize_t recvmsg(int fd, struct msghdr *message, int flags)
{
    if (!managed(fd))
        return REAL(recvmsg, fd, message, flags);
    if (nisdgram(fd))
        return ndgram_recvmsg(fd, message, flags);
    if (!message) {
        errno = EFAULT;
        return -1;
    }
    if (message->msg_iovlen > IOV_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    ssize_t result = vector_io(fd, message->msg_iov, message->msg_iovlen, flags, 0);
    if (result >= 0) {
        message->msg_flags = 0;
        message->msg_controllen = 0;
        if (message->msg_name)
            ngetpeername(fd, message->msg_name, &message->msg_namelen);
    }
    return result;
}

int shutdown(int fd, int how)
{
    return managed(fd) ? nshutdown(fd, how) : REAL(shutdown, fd, how);
}

int setsockopt(int fd, int level, int option, const void *value, socklen_t option_len)
{
    return managed(fd) ? nsetsockopt(fd, level, option, value, option_len)
                       : REAL(setsockopt, fd, level, option, value, option_len);
}

int getsockopt(int fd, int level, int option, void *value, socklen_t *option_len)
{
    return managed(fd) ? ngetsockopt(fd, level, option, value, option_len)
                       : REAL(getsockopt, fd, level, option, value, option_len);
}

int getsockname(int fd, struct sockaddr *socket_addr, socklen_t *address_len)
{
    return managed(fd) ? ngetsockname(fd, socket_addr, address_len)
                       : REAL(getsockname, fd, socket_addr, address_len);
}

int getpeername(int fd, struct sockaddr *socket_addr, socklen_t *address_len)
{
    return managed(fd) ? ngetpeername(fd, socket_addr, address_len)
                       : REAL(getpeername, fd, socket_addr, address_len);
}

static int fcntl_arg_kind(int cmd)
{
    switch (cmd) {
        case F_GETFD:
        case F_GETFL:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
        case F_GETPIPE_SZ:
            return 0;
#ifdef F_GET_SEALS
        case F_GET_SEALS:
            return 0;
#endif
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW:
            return 2;
#ifdef F_OFD_GETLK
        case F_OFD_GETLK:
        case F_OFD_SETLK:
        case F_OFD_SETLKW:
            return 2;
#endif
#ifdef F_GETOWN_EX
        case F_GETOWN_EX:
        case F_SETOWN_EX:
            return 2;
#endif
#ifdef F_GET_RW_HINT
        case F_GET_RW_HINT:
        case F_SET_RW_HINT:
        case F_GET_FILE_RW_HINT:
        case F_SET_FILE_RW_HINT:
            return 2;
#endif
        default:
            return 1;
    }
}

static int do_fcntl(int fd, int cmd, va_list ap)
{
    int argument_kind = fcntl_arg_kind(cmd);
    long argument = 0;
    if (argument_kind == 1)
        argument = va_arg(ap, int);
    else if (argument_kind == 2)
        argument = (long)va_arg(ap, void *);
    if (nepoll_owned(fd) && cmd == F_SETFL)
        argument |= O_NONBLOCK; /* The internal doorbell must never block. */
    if (nepoll_owned(fd) && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (managed(fd))
        return nfcntl(fd, cmd, argument);
    typedef int (*fn)(int, int, ...);
    fn real_fcntl = (fn)sym("fcntl");
    if (!argument_kind)
        return real_fcntl(fd, cmd);
    if (argument_kind == 2)
        return real_fcntl(fd, cmd, (void *)argument);
    return real_fcntl(fd, cmd, (int)argument);
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    int r = do_fcntl(fd, cmd, ap);
    va_end(ap);
    return r;
}

int fcntl64(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    int r = do_fcntl(fd, cmd, ap);
    va_end(ap);
    return r;
}

int ioctl(int fd, unsigned long request, ...)
{
    if (nepoll_owned(fd) && request == FIONBIO) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (!managed(fd) && (request == FIOCLEX || request == FIONCLEX))
        return REAL(ioctl, fd, request);
    va_list ap;
    va_start(ap, request);
    void *argument = va_arg(ap, void *);
    va_end(ap);
    return managed(fd) ? nioctl(fd, request, argument) : REAL(ioctl, fd, request, argument);
}

/* poll/select combine native descriptors and stack readiness at 1 ms cadence.
 * poll's interruptible wait preserves EINTR and the absolute caller deadline. */
int poll(struct pollfd *poll_fds, nfds_t fd_count, int timeout)
{
    if (fd_count && !poll_fds) {
        errno = EFAULT;
        return -1;
    }
    int has_stack_fds = 0;
    for (nfds_t i = 0; i < fd_count; ++i)
        if (managed(poll_fds[i].fd)) {
            has_stack_fds = 1;
            break;
        }
    if (!has_stack_fds)
        return REAL(poll, poll_fds, fd_count, timeout);
    struct pollfd *native_fds = calloc(fd_count ? fd_count : 1, sizeof(*native_fds));
    if (!native_fds) {
        errno = ENOMEM;
        return -1;
    }
    int64_t deadline_ms = timeout < 0 ? INT64_MAX : now_ms() + timeout;
    int result;
    for (;;) {
        for (nfds_t i = 0; i < fd_count; ++i) {
            native_fds[i] = poll_fds[i];
            if (managed(poll_fds[i].fd))
                native_fds[i].fd = -1;
            native_fds[i].revents = 0;
        }
        result = REAL(poll, native_fds, fd_count, 0);
        if (result < 0)
            break;
        result = 0;
        for (nfds_t i = 0; i < fd_count; ++i) {
            int ready_mask = native_fds[i].revents;
            if (managed(poll_fds[i].fd)) {
                int stack_events = nready(poll_fds[i].fd);
                ready_mask = 0;
                if (stack_events & EPOLLIN)
                    ready_mask |= poll_fds[i].events & (POLLIN | POLLRDNORM);
                if (stack_events & EPOLLOUT)
                    ready_mask |= poll_fds[i].events & (POLLOUT | POLLWRNORM);
                if (stack_events & EPOLLERR)
                    ready_mask |= POLLERR;
                if (stack_events & EPOLLHUP)
                    ready_mask |= POLLHUP;
#ifdef POLLRDHUP
                if (stack_events & EPOLLRDHUP)
                    ready_mask |= poll_fds[i].events & POLLRDHUP;
#endif
            }
            poll_fds[i].revents = ready_mask;
            if (ready_mask)
                ++result;
        }
        if (result || now_ms() >= deadline_ms)
            break;
        if (REAL(poll, native_fds, fd_count, 1) < 0) {
            result = -1;
            break;
        }
    }
    free(native_fds);
    return result;
}

int select(int nfds, fd_set *read_fds, fd_set *write_fds, fd_set *except_fds,
           struct timeval *timeout_value)
{
    if (nfds < 0 || nfds > FD_SETSIZE ||
        (timeout_value && (timeout_value->tv_sec < 0 || timeout_value->tv_usec < 0 ||
                           timeout_value->tv_usec >= 1000000))) {
        errno = EINVAL;
        return -1;
    }
    int has_stack_fds = 0;
    for (int i = 0; i < nfds; ++i)
        if (managed(i) &&
            ((read_fds && FD_ISSET(i, read_fds)) || (write_fds && FD_ISSET(i, write_fds)) ||
             (except_fds && FD_ISSET(i, except_fds))))
            has_stack_fds = 1;
    if (!has_stack_fds)
        return REAL(select, nfds, read_fds, write_fds, except_fds, timeout_value);
    struct pollfd poll_fds[FD_SETSIZE];
    nfds_t count = 0;
    for (int i = 0; i < nfds; ++i) {
        short event_mask = 0;
        if (read_fds && FD_ISSET(i, read_fds))
            event_mask |= POLLIN;
        if (write_fds && FD_ISSET(i, write_fds))
            event_mask |= POLLOUT;
        if (except_fds && FD_ISSET(i, except_fds))
            event_mask |= POLLPRI;
        if (event_mask)
            poll_fds[count++] = (struct pollfd){i, event_mask, 0};
    }
    int64_t timeout_ms =
                timeout_value
                    ? (int64_t)timeout_value->tv_sec * 1000 + (timeout_value->tv_usec + 999) / 1000
                    : -1,
            start_ms = now_ms();
    int timeout = timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms;
    int result = poll(poll_fds, count, timeout);
    if (timeout_value) {
        int64_t remaining_ms = timeout_ms - (now_ms() - start_ms);
        if (remaining_ms < 0)
            remaining_ms = 0;
        timeout_value->tv_sec = remaining_ms / 1000;
        timeout_value->tv_usec = (remaining_ms % 1000) * 1000;
    }
    if (result < 0)
        return result;
    if (read_fds)
        FD_ZERO(read_fds);
    if (write_fds)
        FD_ZERO(write_fds);
    if (except_fds)
        FD_ZERO(except_fds);
    result = 0;
    for (nfds_t i = 0; i < count; ++i) {
        int event_mask = poll_fds[i].revents, fd = poll_fds[i].fd;
        if (event_mask & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        if (read_fds && (poll_fds[i].events & POLLIN) &&
            (event_mask & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(fd, read_fds);
            ++result;
        }
        if (write_fds && (poll_fds[i].events & POLLOUT) &&
            (event_mask & (POLLOUT | POLLHUP | POLLERR))) {
            FD_SET(fd, write_fds);
            ++result;
        }
        if (except_fds && (event_mask & POLLPRI)) {
            FD_SET(fd, except_fds);
            ++result;
        }
    }
    return result;
}

/* Ownership is independent of socket protocol and does not initialize DPDK. */
int epoll_create(int size)
{
    return initializing || fork_child_mode ? REAL(epoll_create, size) : nepoll_create(size);
}

int epoll_create1(int flags)
{
    return initializing || fork_child_mode ? REAL(epoll_create1, flags) : nepoll_create1(flags);
}

int epoll_ctl(int epoll_fd, int op, int fd, struct epoll_event *event)
{
    return nepoll_owned(epoll_fd) ? nepoll_ctl(epoll_fd, op, fd, event)
                                  : REAL(epoll_ctl, epoll_fd, op, fd, event);
}

int epoll_wait(int epoll_fd, struct epoll_event *events, int maxevents, int timeout)
{
    return nepoll_owned(epoll_fd) ? nepoll_wait(epoll_fd, events, maxevents, timeout)
                                  : REAL(epoll_wait, epoll_fd, events, maxevents, timeout);
}

int epoll_pwait(int epoll_fd, struct epoll_event *events, int maxevents, int timeout,
                const sigset_t *mask)
{
    return nepoll_owned(epoll_fd) ? nepoll_pwait(epoll_fd, events, maxevents, timeout, mask)
                                  : REAL(epoll_pwait, epoll_fd, events, maxevents, timeout, mask);
}

int close(int fd)
{
    if (nepoll_forget(fd))
        return 0;
    return managed(fd) ? nclose(fd) : REAL(close, fd);
}

/* Descriptor aliases need a shared open-file-description table. Reject them
 * explicitly instead of silently creating an eventfd alias with wrong I/O. */
int dup(int fd)
{
    if (managed(fd) || nepoll_owned(fd)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return REAL(dup, fd);
}

int dup2(int fd, int target)
{
    if (fd == target)
        return REAL(dup2, fd, target);
    if (managed(fd) || managed(target) || nepoll_owned(fd) || nepoll_owned(target)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return REAL(dup2, fd, target);
}

int dup3(int fd, int target, int flags)
{
    if (managed(fd) || managed(target) || nepoll_owned(fd) || nepoll_owned(target)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return REAL(dup3, fd, target, flags);
}

static void hook_fork_prepare(void)
{
    nepoll_fork_prepare();
    nfork_prepare();
}

static void hook_fork_parent(void)
{
    nfork_parent();
    nepoll_fork_parent();
}

static void hook_fork_child(void)
{
    nfork_child();

    fork_child_mode = 1;
    initializing = 0;
    nepoll_fork_child();
}

__attribute__((constructor)) static void hook_register_fork(void)
{
    pthread_atfork(hook_fork_prepare, hook_fork_parent, hook_fork_child);
}

__attribute__((destructor)) static void hook_drain_on_exit(void)
{
    if (!fork_child_mode)
        netstack_fini();
}
