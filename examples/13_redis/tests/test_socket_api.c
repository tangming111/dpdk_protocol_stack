/* Exercise the production public API, registry and both real protocol engines.
 * Reverse engine inclusion also guards against UDP depending on TCP internals. */
#include "packet_test_support.h"
#include "../udp_core.h"
#include "../tcp_core.h"
#include "../socket_api.h"

static struct sockaddr_in address(uint32_t ip, uint16_t port)
{
    struct sockaddr_in socket_addr;
    memset(&socket_addr, 0, sizeof(socket_addr));
    socket_addr.sin_family = AF_INET;
    socket_addr.sin_addr.s_addr = ip;
    socket_addr.sin_port = htons(port);
    return socket_addr;
}

static int inject(uint16_t local, uint32_t peer, uint16_t peer_port, const void *data, size_t n,
                  int corrupt)
{
    struct rte_mbuf *packet = calloc(1, sizeof(*packet));
    packet->size = 42 + n;
    struct rte_ipv4_hdr *ipv4_header = (void *)(packet->data + 14);
    ipv4_header->version_ihl = 0x45;
    ipv4_header->next_proto_id = IPPROTO_UDP;
    ipv4_header->total_length = htons(28 + n);
    ipv4_header->src_addr = peer;
    ipv4_header->dst_addr = g_stack_ipv4_addr;
    ipv4_header->hdr_checksum = rte_ipv4_cksum(ipv4_header);
    struct rte_udp_hdr *udp_header = (void *)(ipv4_header + 1);
    udp_header->src_port = htons(peer_port);
    udp_header->dst_port = local;
    udp_header->dgram_len = htons(8 + n);
    if (n)
        memcpy(udp_header + 1, data, n);
    udp_header->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_header, udp_header);
    if (corrupt == 1)
        udp_header->dgram_cksum ^= 0x101;
    if (corrupt == 2)
        udp_header->dgram_cksum = 0; /* Optional in IPv4. */
    if (corrupt == 3)
        udp_header->dgram_len = htons(7);
    return ng_udp_process(packet);
}

static void input(struct ng_tcp_stream *tcp_stream, uint32_t seq, uint32_t ack, unsigned flags,
                  unsigned wnd, const void *data, unsigned n)
{
    struct rte_mbuf *packet = calloc(1, sizeof(*packet));
    packet->size = 54 + n;
    struct rte_ipv4_hdr *ipv4_header = (void *)(packet->data + 14);
    ipv4_header->version_ihl = 0x45;
    ipv4_header->total_length = htons(40 + n);
    ipv4_header->next_proto_id = IPPROTO_TCP;
    ipv4_header->src_addr = tcp_stream->peer_ip;
    ipv4_header->dst_addr = tcp_stream->local_ip;
    ipv4_header->hdr_checksum = rte_ipv4_cksum(ipv4_header);
    struct rte_tcp_hdr *tcp_header = (void *)(packet->data + 54 - sizeof(*tcp_header));
    tcp_header->src_port = tcp_stream->peer_port;
    tcp_header->dst_port = tcp_stream->local_port;
    tcp_header->sent_seq = htonl(seq);
    tcp_header->recv_ack = htonl(ack);
    tcp_header->data_off = 0x50;
    tcp_header->tcp_flags = flags;
    tcp_header->rx_win = htons(wnd);
    if (n)
        memcpy(tcp_header + 1, data, n);
    tcp_header->cksum = rte_ipv4_udptcp_cksum(ipv4_header, tcp_header);
    ng_tcp_process(packet);
}

static void clear_output(void)
{
    while (output.count)
        free(output.items[--output.count]);
}

static void test_registry_and_invalid_fd(void)
{
    assert(netstack_socket_supported(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    assert(netstack_socket_supported(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP));
    assert(!netstack_socket_supported(AF_INET, SOCK_STREAM, IPPROTO_UDP));
    assert(!netstack_socket_supported(AF_INET, SOCK_DGRAM, IPPROTO_TCP));
    assert(!netstack_socket_supported(AF_UNSPEC, SOCK_STREAM, 0));
    assert(!netstack_socket_supported(AF_INET, 15, 0));
    assert(nsocket(AF_INET, SOCK_STREAM, IPPROTO_UDP) == -1 && errno == EPROTONOSUPPORT);
    assert(nsocket(AF_UNSPEC, SOCK_DGRAM, 0) == -1 && errno == EPROTONOSUPPORT);
    assert(nbind(-1, NULL, 0) == -1 && errno == EBADF);
    assert(nlisten(STACK_MAX_FDS, 1) == -1 && errno == EBADF);
    assert(naccept(-1, NULL, NULL) == -1 && errno == EBADF);
    assert(nsend(-1, NULL, 0, 0) == -1 && errno == EBADF);
    assert(nrecvmsg(-1, NULL, 0) == -1 && errno == EBADF);
    assert(nclose(-1) == -1 && errno == EBADF);
    assert(nready(-1) == EPOLLERR);
    assert(!nisdgram(-1) && !netstack_fd_is_managed(STACK_MAX_FDS));
    puts("socket_api: protocol selection, creation flags and invalid descriptors passed");
}

static void test_udp_api(void)
{
    uint32_t peer = htonl(0xc0000201);
    struct sockaddr_in local = address(0, 19101), remote = address(peer, 19102), source;
    int fd = nsocket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    assert(fd >= 0 && nisdgram(fd) && netstack_fd_is_managed(fd));
    assert(nfcntl(fd, F_GETFL, 0) & O_NONBLOCK);
    assert(nbind(fd, (void *)&local, sizeof(local)) == 0);
    socklen_t size = sizeof(source);
    assert(ngetsockname(fd, (void *)&source, &size) == 0 && source.sin_port == local.sin_port);
    assert(ngetpeername(fd, (void *)&source, &size) == -1 && errno == ENOTCONN);
    assert(nlisten(fd, 1) == -1 && errno == EOPNOTSUPP);
    assert(naccept(fd, NULL, NULL) == -1 && errno == EOPNOTSUPP);
    int value = 0;
    size = sizeof(value);
    assert(ngetsockopt(fd, SOL_SOCKET, SO_TYPE, &value, &size) == 0 && value == SOCK_DGRAM);
    value = 42;
    assert(nsetsockopt(fd, IPPROTO_IP, IP_TTL, &value, sizeof(value)) == 0);
    value = 0;
    assert(ngetsockopt(fd, IPPROTO_IP, IP_TTL, &value, &size) == 0 && value == 42);
    assert(nsend(fd, "x", 1, 0) == -1 && errno == EDESTADDRREQ);
    assert(nsendto(fd, "to", 2, 0, (void *)&remote, sizeof(remote)) == 2);
    ng_udp_out(NULL);
    clear_output();
    assert(nconnect(fd, (void *)&remote, sizeof(remote)) == 0);
    size = sizeof(source);
    assert(ngetpeername(fd, (void *)&source, &size) == 0 && source.sin_port == remote.sin_port);
    struct iovec iov[2] = {{"ab", 2}, {"cdef", 4}};
    struct msghdr message = {0};
    message.msg_iov = iov;
    message.msg_iovlen = 2;
    assert(nsendmsg(fd, &message, 0) == 6);
    assert(udp_find(fd)->send_datagram_count == 1); /* one call, one datagram */
    ng_udp_out(NULL);
    assert(output.count == 1);
    struct rte_mbuf *packet = output.items[0];
    assert(packet->size == 48 && !memcmp(packet->data + 42, "abcdef", 6));
    clear_output();
    assert(inject(local.sin_port, peer, 19102, "abcdef", 6, 0) == 0);
    assert(nready(fd) & EPOLLIN);
    value = -1;
    assert(nioctl(fd, FIONREAD, &value) == 0 && value == 6);
    char first[2] = {0}, second[4] = {0};
    iov[0] = (struct iovec){first, 2};
    iov[1] = (struct iovec){second, 4};
    message.msg_name = &source;
    message.msg_namelen = sizeof(source);
    assert(nrecvmsg(fd, &message, MSG_PEEK) == 6);
    assert(!memcmp(first, "ab", 2) && !memcmp(second, "cdef", 4));
    assert(source.sin_port == remote.sin_port && (nready(fd) & EPOLLIN));
    iov[1].iov_len = 1;
    assert(nrecvmsg(fd, &message, MSG_TRUNC) == 6 && (message.msg_flags & MSG_TRUNC));
    assert(!(nready(fd) & EPOLLIN));
    assert(inject(local.sin_port, peer, 19102, "next", 4, 0) == 0);
    char buffer[8];
    size = sizeof(source);
    assert(nrecvfrom(fd, buffer, sizeof(buffer), 0, (void *)&source, &size) == 4);
    assert(!memcmp(buffer, "next", 4) && source.sin_port == remote.sin_port);
    assert(inject(local.sin_port, peer, 19102, NULL, 0, 0) == 0);
    assert(nrecv(fd, NULL, 0, 0) == 0 && !(nready(fd) & EPOLLIN));
    assert(nsend(fd, "pending", 7, 0) == 7);
    assert(nclose(fd) == 0 && !netstack_fd_is_managed(fd));
    assert(udp_sockets && udp_sockets->fd == -1); /* output survives descriptor close */
    assert(nclose(fd) == -1 && errno == EBADF);
    fake_reuse_fd = fd;
    int stream_fd = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    assert(stream_fd == fd && !nisdgram(stream_fd));
    size = sizeof(value);
    assert(ngetsockopt(stream_fd, SOL_SOCKET, SO_TYPE, &value, &size) == 0 && value == SOCK_STREAM);
    assert(nbind(stream_fd, (void *)&local, sizeof(local)) == 0);
    ng_udp_out(NULL);
    clear_output();
    assert(!udp_sockets && netstack_fd_is_managed(stream_fd));
    assert(nclose(stream_fd) == 0);
    ng_tcp_out(NULL);
    clear_output();
    assert(!tcp_table.streams);
    puts("socket_api: UDP messages, addresses, readiness, options and cross-protocol fd reuse passed");
}

static void test_tcp_api(void)
{
    struct sockaddr_in remote = address(htonl(0xc0000201), 19202), source;
    int fd = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert(fd >= 0 && !nisdgram(fd));
    assert(nconnect(fd, (void *)&remote, sizeof(remote)) == -1 && errno == EINPROGRESS);
    struct ng_tcp_stream *stream = tcp_find(fd);
    ng_tcp_out(NULL);
    clear_output();
    input(stream, 100, stream->send_next_seq, RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    assert(nready(fd) & EPOLLOUT);
    ng_tcp_out(NULL);
    clear_output();
    input(stream, 101, stream->send_next_seq, RTE_TCP_ACK_FLAG, 65535, "abcdef", 6);
    char first[2], second[4];
    struct iovec iov[2] = {{first, 2}, {second, 4}};
    struct msghdr message = {0};
    message.msg_iov = iov;
    message.msg_iovlen = 2;
    message.msg_name = &source;
    message.msg_namelen = sizeof(source);
    assert(nrecvmsg(fd, &message, MSG_PEEK) == 6);
    assert(!memcmp(first, "ab", 2) && !memcmp(second, "cdef", 4));
    assert(source.sin_port == remote.sin_port && (nready(fd) & EPOLLIN));
    assert(nrecv(fd, first, 2, 0) == 2);
    socklen_t size = sizeof(source);
    assert(nrecvfrom(fd, second, 4, 0, (void *)&source, &size) == 4);
    assert(!(nready(fd) & EPOLLIN));
    assert(nsendto(fd, "x", 1, 0, (void *)&remote, sizeof(remote)) == -1 && errno == EISCONN);
    message.msg_name = NULL;
    message.msg_namelen = 0;
    assert(nsendmsg(fd, &message, 0) == 6);
    assert(stream->send_queue_bytes == 6);
    assert(nsendmsg(fd, NULL, 0) == -1 && errno == EFAULT);
    struct iovec bad = {NULL, 1};
    message.msg_iov = &bad;
    message.msg_iovlen = 1;
    assert(nsendmsg(fd, &message, 0) == -1 && errno == EINVAL);
    int available = -1;
    assert(nioctl(fd, FIONREAD, &available) == 0 && available == 0);
    assert(nfcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    assert(nshutdown(fd, SHUT_WR) == 0);
    assert(nsend(fd, "x", 1, 0) == -1 && errno == EPIPE);
    assert(nclose(fd) == 0 && !netstack_fd_is_managed(fd));
    ng_tcp_out(NULL);
    clear_output();
    input(stream, 107, stream->send_next_seq, RTE_TCP_ACK_FLAG | RTE_TCP_FIN_FLAG, 65535, NULL, 0);
    ng_tcp_out(NULL);
    clear_output();
    fake_ms += TCP_TIME_WAIT_MS + 1;
    ng_tcp_out(NULL);
    clear_output();
    assert(!tcp_table.streams);
    puts("socket_api: TCP handshake, stream messages, peek, shutdown and FIN lifetime passed");
}

static void test_accept_registration(void)
{
    struct sockaddr_in local = address(g_stack_ipv4_addr, 19301);
    int listener = nsocket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    assert(nbind(listener, (void *)&local, sizeof(local)) == 0 && nlisten(listener, 4) == 0);
    assert(naccept(listener, NULL, NULL) == -1 && errno == EAGAIN);
    struct ng_tcp_stream template = {0};
    template.local_ip = g_stack_ipv4_addr;
    template.peer_ip = htonl(0xc0000201);
    template.local_port = local.sin_port;
    template.peer_port = htons(19302);
    input(&template, 700, 0, RTE_TCP_SYN_FLAG, 65535, NULL, 0);
    struct ng_tcp_stream *stream = tcp_table.streams;
    ng_tcp_out(NULL);
    clear_output();
    input(stream, 701, stream->send_next_seq, RTE_TCP_ACK_FLAG, 65535, NULL, 0);
    assert(nready(listener) & EPOLLIN);
    int accepted = naccept(listener, NULL, NULL);
    assert(accepted >= 0 && accepted != listener && !nisdgram(accepted));
    int type = 0;
    socklen_t size = sizeof(type);
    assert(ngetsockopt(accepted, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_STREAM);
    assert(nsend(accepted, "accepted", 8, 0) == 8);
    assert(nclose(listener) == 0 && nclose(accepted) == 0);
    ng_tcp_out(NULL);
    clear_output();
    input(stream, 701, stream->send_next_seq, RTE_TCP_ACK_FLAG | RTE_TCP_FIN_FLAG, 65535, NULL, 0);
    ng_tcp_out(NULL);
    clear_output();
    fake_ms += TCP_TIME_WAIT_MS + 1;
    ng_tcp_out(NULL);
    clear_output();
    assert(!tcp_table.streams);
    puts("socket_api: accepted sockets inherit TCP dispatch and readiness passed");
}

static void test_fork_reset(void)
{
    int tcp_fd = nsocket(AF_INET, SOCK_STREAM, 0);
    int udp_fd = nsocket(AF_INET, SOCK_DGRAM, 0);
    struct ng_tcp_stream *saved_tcp = tcp_table.streams;
    struct udp_socket *saved_udp = udp_sockets;
    nfork_prepare();
    nfork_child();
    assert(!netstack_fd_is_managed(tcp_fd) && !netstack_fd_is_managed(udp_fd));
    assert(!tcp_table.streams && !udp_sockets);
    assert(nsocket(AF_INET, SOCK_STREAM, 0) == -1 && errno == EOPNOTSUPP);
    assert(nsocket(AF_INET, SOCK_DGRAM, 0) == -1 && errno == EOPNOTSUPP);
    assert(pthread_mutex_trylock(&socket_state_lock) == 0);
    pthread_mutex_unlock(&socket_state_lock);
    /* This fixture simulates the child in-place; release its detached objects. */
    tcp_table.streams = saved_tcp;
    udp_sockets = saved_udp;
    tcp_destroy(saved_tcp);
    udp_destroy(saved_udp);
    puts("socket_api: fork resets both protocol registries and releases the shared lock passed");
}

int main(void)
{
    g_stack_ipv4_addr = htonl(0xc0000202);
    test_registry_and_invalid_fd();
    test_udp_api();
    test_tcp_api();
    test_accept_registration();
    test_fork_reset();
    return 0;
}
