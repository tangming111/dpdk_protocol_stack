/* IPv4 UDP engine; shares descriptor management and locking via socket_core.h. */
#ifndef DPDKNET_UDP_CORE_H
#define DPDKNET_UDP_CORE_H
#include "socket_core.h"
#define UDP_MAX_PAYLOAD 1472u
#define UDP_QUEUE_BYTES (1024u * 1024u)
#define UDP_QUEUE_PACKETS 1024u
#define UDP_ARP_TIMEOUT_MS 3000u
struct udp_datagram {
    struct udp_datagram *next;
    /* RX: source endpoint; TX: destination endpoint, in network byte order. */
    uint32_t peer_ip;
    uint16_t peer_port;
    size_t payload_len;
    uint64_t queued_at_ms;
    unsigned char data[];
};
struct udp_socket {
    struct udp_socket *next, *prev;
    int fd, flags, connected, reuse_address, error, read_shutdown, write_shutdown;
    int tos, ttl, path_mtu_discovery;
    uint32_t local_ip, peer_ip;
    uint16_t local_port, peer_port;
    size_t recv_queue_bytes, send_queue_bytes, recv_queue_limit_bytes, send_queue_limit_bytes;
    unsigned recv_datagram_count, send_datagram_count;
    struct udp_datagram *recv_head, *recv_tail, *send_head, *send_tail;
    uint64_t last_arp_request_ms;
};
static struct udp_socket *udp_sockets;
static void udp_fork_child_reset(void)
{
    udp_sockets = NULL;
}

static struct udp_socket *udp_find(int fd)
{
    struct udp_socket *udp_endpoint;
    for (udp_endpoint = udp_sockets; udp_endpoint; udp_endpoint = udp_endpoint->next)
        if (udp_endpoint->fd == fd)
            return udp_endpoint;
    return NULL;
}

static void udp_free_queue(struct udp_datagram *datagram)
{
    while (datagram) {
        struct udp_datagram *next = datagram->next;
        rte_free(datagram);
        datagram = next;
    }
}

static void udp_destroy(struct udp_socket *udp_endpoint)
{
    udp_free_queue(udp_endpoint->recv_head);
    udp_free_queue(udp_endpoint->send_head);
    LL_REMOVE(udp_endpoint, udp_sockets);
    rte_free(udp_endpoint);
}

static int udp_socket_create(int domain, int type, int protocol)
{
    if (stack_fork_child) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (domain != AF_INET || (type & 15) != SOCK_DGRAM || (protocol && protocol != IPPROTO_UDP)) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    int fd = socket_fd_alloc(STACK_SOCKET_UDP);
    if (fd >= 0) {
        struct udp_socket *udp_endpoint = rte_zmalloc("udp_socket", sizeof(*udp_endpoint), 0);
        if (!udp_endpoint) {
            socket_fd_release(fd);
            fd = -1;
            errno = ENOMEM;
        } else {
            udp_endpoint->fd = fd;
            udp_endpoint->flags = (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0;
            udp_endpoint->recv_queue_limit_bytes = udp_endpoint->send_queue_limit_bytes =
                UDP_QUEUE_BYTES;
            udp_endpoint->ttl = 64;
            udp_endpoint->path_mtu_discovery = IP_PMTUDISC_DO;
            syscall(SYS_fcntl, fd, F_SETFD, (type & SOCK_CLOEXEC) ? FD_CLOEXEC : 0);
            LL_ADD(udp_endpoint, udp_sockets);
        }
    }
    pthread_mutex_unlock(&socket_state_lock);
    return fd;
}

static int udp_autobind(struct udp_socket *udp_endpoint)
{
    static uint16_t next_port = 49151;
    if (udp_endpoint->local_port)
        return 0;
    for (unsigned i = 0; i < 16384; ++i) {
        if (++next_port < 49152)
            next_port = 49152;
        struct udp_socket *candidate_endpoint;
        for (candidate_endpoint = udp_sockets; candidate_endpoint;
             candidate_endpoint = candidate_endpoint->next)
            if (candidate_endpoint != udp_endpoint && candidate_endpoint->fd >= 0 &&
                candidate_endpoint->local_port == htons(next_port))
                break;
        if (!candidate_endpoint) {
            udp_endpoint->local_port = htons(next_port);
            return 0;
        }
    }
    errno = EADDRINUSE;
    return -1;
}

static int udp_bind(int fd, const struct sockaddr *addr, socklen_t address_len)
{
    if (!addr || address_len < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    const struct sockaddr_in *socket_addr = (const void *)addr;
    if (socket_addr->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (socket_addr->sin_addr.s_addr && socket_addr->sin_addr.s_addr != g_stack_ipv4_addr) {
        errno = EADDRNOTAVAIL;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int result = -1;
    if (!udp_endpoint)
        errno = EBADF;
    else if (udp_endpoint->local_port)
        errno = EINVAL;
    else {
        struct udp_socket *candidate_endpoint;
        for (candidate_endpoint = udp_sockets; candidate_endpoint;
             candidate_endpoint = candidate_endpoint->next)
            if (candidate_endpoint != udp_endpoint && candidate_endpoint->fd >= 0 &&
                socket_addr->sin_port && candidate_endpoint->local_port == socket_addr->sin_port &&
                (!candidate_endpoint->local_ip || !socket_addr->sin_addr.s_addr ||
                 candidate_endpoint->local_ip == socket_addr->sin_addr.s_addr) &&
                !(candidate_endpoint->reuse_address && udp_endpoint->reuse_address))
                break;
        if (candidate_endpoint)
            errno = EADDRINUSE;
        else {
            udp_endpoint->local_ip = socket_addr->sin_addr.s_addr;
            udp_endpoint->local_port = socket_addr->sin_port;
            result = udp_autobind(udp_endpoint);
        }
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int udp_destination(const struct sockaddr *addr, socklen_t address_len, uint32_t *peer_ip,
                           uint16_t *peer_port)
{
    if (!addr || address_len < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    const struct sockaddr_in *socket_addr = (const void *)addr;
    if (socket_addr->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    uint32_t host = ntohl(socket_addr->sin_addr.s_addr);
    if (!host || !socket_addr->sin_port) {
        errno = EINVAL;
        return -1;
    }
    if (host == 0xffffffffu || (host >> 28) == 14) {
        errno = EOPNOTSUPP;
        return -1;
    }
    *peer_ip = socket_addr->sin_addr.s_addr;
    *peer_port = socket_addr->sin_port;
    return 0;
}

static int udp_connect(int fd, const struct sockaddr *addr, socklen_t address_len)
{
    if (!addr || address_len < sizeof(sa_family_t)) {
        errno = EINVAL;
        return -1;
    }
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    int disconnect = addr->sa_family == AF_UNSPEC;
    if (!disconnect && udp_destination(addr, address_len, &peer_ip, &peer_port) < 0)
        return -1;
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int result = -1;
    if (!udp_endpoint)
        errno = EBADF;
    else if (disconnect || udp_autobind(udp_endpoint) == 0) {
        udp_endpoint->connected = !disconnect;
        udp_endpoint->peer_ip = peer_ip;
        udp_endpoint->peer_port = peer_port;
        if (!disconnect && !udp_endpoint->local_ip)
            udp_endpoint->local_ip = g_stack_ipv4_addr;
        udp_endpoint->error = 0;
        result = 0;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int udp_name(int fd, struct sockaddr *addr, socklen_t *address_len, int peer)
{
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int result = -1;
    if (!udp_endpoint)
        errno = EBADF;
    else if (peer && !udp_endpoint->connected)
        errno = ENOTCONN;
    else
        result =
            socket_copy_addr(addr, address_len, peer ? udp_endpoint->peer_ip : udp_endpoint->local_ip,
                          peer ? udp_endpoint->peer_port : udp_endpoint->local_port);
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static ssize_t udp_sendto(int fd, const void *buffer, size_t buffer_len, int flags,
                          const struct sockaddr *addr, socklen_t address_len)
{
    if (flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (buffer_len > UDP_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    if (buffer_len && !buffer) {
        errno = EFAULT;
        return -1;
    }
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    if (addr && udp_destination(addr, address_len, &peer_ip, &peer_port) < 0)
        return -1;
    pthread_mutex_lock(&socket_state_lock);
    ssize_t result = -1;
    for (;;) {
        struct udp_socket *udp_endpoint = udp_find(fd);
        if (!udp_endpoint) {
            errno = EBADF;
            break;
        }
        if (udp_endpoint->error) {
            errno = udp_endpoint->error;
            udp_endpoint->error = 0;
            break;
        }
        if (udp_endpoint->write_shutdown) {
            errno = EPIPE;
            break;
        }
        if (!addr) {
            if (!udp_endpoint->connected) {
                errno = EDESTADDRREQ;
                break;
            }
            peer_ip = udp_endpoint->peer_ip;
            peer_port = udp_endpoint->peer_port;
        }
        if (udp_autobind(udp_endpoint) < 0)
            break;
        if (udp_endpoint->send_datagram_count < UDP_QUEUE_PACKETS &&
            udp_endpoint->send_queue_bytes + buffer_len <= udp_endpoint->send_queue_limit_bytes) {
            struct udp_datagram *datagram = rte_malloc("udp_tx", sizeof(*datagram) + buffer_len, 0);
            if (!datagram) {
                errno = ENOMEM;
                break;
            }
            datagram->next = NULL;
            datagram->peer_ip = peer_ip;
            datagram->peer_port = peer_port;
            datagram->payload_len = buffer_len;
            datagram->queued_at_ms = socket_now();
            if (buffer_len)
                memcpy(datagram->data, buffer, buffer_len);
            if (udp_endpoint->send_tail)
                udp_endpoint->send_tail->next = datagram;
            else
                udp_endpoint->send_head = datagram;
            udp_endpoint->send_tail = datagram;
            udp_endpoint->send_queue_bytes += buffer_len;
            ++udp_endpoint->send_datagram_count;
            result = buffer_len;
            break;
        }
        if ((udp_endpoint->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) {
            errno = EAGAIN;
            break;
        }
        socket_pause();
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static ssize_t udp_recvfrom(int fd, void *buffer, size_t buffer_len, int flags,
                            struct sockaddr *addr, socklen_t *address_len, int *outflags)
{
    if (flags & ~(MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC | MSG_WAITALL)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if ((buffer_len && !buffer) || (addr && !address_len)) {
        errno = EFAULT;
        return -1;
    }
    if (outflags)
        *outflags = 0;
    pthread_mutex_lock(&socket_state_lock);
    ssize_t result = -1;
    for (;;) {
        struct udp_socket *udp_endpoint = udp_find(fd);
        if (!udp_endpoint) {
            errno = EBADF;
            break;
        }
        if (udp_endpoint->recv_head) {
            struct udp_datagram *datagram = udp_endpoint->recv_head;
            size_t copy = buffer_len < datagram->payload_len ? buffer_len : datagram->payload_len;
            if (copy)
                memcpy(buffer, datagram->data, copy);
            if (addr)
                socket_copy_addr(addr, address_len, datagram->peer_ip, datagram->peer_port);
            if (outflags && buffer_len < datagram->payload_len)
                *outflags = MSG_TRUNC;
            result = (flags & MSG_TRUNC) ? datagram->payload_len : copy;
            if (!(flags & MSG_PEEK)) {
                udp_endpoint->recv_head = datagram->next;
                if (!udp_endpoint->recv_head)
                    udp_endpoint->recv_tail = NULL;
                udp_endpoint->recv_queue_bytes -= datagram->payload_len;
                --udp_endpoint->recv_datagram_count;
                rte_free(datagram);
            }
            break;
        }
        if (udp_endpoint->error) {
            errno = udp_endpoint->error;
            udp_endpoint->error = 0;
            break;
        }
        if (udp_endpoint->read_shutdown) {
            result = 0;
            break;
        }
        if ((udp_endpoint->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) {
            errno = EAGAIN;
            break;
        }
        socket_pause();
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int ng_udp_process(struct rte_mbuf *packet)
{
    unsigned ethernet_header_len = sizeof(struct rte_ether_hdr),
             available_bytes = rte_pktmbuf_data_len(packet);
    if (available_bytes < ethernet_header_len + 28) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    struct rte_ipv4_hdr *ipv4_header =
        rte_pktmbuf_mtod_offset(packet, struct rte_ipv4_hdr *, ethernet_header_len);
    unsigned ipv4_packet_len = ntohs(ipv4_header->total_length);
    if (ipv4_header->version_ihl != 0x45 || ipv4_header->next_proto_id != IPPROTO_UDP ||
        ipv4_packet_len < 28 || ipv4_packet_len > available_bytes - ethernet_header_len ||
        (ntohs(ipv4_header->fragment_offset) & 0x3fff) ||
        ipv4_header->dst_addr != g_stack_ipv4_addr || !socket_ipv4_valid(ipv4_header)) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    struct rte_udp_hdr *udp_header = (void *)(ipv4_header + 1);
    unsigned udp_packet_len = ntohs(udp_header->dgram_len);
    if (udp_packet_len < sizeof(*udp_header) || udp_packet_len != ipv4_packet_len - 20 ||
        udp_packet_len - sizeof(*udp_header) > UDP_MAX_PAYLOAD) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    if (udp_header->dgram_cksum) {
        uint16_t received_checksum = udp_header->dgram_cksum;
        udp_header->dgram_cksum = 0;
        uint16_t computed_checksum = rte_ipv4_udptcp_cksum(ipv4_header, udp_header);
        udp_header->dgram_cksum = received_checksum;
        if (received_checksum != computed_checksum) {
            rte_pktmbuf_free(packet);
            return -1;
        }
    }
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *matched_endpoint = NULL, *candidate_endpoint;
    int best_match_score = -1;
    for (candidate_endpoint = udp_sockets; candidate_endpoint;
         candidate_endpoint = candidate_endpoint->next) {
        if (candidate_endpoint->fd < 0 || candidate_endpoint->read_shutdown ||
            candidate_endpoint->local_port != udp_header->dst_port ||
            (candidate_endpoint->local_ip && candidate_endpoint->local_ip != ipv4_header->dst_addr))
            continue;
        if (candidate_endpoint->connected &&
            (candidate_endpoint->peer_ip != ipv4_header->src_addr ||
             candidate_endpoint->peer_port != udp_header->src_port))
            continue;
        int match_score =
            (candidate_endpoint->connected ? 4 : 0) + (candidate_endpoint->local_ip ? 1 : 0);
        if (match_score > best_match_score) {
            matched_endpoint = candidate_endpoint;
            best_match_score = match_score;
        }
    }
    size_t payload_len = udp_packet_len - sizeof(*udp_header);
    if (matched_endpoint && matched_endpoint->recv_datagram_count < UDP_QUEUE_PACKETS &&
        matched_endpoint->recv_queue_bytes + payload_len <=
            matched_endpoint->recv_queue_limit_bytes) {
        struct udp_datagram *datagram = rte_malloc("udp_rx", sizeof(*datagram) + payload_len, 0);
        if (datagram) {
            datagram->next = NULL;
            datagram->peer_ip = ipv4_header->src_addr;
            datagram->peer_port = udp_header->src_port;
            datagram->payload_len = payload_len;
            datagram->queued_at_ms = socket_now();
            if (payload_len)
                memcpy(datagram->data, udp_header + 1, payload_len);
            if (matched_endpoint->recv_tail)
                matched_endpoint->recv_tail->next = datagram;
            else
                matched_endpoint->recv_head = datagram;
            matched_endpoint->recv_tail = datagram;
            matched_endpoint->recv_queue_bytes += payload_len;
            ++matched_endpoint->recv_datagram_count;
        }
    }
    pthread_mutex_unlock(&socket_state_lock);
    rte_pktmbuf_free(packet);
    return 0;
}

static int udp_emit(struct udp_socket *udp_endpoint, struct udp_datagram *datagram,
                    struct rte_mempool *packet_pool)
{
    uint8_t *peer_mac = ng_get_dst_macaddr(datagram->peer_ip);
    struct inout_ring *ring = ring_instance();
    if (!peer_mac) {
        uint64_t now_ms = socket_now();
        if (now_ms - udp_endpoint->last_arp_request_ms >= 1000) {
            struct rte_mbuf *arp_packet =
                ng_send_arp(packet_pool, RTE_ARP_OP_REQUEST, g_arp_broadcast_mac,
                            udp_endpoint->local_ip ? udp_endpoint->local_ip : g_stack_ipv4_addr,
                            datagram->peer_ip);
            if (arp_packet && rte_ring_mp_enqueue(ring->out, arp_packet) < 0)
                rte_pktmbuf_free(arp_packet);
            udp_endpoint->last_arp_request_ms = now_ms;
        }
        return -1;
    }
    struct rte_mbuf *packet = rte_pktmbuf_alloc(packet_pool);
    if (!packet)
        return -1;
    unsigned frame_len = sizeof(struct rte_ether_hdr) + 28 + datagram->payload_len;
    unsigned char *frame_data = (void *)rte_pktmbuf_append(packet, frame_len);
    if (!frame_data) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    memset(frame_data, 0, frame_len);
    struct rte_ether_hdr *ethernet_header = (void *)frame_data;
    memcpy(ethernet_header->s_addr.addr_bytes, g_port_mac_addr, 6);
    memcpy(ethernet_header->d_addr.addr_bytes, peer_mac, 6);
    ethernet_header->ether_type = htons(RTE_ETHER_TYPE_IPV4);
    struct rte_ipv4_hdr *ipv4_header = (void *)(ethernet_header + 1);
    ipv4_header->version_ihl = 0x45;
    ipv4_header->total_length = htons(28 + datagram->payload_len);
    ipv4_header->fragment_offset =
        htons(udp_endpoint->path_mtu_discovery == IP_PMTUDISC_DO ? 0x4000 : 0);
    ipv4_header->time_to_live = udp_endpoint->ttl;
    ipv4_header->type_of_service = udp_endpoint->tos;
    ipv4_header->next_proto_id = IPPROTO_UDP;
    ipv4_header->src_addr = udp_endpoint->local_ip ? udp_endpoint->local_ip : g_stack_ipv4_addr;
    ipv4_header->dst_addr = datagram->peer_ip;
    ipv4_header->hdr_checksum = rte_ipv4_cksum(ipv4_header);
    struct rte_udp_hdr *udp_header = (void *)(ipv4_header + 1);
    udp_header->src_port = udp_endpoint->local_port;
    udp_header->dst_port = datagram->peer_port;
    udp_header->dgram_len = htons(8 + datagram->payload_len);
    if (datagram->payload_len)
        memcpy(udp_header + 1, datagram->data, datagram->payload_len);
    udp_header->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_header, udp_header);
    if (rte_ring_mp_enqueue(ring->out, packet) < 0) {
        rte_pktmbuf_free(packet);
        return -1;
    }
    return 0;
}

static void ng_udp_out(struct rte_mempool *packet_pool)
{
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_sockets;
    while (udp_endpoint) {
        struct udp_socket *next = udp_endpoint->next;
        unsigned burst = 32;
        while (udp_endpoint->send_head && burst--) {
            struct udp_datagram *datagram = udp_endpoint->send_head;
            int expired = !ng_get_dst_macaddr(datagram->peer_ip) &&
                          socket_now() - datagram->queued_at_ms >= UDP_ARP_TIMEOUT_MS;
            if (!expired && udp_emit(udp_endpoint, datagram, packet_pool) < 0)
                break;
            if (expired)
                udp_endpoint->error = EHOSTUNREACH;
            udp_endpoint->send_head = datagram->next;
            if (!udp_endpoint->send_head)
                udp_endpoint->send_tail = NULL;
            udp_endpoint->send_queue_bytes -= datagram->payload_len;
            --udp_endpoint->send_datagram_count;
            rte_free(datagram);
        }
        if (udp_endpoint->fd < 0 && !udp_endpoint->send_head)
            udp_destroy(udp_endpoint);
        udp_endpoint = next;
    }
    pthread_mutex_unlock(&socket_state_lock);
}

static int udp_close(int fd)
{
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int result = -1;
    if (!udp_endpoint)
        errno = EBADF;
    else {
        udp_endpoint->fd = -1;
        udp_free_queue(udp_endpoint->recv_head);
        udp_endpoint->recv_head = udp_endpoint->recv_tail = NULL;
        udp_endpoint->recv_datagram_count = udp_endpoint->recv_queue_bytes = 0;
        result = socket_fd_release(fd);
        if (!udp_endpoint->send_head)
            udp_destroy(udp_endpoint);
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int udp_ready(int fd)
{
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int events = EPOLLERR;
    if (udp_endpoint) {
        events = 0;
        if (udp_endpoint->recv_head || udp_endpoint->read_shutdown)
            events |= EPOLLIN;
        if (!udp_endpoint->write_shutdown &&
            udp_endpoint->send_datagram_count < UDP_QUEUE_PACKETS &&
            udp_endpoint->send_queue_bytes + UDP_MAX_PAYLOAD <=
                udp_endpoint->send_queue_limit_bytes)
            events |= EPOLLOUT;
        if (udp_endpoint->error)
            events |= EPOLLERR;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return events;
}

static int udp_fcntl(int fd, int cmd, long arg)
{
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int result = -1;
    if (!udp_endpoint)
        errno = EBADF;
    else
        switch (cmd) {
            case F_GETFL:
                result = O_RDWR | udp_endpoint->flags;
                break;
            case F_SETFL:
                udp_endpoint->flags = arg & O_NONBLOCK;
                result = 0;
                break;
            case F_GETFD:
            case F_SETFD:
                result = syscall(SYS_fcntl, fd, cmd, arg);
                break;
            default:
                errno = EOPNOTSUPP;
        }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int udp_ioctl(int fd, unsigned long request, void *arg)
{
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int result = -1;
    if (!udp_endpoint)
        errno = EBADF;
    else if (!arg)
        errno = EFAULT;
    else if (request == FIONBIO) {
        udp_endpoint->flags = *(int *)arg ? O_NONBLOCK : 0;
        result = 0;
    } else if (request == FIONREAD) {
        *(int *)arg = udp_endpoint->recv_head ? udp_endpoint->recv_head->payload_len : 0;
        result = 0;
    } else
        errno = ENOTTY;
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int udp_shutdown(int fd, int how)
{
    if (how < SHUT_RD || how > SHUT_RDWR) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int result = -1;
    if (!udp_endpoint)
        errno = EBADF;
    else if (!udp_endpoint->connected)
        errno = ENOTCONN;
    else {
        if (how != SHUT_WR) {
            udp_endpoint->read_shutdown = 1;
            udp_free_queue(udp_endpoint->recv_head);
            udp_endpoint->recv_head = udp_endpoint->recv_tail = NULL;
            udp_endpoint->recv_queue_bytes = udp_endpoint->recv_datagram_count = 0;
        }
        if (how != SHUT_RD)
            udp_endpoint->write_shutdown = 1;
        result = 0;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int udp_setsockopt(int fd, int level, int opt, const void *value, socklen_t option_len)
{
    if (!value || option_len < sizeof(int)) {
        errno = EINVAL;
        return -1;
    }
    int option_value;
    memcpy(&option_value, value, sizeof(option_value));
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int result = 0;
    if (!udp_endpoint) {
        errno = EBADF;
        result = -1;
    } else if (level == SOL_SOCKET && opt == SO_REUSEADDR)
        udp_endpoint->reuse_address = !!option_value;
    else if (level == SOL_SOCKET && (opt == SO_SNDBUF || opt == SO_RCVBUF)) {
        if (option_value <= 0) {
            errno = EINVAL;
            result = -1;
        } else {
            size_t queue_limit_bytes = option_value < 4096 ? 4096u : option_value > 4 * 1024 * 1024
                                                                         ? 4u * 1024 * 1024
                                                                         : (unsigned)option_value;
            if (opt == SO_SNDBUF)
                udp_endpoint->send_queue_limit_bytes = queue_limit_bytes;
            else
                udp_endpoint->recv_queue_limit_bytes = queue_limit_bytes;
        }
    } else if (level == IPPROTO_IP && opt == IP_TOS && option_value >= 0 && option_value <= 255)
        udp_endpoint->tos = option_value;
    else if (level == IPPROTO_IP && opt == IP_TTL && option_value > 0 && option_value <= 255)
        udp_endpoint->ttl = option_value;
    else if (level == IPPROTO_IP && opt == IP_MTU_DISCOVER && option_value >= IP_PMTUDISC_DONT &&
             option_value <= IP_PMTUDISC_DO)
        udp_endpoint->path_mtu_discovery = option_value;
    else {
        errno = ENOPROTOOPT;
        result = -1;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int udp_getsockopt(int fd, int level, int opt, void *value, socklen_t *option_len)
{
    if (!value || !option_len) {
        errno = EFAULT;
        return -1;
    }
    pthread_mutex_lock(&socket_state_lock);
    struct udp_socket *udp_endpoint = udp_find(fd);
    int result = 0, option_value = 0;
    if (!udp_endpoint) {
        errno = EBADF;
        result = -1;
    } else if (level == SOL_SOCKET) {
        switch (opt) {
            case SO_TYPE:
                option_value = SOCK_DGRAM;
                break;
            case SO_ERROR:
                option_value = udp_endpoint->error;
                udp_endpoint->error = 0;
                break;
            case SO_REUSEADDR:
                option_value = udp_endpoint->reuse_address;
                break;
            case SO_SNDBUF:
                option_value = udp_endpoint->send_queue_limit_bytes;
                break;
            case SO_RCVBUF:
                option_value = udp_endpoint->recv_queue_limit_bytes;
                break;
            case SO_ACCEPTCONN:
                option_value = 0;
                break;
            default:
                errno = ENOPROTOOPT;
                result = -1;
        }
    } else if (level == IPPROTO_IP && opt == IP_TOS)
        option_value = udp_endpoint->tos;
    else if (level == IPPROTO_IP && opt == IP_TTL)
        option_value = udp_endpoint->ttl;
    else if (level == IPPROTO_IP && opt == IP_MTU_DISCOVER)
        option_value = udp_endpoint->path_mtu_discovery;
    else if (level == IPPROTO_IP && opt == IP_MTU && udp_endpoint->connected)
        option_value = 1500;
    else {
        errno = ENOPROTOOPT;
        result = -1;
    }
    if (!result) {
        socklen_t copy = *option_len < sizeof(option_value) ? *option_len : sizeof(option_value);
        memcpy(value, &option_value, copy);
        *option_len = copy;
    }
    pthread_mutex_unlock(&socket_state_lock);
    return result;
}

static int udp_iov_size(const struct msghdr *message, size_t *total_bytes)
{
    if (!message || (message->msg_iovlen && !message->msg_iov)) {
        errno = EFAULT;
        return -1;
    }
    if (message->msg_iovlen > 1024u) {
        errno = EMSGSIZE;
        return -1;
    }
    *total_bytes = 0;
    for (size_t i = 0; i < message->msg_iovlen; ++i) {
        size_t datagram_len = message->msg_iov[i].iov_len;
        if ((datagram_len && !message->msg_iov[i].iov_base) ||
            datagram_len > (size_t)SSIZE_MAX - *total_bytes) {
            errno = EINVAL;
            return -1;
        }
        *total_bytes += datagram_len;
    }
    return 0;
}

static ssize_t udp_sendmsg(int fd, const struct msghdr *message, int flags)
{
    size_t size;
    if (udp_iov_size(message, &size) < 0)
        return -1;
    if (message->msg_controllen) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (size > UDP_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    unsigned char datagram_buffer[UDP_MAX_PAYLOAD];
    size_t offset = 0;
    for (size_t i = 0; i < message->msg_iovlen; ++i) {
        size_t iov_bytes = message->msg_iov[i].iov_len;
        if (iov_bytes)
            memcpy(datagram_buffer + offset, message->msg_iov[i].iov_base, iov_bytes);
        offset += iov_bytes;
    }
    return udp_sendto(fd, datagram_buffer, size, flags, message->msg_name, message->msg_namelen);
}

static ssize_t udp_recvmsg(int fd, struct msghdr *message, int flags)
{
    size_t size;
    if (udp_iov_size(message, &size) < 0)
        return -1;
    if (size > UDP_MAX_PAYLOAD)
        size = UDP_MAX_PAYLOAD;
    unsigned char datagram_buffer[UDP_MAX_PAYLOAD];
    int outflags;
    ssize_t result = udp_recvfrom(fd, datagram_buffer, size, flags, message->msg_name,
                                  &message->msg_namelen, &outflags);
    if (result < 0)
        return result;
    size_t available = (size_t)result < size ? (size_t)result : size, offset = 0;
    for (size_t i = 0; i < message->msg_iovlen && offset < available; ++i) {
        size_t iov_bytes = message->msg_iov[i].iov_len;
        if (iov_bytes > available - offset)
            iov_bytes = available - offset;
        if (iov_bytes)
            memcpy(message->msg_iov[i].iov_base, datagram_buffer + offset, iov_bytes);
        offset += iov_bytes;
    }
    message->msg_flags = outflags;
    message->msg_controllen = 0;
    if (!message->msg_name)
        message->msg_namelen = 0;
    return result;
}

#endif
