#ifndef PACKET_TEST_SUPPORT_H
#define PACKET_TEST_SUPPORT_H
/* Deterministic packet-level tests. DPDK ownership is modeled by an output ring.
 * No NIC, hugepages or EAL are needed. The production tcp_core.h is compiled. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#ifdef _WIN32
#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define SOCK_DGRAM 2
#define AF_UNSPEC 0
#define MSG_TRUNC 0x20
#define MSG_OOB 1
#define SOL_IP 0
#define IPPROTO_IP 0
#define IP_TOS 1
#define IP_TTL 2
#define IP_MTU_DISCOVER 10
#define IP_MTU 14
#define IP_PMTUDISC_DONT 0
#define IP_PMTUDISC_DO 2
struct iovec {
    void *iov_base;
    size_t iov_len;
};
struct msghdr {
    void *msg_name;
    unsigned msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};
#define SOL_SOCKET 1
#define SO_ERROR 4
#define SO_TYPE 3
#define SO_ACCEPTCONN 30
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define MSG_PEEK 2
#define MSG_WAITALL 256
#define TCP_NODELAY 1
#define FIONBIO 0x5421
#define FIONREAD 0x541b
struct in_addr {
    uint32_t s_addr;
};
struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};
struct sockaddr_in {
    unsigned short sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char zero[8];
};
#define htons(x) __builtin_bswap16((uint16_t)(x))
#define ntohs(x) htons(x)
#define htonl(x) __builtin_bswap32((uint32_t)(x))
#define ntohl(x) htonl(x)
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000
#define O_NONBLOCK 04000
#define F_GETFL 3
#define F_SETFL 4
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define MSG_MORE 0x8000
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT 6
#define TCP_INFO 11
#define TCP_MAXSEG 2
#define TCP_ESTABLISHED 1
#define TCP_CLOSE 7
#define TCP_LISTEN 10
#define TCP_SYN_RECV 3
#define TCP_SYN_SENT 2
#define TCP_FIN_WAIT1 4
#define TCP_FIN_WAIT2 5
#define TCP_CLOSING 11
#define TCP_TIME_WAIT 6
#define TCP_CLOSE_WAIT 8
#define TCP_LAST_ACK 9
struct tcp_info {
    uint8_t tcpi_state;
    uint32_t tcpi_snd_cwnd, tcpi_snd_ssthresh, tcpi_snd_mss, tcpi_rcv_mss, tcpi_rto, tcpi_rtt,
        tcpi_rttvar, tcpi_total_retrans, tcpi_unacked;
};
typedef unsigned short sa_family_t;
typedef unsigned socklen_t;
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif
#define EFD_CLOEXEC 0
#define EFD_NONBLOCK 0
#define SYS_eventfd2 10001
#define SYS_close 10002
#define SYS_fcntl 10003
#define RTE_ETHER_ADDR_LEN 6
#define RTE_ETHER_TYPE_IPV4 0x800
#define RTE_ARP_OP_REQUEST 1
#define RTE_TCP_FIN_FLAG 1
#define RTE_TCP_SYN_FLAG 2
#define RTE_TCP_RST_FLAG 4
#define RTE_TCP_PSH_FLAG 8
#define RTE_TCP_ACK_FLAG 16
#define EPOLLIN 1
#define EPOLLOUT 4
#define EPOLLERR 8
#define EPOLLHUP 16
#define EPOLLRDHUP 8192
#define LL_ADD(item, list)                                                                         \
    do {                                                                                           \
        (item)->prev = NULL;                                                                       \
        (item)->next = (list);                                                                     \
        if (list)                                                                                  \
            (list)->prev = (item);                                                                 \
        (list) = (item);                                                                           \
    } while (0)
#define LL_REMOVE(item, list)                                                                      \
    do {                                                                                           \
        if ((item)->prev)                                                                          \
            (item)->prev->next = (item)->next;                                                     \
        else                                                                                       \
            (list) = (item)->next;                                                                 \
        if ((item)->next)                                                                          \
            (item)->next->prev = (item)->prev;                                                     \
    } while (0)
struct rte_ring {
    void *items[256];
    unsigned count;
};
struct inout_ring {
    struct rte_ring *in, *out;
};
struct rte_mempool {
    int unused;
};
struct rte_mbuf {
    struct rte_mempool *pool;
    unsigned size;
    unsigned char data[2048];
};
struct rte_ether_addr {
    uint8_t addr_bytes[6];
};
#pragma pack(push, 1)
struct rte_ether_hdr {
    struct rte_ether_addr d_addr, s_addr;
    uint16_t ether_type;
};
struct rte_ipv4_hdr {
    uint8_t version_ihl, type_of_service;
    uint16_t total_length, packet_id, fragment_offset;
    uint8_t time_to_live, next_proto_id;
    uint16_t hdr_checksum;
    uint32_t src_addr, dst_addr;
};
struct rte_tcp_hdr {
    uint16_t src_port, dst_port;
    uint32_t sent_seq, recv_ack;
    uint8_t data_off, tcp_flags;
    uint16_t rx_win, cksum, tcp_urp;
};
struct rte_udp_hdr {
    uint16_t src_port, dst_port, dgram_len, dgram_cksum;
};
#pragma pack(pop)
static struct rte_ring output;
static struct inout_ring rings = {NULL, &output};
static uint8_t g_port_mac_addr[6] = {1, 2, 3, 4, 5, 6}, g_arp_broadcast_mac[6];
static uint32_t g_stack_ipv4_addr;
static uint64_t fake_ms = 10000;
static int fake_clock(clockid_t id, struct timespec *timestamp)
{
    (void)id;
    timestamp->tv_sec = fake_ms / 1000;
    timestamp->tv_nsec = fake_ms % 1000 * 1000000;
    return 0;
}

#define clock_gettime fake_clock
static int fake_next_fd = 10;
static int fake_reuse_fd = -1;
static long fake_syscall(long op, ...)
{
    (void)op;
    if (op != SYS_eventfd2)
        return 0;
    if (fake_reuse_fd >= 0) {
        int fd = fake_reuse_fd;
        fake_reuse_fd = -1;
        return fd;
    }
    return fake_next_fd++;
}

#define syscall fake_syscall
static void *rte_malloc(const char *name, size_t n, unsigned align)
{
    (void)name;
    (void)align;
    return malloc(n);
}

static void *rte_zmalloc(const char *name, size_t n, unsigned align)
{
    (void)name;
    (void)align;
    return calloc(1, n);
}

#define rte_free free
static uint64_t rte_get_timer_cycles(void)
{
    return fake_ms;
}

static struct inout_ring *ring_instance(void)
{
    return &rings;
}

static int mock_mac_available = 1;
static uint8_t *ng_get_dst_macaddr(uint32_t ip)
{
    (void)ip;
    return mock_mac_available ? g_port_mac_addr : NULL;
}

static struct rte_mbuf *rte_pktmbuf_alloc(struct rte_mempool *packet_pool)
{
    (void)packet_pool;
    return calloc(1, sizeof(struct rte_mbuf));
}

static void rte_pktmbuf_free(struct rte_mbuf *packet)
{
    free(packet);
}

static void *rte_pktmbuf_append(struct rte_mbuf *packet, unsigned n)
{
    if (n > sizeof(packet->data))
        return NULL;
    packet->size = n;
    return packet->data;
}

static unsigned rte_pktmbuf_data_len(struct rte_mbuf *packet)
{
    return packet->size;
}

#define rte_pktmbuf_mtod_offset(m, t, o) ((t)((m)->data + (o)))
static int rte_ring_mp_enqueue(struct rte_ring *r, void *p)
{
    if (r->count == 256)
        return -1;
    r->items[r->count++] = p;
    return 0;
}

static struct rte_mbuf *ng_send_arp(struct rte_mempool *packet_pool, int op, uint8_t *mac,
                                    uint32_t a, uint32_t b)
{
    (void)op;
    (void)mac;
    (void)a;
    (void)b;
    return rte_pktmbuf_alloc(packet_pool);
}

static uint16_t checksum(const void *buf, unsigned n, uint32_t sum)
{
    const unsigned char *b = buf;
    for (unsigned i = 0; i < n; i += 2)
        sum += ((uint32_t)b[i] << 8) + (i + 1 < n ? b[i + 1] : 0);
    while (sum >> 16)
        sum = (sum & 65535) + (sum >> 16);
    return htons((uint16_t)~sum);
}

static uint16_t rte_ipv4_cksum(const struct rte_ipv4_hdr *ipv4_header)
{
    uint16_t c = checksum(ipv4_header, 20, 0);
    return c ? c : 0xffff;
}

static uint16_t rte_ipv4_udptcp_cksum(const struct rte_ipv4_hdr *ipv4_header, const void *tcp)
{
    uint32_t sum = 0;
    const unsigned char *a = (const void *)&ipv4_header->src_addr;
    for (int i = 0; i < 8; i += 2)
        sum += ((uint32_t)a[i] << 8) | a[i + 1];
    unsigned len = ntohs(ipv4_header->total_length) - 20;
    sum += ipv4_header->next_proto_id + len;
    uint16_t c = checksum(tcp, len, sum);
    return c ? c : 0xffff;
}

#endif
