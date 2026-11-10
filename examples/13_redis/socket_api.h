/* Internal API dispatch implementation; include after tcp_core.h and udp_core.h. */
#ifndef DPDKNET_SOCKET_API_H
#define DPDKNET_SOCKET_API_H

static ssize_t tcp_vector_io(int fd, const struct iovec *iovecs, int iov_count, int flags, int is_write)
{
    if (iov_count < 0 || iov_count > 1024 || (iov_count && !iovecs)) {
        errno = EINVAL;
        return -1;
    }
    size_t total_bytes = 0;
    for (int i = 0; i < iov_count; ++i) {
        if ((iovecs[i].iov_len && !iovecs[i].iov_base) ||
            iovecs[i].iov_len > (size_t)SSIZE_MAX - total_bytes) {
            errno = EINVAL;
            return -1;
        }
        total_bytes += iovecs[i].iov_len;
    }
    if (!total_bytes)
        return is_write ? tcp_send(fd, "", 0, flags) : tcp_recv(fd, NULL, 0, flags);
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
    ssize_t result = is_write ? tcp_send(fd, io_buffer, total_bytes, flags)
                              : tcp_recv(fd, io_buffer, total_bytes, flags);
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

static ssize_t tcp_sendmsg(int fd, const struct msghdr *message, int flags)
{
    if (!message) {
        errno = EFAULT;
        return -1;
    }
    if (message->msg_controllen || message->msg_name) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (message->msg_iovlen > 1024u) {
        errno = EMSGSIZE;
        return -1;
    }
    return tcp_vector_io(fd, message->msg_iov, message->msg_iovlen, flags, 1);
}

static ssize_t tcp_recvmsg(int fd, struct msghdr *message, int flags)
{
    if (!message) {
        errno = EFAULT;
        return -1;
    }
    if (message->msg_iovlen > 1024u) {
        errno = EMSGSIZE;
        return -1;
    }
    ssize_t result = tcp_vector_io(fd, message->msg_iov, message->msg_iovlen, flags, 0);
    if (result >= 0) {
        message->msg_flags = 0;
        message->msg_controllen = 0;
        if (message->msg_name)
            tcp_name(fd, message->msg_name, &message->msg_namelen, 1);
    }
    return result;
}

static ssize_t udp_send(int fd, const void *buffer, size_t len, int flags)
{
    return udp_sendto(fd, buffer, len, flags, NULL, 0);
}

static ssize_t udp_recv(int fd, void *buffer, size_t len, int flags)
{
    return udp_recvfrom(fd, buffer, len, flags, NULL, NULL, NULL);
}

static ssize_t udp_recvfrom_api(int fd, void *buffer, size_t len, int flags,
                                struct sockaddr *addr, socklen_t *addrlen)
{
    return udp_recvfrom(fd, buffer, len, flags, addr, addrlen, NULL);
}

static ssize_t tcp_sendto(int fd, const void *buffer, size_t len, int flags,
                          const struct sockaddr *addr, socklen_t addrlen)
{
    (void)addrlen;
    if (addr) {
        errno = EISCONN;
        return -1;
    }
    return tcp_send(fd, buffer, len, flags);
}

static ssize_t tcp_recvfrom(int fd, void *buffer, size_t len, int flags,
                            struct sockaddr *addr, socklen_t *addrlen)
{
    if (addr && !addrlen) {
        errno = EFAULT;
        return -1;
    }
    ssize_t result = tcp_recv(fd, buffer, len, flags);
    if (result >= 0 && addr && tcp_name(fd, addr, addrlen, 1) < 0)
        return -1;
    return result;
}

struct socket_ops {
    int (*bind)(int, const struct sockaddr *, socklen_t);
    int (*listen)(int, int);
    int (*accept)(int, struct sockaddr *, socklen_t *);
    int (*connect)(int, const struct sockaddr *, socklen_t);
    ssize_t (*send)(int, const void *, size_t, int);
    ssize_t (*recv)(int, void *, size_t, int);
    ssize_t (*sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
    ssize_t (*recvfrom)(int, void *, size_t, int, struct sockaddr *, socklen_t *);
    ssize_t (*sendmsg)(int, const struct msghdr *, int);
    ssize_t (*recvmsg)(int, struct msghdr *, int);
    int (*shutdown)(int, int);
    int (*close)(int);
    int (*name)(int, struct sockaddr *, socklen_t *, int);
    int (*fcntl)(int, int, long);
    int (*ioctl)(int, unsigned long, void *);
    int (*ready)(int);
    int (*setsockopt)(int, int, int, const void *, socklen_t);
    int (*getsockopt)(int, int, int, void *, socklen_t *);
};

static const struct socket_ops tcp_ops = {
    .bind = tcp_bind,
    .listen = tcp_listen,
    .accept = tcp_accept,
    .connect = tcp_connect,
    .send = tcp_send,
    .recv = tcp_recv,
    .sendto = tcp_sendto,
    .recvfrom = tcp_recvfrom,
    .sendmsg = tcp_sendmsg,
    .recvmsg = tcp_recvmsg,
    .shutdown = tcp_shutdown,
    .close = tcp_close,
    .name = tcp_name,
    .fcntl = tcp_fcntl,
    .ioctl = tcp_ioctl,
    .ready = tcp_ready,
    .setsockopt = tcp_setsockopt,
    .getsockopt = tcp_getsockopt,
};

static const struct socket_ops udp_ops = {
    .bind = udp_bind,
    /* Datagram sockets do not implement listen/accept. */
    .connect = udp_connect,
    .send = udp_send,
    .recv = udp_recv,
    .sendto = udp_sendto,
    .recvfrom = udp_recvfrom_api,
    .sendmsg = udp_sendmsg,
    .recvmsg = udp_recvmsg,
    .shutdown = udp_shutdown,
    .close = udp_close,
    .name = udp_name,
    .fcntl = udp_fcntl,
    .ioctl = udp_ioctl,
    .ready = udp_ready,
    .setsockopt = udp_setsockopt,
    .getsockopt = udp_getsockopt,
};

struct socket_protocol {
    int domain, type, protocol;
    int (*create)(int, int, int);
    const struct socket_ops *ops;
    void (*fork_child_reset)(void);
};

/* Immutable registry, indexed by the fd kind for O(1) operation lookup.
 * protocol == 0 selects the default for the matching domain/socket type. */
static const struct socket_protocol socket_protocols[STACK_SOCKET_KIND_COUNT] = {
    [STACK_SOCKET_TCP] = {AF_INET, SOCK_STREAM, IPPROTO_TCP, tcp_socket,
                          &tcp_ops, tcp_fork_child_reset},
    [STACK_SOCKET_UDP] = {AF_INET, SOCK_DGRAM, IPPROTO_UDP, udp_socket_create,
                          &udp_ops, udp_fork_child_reset},
};

static const struct socket_protocol *socket_protocol_find(int domain, int type, int protocol)
{
    int base_type = type & 15;
    for (size_t i = 1; i < STACK_SOCKET_KIND_COUNT; ++i) {
        const struct socket_protocol *entry = &socket_protocols[i];
        if (entry->domain == domain && entry->type == base_type &&
            (!protocol || protocol == entry->protocol))
            return entry;
    }
    return NULL;
}

int netstack_socket_supported(int domain, int type, int protocol)
{
    return socket_protocol_find(domain, type, protocol) != NULL;
}

static const struct socket_ops *socket_ops_for_fd(int fd)
{
    enum stack_socket_kind kind = socket_fd_kind(fd);
    if (kind <= STACK_SOCKET_NONE || kind >= STACK_SOCKET_KIND_COUNT) {
        errno = EBADF;
        return NULL;
    }
    return socket_protocols[kind].ops;
}

int nsocket(int domain, int type, int protocol)
{
    const struct socket_protocol *entry = socket_protocol_find(domain, type, protocol);
    if (!entry) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    return entry->create(domain, type, protocol);
}

void nfork_child(void)
{
    /* atfork prepare already holds socket_state_lock. No data-plane threads
     * survive in the child; inherited descriptors revert to kernel ownership. */
    for (size_t i = 1; i < STACK_SOCKET_KIND_COUNT; ++i)
        socket_protocols[i].fork_child_reset();
    socket_fork_child_reset();
}

int nbind(int fd, const struct sockaddr *addr, socklen_t len)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->bind(fd, addr, len);
}

int nlisten(int fd, int backlog)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    if (!ops->listen) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return ops->listen(fd, backlog);
}

int naccept(int fd, struct sockaddr *addr, socklen_t *len)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    if (!ops->accept) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return ops->accept(fd, addr, len);
}

int nconnect(int fd, const struct sockaddr *addr, socklen_t len)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->connect(fd, addr, len);
}

ssize_t nsend(int fd, const void *buffer, size_t len, int flags)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->send(fd, buffer, len, flags);
}

ssize_t nrecv(int fd, void *buffer, size_t len, int flags)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->recv(fd, buffer, len, flags);
}

ssize_t nsendto(int fd, const void *buffer, size_t len, int flags,
                const struct sockaddr *addr, socklen_t addrlen)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->sendto(fd, buffer, len, flags, addr, addrlen);
}

ssize_t nrecvfrom(int fd, void *buffer, size_t len, int flags,
                  struct sockaddr *addr, socklen_t *addrlen)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->recvfrom(fd, buffer, len, flags, addr, addrlen);
}

ssize_t nsendmsg(int fd, const struct msghdr *message, int flags)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->sendmsg(fd, message, flags);
}

ssize_t nrecvmsg(int fd, struct msghdr *message, int flags)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->recvmsg(fd, message, flags);
}

int nshutdown(int fd, int how)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->shutdown(fd, how);
}

int nclose(int fd)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->close(fd);
}

int ngetsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->name(fd, addr, len, 0);
}

int ngetpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->name(fd, addr, len, 1);
}

int nfcntl(int fd, int command, long argument)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->fcntl(fd, command, argument);
}

int nioctl(int fd, unsigned long request, void *argument)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->ioctl(fd, request, argument);
}

int nsetsockopt(int fd, int level, int option, const void *value, socklen_t len)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->setsockopt(fd, level, option, value, len);
}

int ngetsockopt(int fd, int level, int option, void *value, socklen_t *len)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    if (!ops)
        return -1;
    return ops->getsockopt(fd, level, option, value, len);
}

int nready(int fd)
{
    const struct socket_ops *ops = socket_ops_for_fd(fd);
    /* Preserve the readiness API's event-mask result for invalid descriptors. */
    return ops ? ops->ready(fd) : EPOLLERR;
}

/* Retain existing exported datagram-only entry points for compatibility. */
ssize_t ndgram_sendmsg(int fd, const struct msghdr *message, int flags)
{
    if (!nisdgram(fd)) {
        errno = EBADF;
        return -1;
    }
    return nsendmsg(fd, message, flags);
}

ssize_t ndgram_recvmsg(int fd, struct msghdr *message, int flags)
{
    if (!nisdgram(fd)) {
        errno = EBADF;
        return -1;
    }
    return nrecvmsg(fd, message, flags);
}

#endif
