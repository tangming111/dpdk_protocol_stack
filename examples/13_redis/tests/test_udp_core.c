#include "packet_test_support.h"
#include "../tcp_core.h"
#include "../udp_core.h"
static struct rte_mbuf *pop_udp(void)
{
    assert(output.count);
    struct rte_mbuf *packet = output.items[0];
    memmove(output.items, output.items + 1, --output.count * sizeof(void *));
    return packet;
}

static void clear_udp_output(void)
{
    while (output.count)
        free(pop_udp());
}

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

int main(void)
{
    g_stack_ipv4_addr = htonl(0xc0000202);
    uint32_t peer = htonl(0xc0000201);
    int fd = udp_socket_create(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    assert(fd >= 0 && nisdgram(fd));
    struct sockaddr_in local = address(0, 19001), remote = address(peer, 19002), src;
    socklen_t addrlen = sizeof(src);
    assert(udp_bind(fd, (void *)&local, sizeof(local)) == 0);
    int tcpfd = tcp_socket(AF_INET, SOCK_STREAM, 0);
    assert(tcpfd >= 0 && !nisdgram(tcpfd));
    assert(tcp_bind(tcpfd, (void *)&local, sizeof(local)) == 0);
    assert(tcp_close(tcpfd) == 0);
    ng_tcp_out(NULL);
    assert(!tcp_table.streams);
    assert(udp_recvfrom(fd, NULL, 0, 0, NULL, NULL, NULL) == -1 && errno == EAGAIN);
    int type = 0;
    socklen_t size = sizeof(type);
    assert(udp_getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0 && type == SOCK_DGRAM);
    assert(udp_name(fd, (void *)&src, &addrlen, 1) == -1 && errno == ENOTCONN);
    unsigned char buf[2000] = {0};
    assert(udp_sendto(fd, buf, 3, 0, NULL, 0) == -1 && errno == EDESTADDRREQ);
    assert(udp_sendto(fd, buf, UDP_MAX_PAYLOAD + 1, 0, (void *)&remote, sizeof(remote)) == -1 &&
           errno == EMSGSIZE);
    assert(inject(local.sin_port, peer, 19002, "abcdef", 6, 1) == -1);
    assert(inject(local.sin_port, peer, 19002, "abcdef", 6, 3) == -1);
    assert(!(udp_ready(fd) & EPOLLIN));
    assert(inject(local.sin_port, peer, 19002, "abcdef", 6, 0) == 0);
    int available = -1;
    assert(udp_ioctl(fd, FIONREAD, &available) == 0 && available == 6);
    assert(udp_recvfrom(fd, buf, 2, MSG_PEEK, (void *)&src, &addrlen, NULL) == 2 &&
           !memcmp(buf, "ab", 2));
    assert(src.sin_addr.s_addr == peer && src.sin_port == remote.sin_port &&
           src.sin_family == AF_INET);
    assert(udp_recvfrom(fd, buf, 3, 0, NULL, NULL, NULL) == 3 && !memcmp(buf, "abc", 3));
    assert(!(udp_ready(fd) & EPOLLIN)); /* Truncated tail is discarded. */
    inject(local.sin_port, peer, 19002, "next", 4, 2);
    assert(udp_recvfrom(fd, buf, 2, MSG_TRUNC, NULL, NULL, NULL) == 4 && !memcmp(buf, "ne", 2));
    inject(local.sin_port, peer, 19002, NULL, 0, 0);
    assert(udp_ready(fd) & EPOLLIN);
    assert(udp_recvfrom(fd, NULL, 0, 0, NULL, NULL, NULL) == 0 && !(udp_ready(fd) & EPOLLIN));
    assert(udp_connect(fd, (void *)&remote, sizeof(remote)) == 0);
    inject(local.sin_port, peer, 19003, "wrong", 5, 0);
    assert(!(udp_ready(fd) & EPOLLIN));
    inject(local.sin_port, peer, 19002, "correct", 7, 0);
    assert(udp_recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL, NULL) == 7);
    assert(udp_sendto(fd, "binary\0data", 11, 0, NULL, 0) == 11);
    ng_udp_out(NULL);
    struct rte_mbuf *packet = pop_udp();
    assert(packet->size == 53);
    struct rte_ipv4_hdr *ipv4_header = (void *)(packet->data + 14);
    struct rte_udp_hdr *udp_header = (void *)(ipv4_header + 1);
    assert(ipv4_header->next_proto_id == IPPROTO_UDP &&
           ipv4_header->src_addr == g_stack_ipv4_addr && ipv4_header->dst_addr == peer &&
           tcp_ipv4_valid(ipv4_header));
    assert(udp_header->src_port == local.sin_port && udp_header->dst_port == remote.sin_port &&
           ntohs(udp_header->dgram_len) == 19 && !memcmp(udp_header + 1, "binary\0data", 11));
    uint16_t saved = udp_header->dgram_cksum;
    udp_header->dgram_cksum = 0;
    assert(saved == rte_ipv4_udptcp_cksum(ipv4_header, udp_header));
    free(packet);
    assert(udp_sendto(fd, NULL, 0, 0, NULL, 0) == 0);
    ng_udp_out(NULL);
    packet = pop_udp();
    assert(packet->size == 42);
    free(packet);
    struct iovec iovecs[2] = {{buf, 2}, {buf + 8, 2}};
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iovecs;
    msg.msg_iovlen = 2;
    msg.msg_name = &src;
    msg.msg_namelen = sizeof(src);
    inject(local.sin_port, peer, 19002, "abcdefgh", 8, 0);
    assert(udp_recvmsg(fd, &msg, MSG_TRUNC) == 8 && (msg.msg_flags & MSG_TRUNC));
    assert(!memcmp(buf, "ab", 2) && !memcmp(buf + 8, "cd", 2));
    msg.msg_name = &remote;
    msg.msg_namelen = sizeof(remote);
    assert(udp_sendmsg(fd, &msg, 0) == 4);
    ng_udp_out(NULL);
    packet = pop_udp();
    assert(!memcmp(packet->data + 42, "abcd", 4));
    free(packet);
    /* A connected socket wins over a new wildcard listener on the same port. */
    int one = 1;
    assert(udp_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    int listener = udp_socket_create(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    assert(udp_setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
           udp_bind(listener, (void *)&local, sizeof(local)) == 0);
    inject(local.sin_port, peer, 19002, "one", 3, 0);
    assert(udp_ready(fd) & EPOLLIN);
    assert(!(udp_ready(listener) & EPOLLIN));
    inject(local.sin_port, peer, 19003, "two", 3, 0);
    assert(udp_ready(listener) & EPOLLIN);
    udp_recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL, NULL);
    udp_recvfrom(listener, buf, sizeof(buf), 0, NULL, NULL, NULL);
    for (unsigned i = 0; i < UDP_QUEUE_PACKETS + 10; ++i)
        inject(local.sin_port, peer, 19002, NULL, 0, 0);
    assert(udp_find(fd)->recv_datagram_count == UDP_QUEUE_PACKETS);
    for (unsigned i = 0; i < UDP_QUEUE_PACKETS; ++i)
        assert(udp_recvfrom(fd, NULL, 0, 0, NULL, NULL, NULL) == 0);
    for (unsigned i = 0; i < UDP_QUEUE_PACKETS; ++i)
        assert(udp_sendto(fd, NULL, 0, 0, NULL, 0) == 0);
    assert(udp_sendto(fd, NULL, 0, 0, NULL, 0) == -1 && errno == EAGAIN &&
           !(udp_ready(fd) & EPOLLOUT));
    while (udp_find(fd)->send_head) {
        ng_udp_out(NULL);
        clear_udp_output();
    }
    assert(udp_ready(fd) & EPOLLOUT);
    /* Ring backpressure must retain the datagram rather than report ARP loss. */
    assert(udp_sendto(fd, "held", 4, 0, NULL, 0) == 4);
    output.count = 256;
    fake_ms += UDP_ARP_TIMEOUT_MS + 1;
    ng_udp_out(NULL);
    assert(udp_find(fd)->send_datagram_count == 1 && !udp_find(fd)->error);
    output.count = 0;
    ng_udp_out(NULL);
    clear_udp_output();
    assert(!udp_find(fd)->send_datagram_count);
    unsigned char short_addr[4] = {0, 0, 0xa5, 0x5a};
    socklen_t short_len = 2;
    assert(udp_name(fd, (void *)short_addr, &short_len, 0) == 0 &&
           short_len == sizeof(struct sockaddr_in) && short_addr[2] == 0xa5 &&
           short_addr[3] == 0x5a);
    struct sockaddr disconnected;
    memset(&disconnected, 0, sizeof(disconnected));
    disconnected.sa_family = AF_UNSPEC;
    assert(udp_connect(fd, &disconnected, sizeof(disconnected)) == 0 &&
           udp_sendto(fd, buf, 1, 0, NULL, 0) == -1 && errno == EDESTADDRREQ);
    /* Close retains queued sends until output accepts them. */
    assert(udp_sendto(fd, "last", 4, 0, (void *)&remote, sizeof(remote)) == 4);
    assert(udp_close(fd) == 0 && !tcp_fd_managed(fd));
    assert(udp_close(listener) == 0);
    int rebound = udp_socket_create(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    assert(udp_bind(rebound, (void *)&local, sizeof(local)) == 0);
    udp_close(rebound);
    ng_udp_out(NULL);
    clear_udp_output();
    assert(!udp_sockets);
    fd = udp_socket_create(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    mock_mac_available = 0;
    assert(udp_sendto(fd, buf, 1, 0, (void *)&remote, sizeof(remote)) == 1);
    ng_udp_out(NULL);
    clear_udp_output();
    fake_ms += UDP_ARP_TIMEOUT_MS + 1;
    ng_udp_out(NULL);
    assert(udp_find(fd)->error == EHOSTUNREACH);
    mock_mac_available = 1;
    udp_close(fd);
    assert(!udp_sockets);
    puts("udp_core: datagram boundaries, checksum, zero length, truncation/peek, peer filtering, "
         "message I/O, queue limits, close/ARP timeout passed");
    return 0;
}
