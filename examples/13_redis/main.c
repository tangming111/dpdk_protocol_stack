#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <netinet/tcp.h>
#include <time.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#ifndef ENABLE_KNI_APP
#define ENABLE_KNI_APP 0
#endif
#if ENABLE_KNI_APP
#include <rte_kni.h>
#endif
#include <math.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include "arp.h"
#include "net.h"
#include "epoll_core.h"

#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>

#define ENABLE_SEND 1
#define ENABLE_ARP 1
#define ENABLE_ICMP 1
#define ENABLE_ARP_REPLY 1
#define ENABLE_DPDK_DUG 1
#define ENABLE_TIMER 0
#define ENABLE_RINGBUFFER 1
#define ENABLE_MULTHREAD 1
#define ENABLE_UDP_APP 1
#define ENABLE_TCP_APP 1
#define ENABLE_DDOS_DETECT 0
#define ENABLE_SINGLE_EPOLL 1

#define ARP_ENTRY_STATUS_DYNAMIC 0
#define ARP_ENTRY_STATUS_STATIC 1

#define NUM_MBUFS (4096 - 1)
#define BURST_SIZE 128
#define MAX_FD_COUNT 1024
#define MAX_PACKET_SIZE 2048

#define MAKE_IPV4_ADDR(a, b, c, d)                                                                 \
    ((uint32_t)(((a)&0xff)) | (((b)&0xff) << 8) | (((c)&0xff) << 16) | (((d)&0xff) << 24))
static int g_stack_stop;
static uint32_t g_stack_ipv4_addr = MAKE_IPV4_ADDR(192, 168, 196, 132);

#define TIMER_RESOLUTION_CYCLES 2000000000ULL /* around 10ms  at 2 Ghz   10ms * 100 */
#define RING_SIZE 1024

#if ENABLE_SEND

// static uint32_t gSrcIp;
// static uint32_t gDstIp;

// static uint8_t gDstMac[RTE_ETHER_ADDR_LEN];
static uint8_t g_port_mac_addr[RTE_ETHER_ADDR_LEN];

// static uint16_t gSrcPort;
// static uint16_t gDstPort;

#endif

#ifdef ENABLE_ARP_REPLY

uint8_t g_arp_broadcast_mac[RTE_ETHER_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#endif
#if ENABLE_KNI_APP

struct rte_kni *global_kni = NULL;

#endif
struct localhost {
    int fd;

    // unsigned int status;//阻塞与非阻塞
    uint32_t local_ip;
    uint8_t local_mac[RTE_ETHER_ADDR_LEN];
    uint16_t local_port;

    int protocol; // 0: UDP, 1: TCP

    struct rte_ring *sndbuf;
    struct rte_ring *rcvbuf;

    struct localhost *next;
    struct localhost *prev;

    pthread_cond_t cond;
    pthread_mutex_t mutex;
};

struct localhost *localhost_list = NULL;

#ifdef ENABLE_RINGBUFFER
struct inout_ring {
    struct rte_ring *in;
    struct rte_ring *out;
};

static struct inout_ring *rInst = NULL;

static struct inout_ring *ring_instance(void)
{
    if (rInst == NULL) {
        rInst = (struct inout_ring *)rte_malloc("RING_INSTANCE", sizeof(struct inout_ring), 0);
        if (rInst == NULL) {
            rte_exit(EXIT_FAILURE, "Failed to allocate memory for ring instance\n");
        }
        memset(rInst, 0, sizeof(struct inout_ring));
    }
    return rInst;
}

#endif

static int ng_arp_entry_insert(uint32_t ip, uint8_t *mac)
{

    struct arp_table *table = arp_table_instance();

    uint8_t *hwaddr = ng_get_dst_macaddr(ip);
    if (hwaddr == NULL) {

        struct arp_table_entry *entry = rte_malloc("arp_entry", sizeof(struct arp_table_entry), 0);
        if (entry) {
            memset(entry, 0, sizeof(struct arp_table_entry));

            entry->ip_addr = ip;
            rte_memcpy(entry->mac_addr, mac, RTE_ETHER_ADDR_LEN);
            entry->type = 0;

            pthread_spin_lock(&table->spinlock);
            LL_ADD(entry, table->entries);
            table->count++;
            pthread_spin_unlock(&table->spinlock);
        }

        return 1; //
    }

    return 0;
}

int g_dpdk_port_id = 0;
static const struct rte_eth_conf port_conf_default = {
    .rxmode =
        {
            .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
        },
};

struct localhost *get_hostinfo_fromip_port(uint32_t ip, uint16_t port, uint8_t protocol);
static int ng_encode_udp_apppkt(uint8_t *msg, uint32_t sip, uint32_t dip, uint16_t sport,
                                uint16_t dport, uint8_t *srcmac, uint8_t *dstmac, uint8_t *data,
                                uint16_t total_len);
static int udp_out(struct rte_mempool *mbuf_pool);
static int get_fd_frombitmap(void);
void *get_hostinfo_fromfd(int socket_fd);
int nclose(int fd);
int udp_server_entry(void *arg);
int nsocket(int domain, int type, int protocol);
int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr,
                  socklen_t *addrlen);
ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen);
int nlisten(int sockfd, __attribute__((unused)) int backlog);

#if ENABLE_TCP_APP
int naccept(int sockfd, struct sockaddr *addr, __attribute__((unused)) socklen_t *addrlen);
static int ng_tcp_process(struct rte_mbuf *tcpmbuf);
static int ng_tcp_out(struct rte_mempool *mbuf_pool);
ssize_t nsend(int sockfd, const void *buf, size_t len, __attribute__((unused)) int flags);
ssize_t nrecv(int sockfd, void *buf, size_t len, __attribute__((unused)) int flags);

#endif

static void ng_init_port(struct rte_mempool *mempool)
{
    uint16_t nb_sys_ports = rte_eth_dev_count_avail();
    if (nb_sys_ports == 0) {
        rte_exit(EXIT_FAILURE, "No supported Ethernet device found\n");
    }
    struct rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(g_dpdk_port_id, &dev_info); //获取设备信息eth原生信息

    const int rx_rings = 1, tx_rings = 1;
    struct rte_eth_conf port_conf = port_conf_default;
    rte_eth_dev_configure(g_dpdk_port_id, rx_rings, tx_rings, &port_conf); //配置队列数量

    if (rte_eth_rx_queue_setup(g_dpdk_port_id, 0, 1024, rte_eth_dev_socket_id(g_dpdk_port_id), NULL,
                               mempool) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot setup rx queue\n");
    }

#if ENABLE_SEND
    struct rte_eth_txconf txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.rxmode.offloads;
    if (rte_eth_tx_queue_setup(g_dpdk_port_id, 0, 1024, rte_eth_dev_socket_id(g_dpdk_port_id),
                               &txconf) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot setup tx queue\n");
    }
#endif

    if (rte_eth_dev_start(g_dpdk_port_id) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot start device\n");
    }

    // rte_eth_promiscuous_enable(gdpdkportid);//开启混杂模式，接收所有数据包
}

static struct rte_mbuf *ng_udp_pkt(struct rte_mempool *mempool, uint32_t sip, uint32_t dip,
                                   uint16_t sport, uint16_t dport, uint8_t *srcmac, uint8_t *dstmac,
                                   uint8_t *data, uint16_t length)
{

    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
                               sizeof(struct rte_udp_hdr) + length; // 14 + 20 + 8
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
    if (mbuf == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf\n");
    }
    mbuf->data_len = total_len;
    mbuf->pkt_len = total_len;

    uint8_t *pktdata = rte_pktmbuf_mtod(mbuf, uint8_t *);
    ng_encode_udp_apppkt(pktdata, sip, dip, sport, dport, srcmac, dstmac, data, total_len);

    return mbuf;
}

static int ng_encode_arp_pkt(uint8_t *msg, uint8_t *dst_mac, uint32_t src_ip, uint32_t dst_ip,
                             uint16_t opcode)
{
    // ethhdr
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)msg;
    /*
        if (strncmp((char *)dst_mac, (char *)gDefaultArpMac, RTE_ETHER_ADDR_LEN) != 0) {
            rte_memcpy(eth_hdr->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
        } else {
            uint8_t mac[RTE_ETHER_ADDR_LEN] = {0x0};
            rte_memcpy(eth_hdr->d_addr.addr_bytes, mac, RTE_ETHER_ADDR_LEN);
        }
    */

    rte_memcpy(eth_hdr->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth_hdr->s_addr.addr_bytes, g_port_mac_addr, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = htons(RTE_ETHER_TYPE_ARP);

    // arphdr
    struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
    arp_hdr->arp_hardware = htons(RTE_ARP_HRD_ETHER);
    arp_hdr->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);
    arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN; //硬件地址长度 即MAC地址长度
    arp_hdr->arp_plen = sizeof(uint32_t);   //协议地址长度 即IP地址长度
    arp_hdr->arp_opcode = htons(opcode);
    rte_memcpy(arp_hdr->arp_data.arp_sha.addr_bytes, g_port_mac_addr, RTE_ETHER_ADDR_LEN);
    // rte_memcpy(arp_hdr->arp_data.arp_tha.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    if (strncmp((char *)dst_mac, (char *)g_arp_broadcast_mac, RTE_ETHER_ADDR_LEN) != 0) {
        rte_memcpy(arp_hdr->arp_data.arp_tha.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    } else {
        uint8_t mac[RTE_ETHER_ADDR_LEN] = {0x0};
        rte_memcpy(arp_hdr->arp_data.arp_tha.addr_bytes, mac, RTE_ETHER_ADDR_LEN);
    }

    arp_hdr->arp_data.arp_tip = dst_ip;
    arp_hdr->arp_data.arp_sip = src_ip;

    return 0;
}

static struct rte_mbuf *ng_send_arp(struct rte_mempool *mempool, uint16_t opcode, uint8_t *dst_mac,
                                    uint32_t src_ip, uint32_t dst_ip)
{
    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
    if (mbuf == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf\n");
    }
    mbuf->data_len = total_len;
    mbuf->pkt_len = total_len;
    uint8_t *pktdata = rte_pktmbuf_mtod(mbuf, uint8_t *);
    ng_encode_arp_pkt(pktdata, dst_mac, src_ip, dst_ip, opcode);

    return mbuf;
}

#if 0
static int ng_rcmp_checksum(uint16_t *buf, int len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += *buf++;
        len -= 2;
    }
    if (len == 1) {
        sum += *(uint8_t *)buf;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return ~sum;
}

static int ng_encode_icmp_pkt(uint8_t *msg, uint8_t *dst_mac,
    uint32_t src_ip, uint32_t dst_ip, uint16_t id, uint16_t seq_nb) {
    //ethhdr
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth_hdr->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth_hdr->s_addr.addr_bytes, g_port_mac_addr, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    //iphdr
    struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    ipv4_hdr->version_ihl = 0x45;
    ipv4_hdr->type_of_service = 0;
    ipv4_hdr->total_length = htons(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr));
    ipv4_hdr->packet_id = 0;
    ipv4_hdr->fragment_offset = 0;
    ipv4_hdr->time_to_live = 64;
    ipv4_hdr->next_proto_id = IPPROTO_ICMP;
    ipv4_hdr->src_addr = src_ip;
    ipv4_hdr->dst_addr = dst_ip;

    ipv4_hdr->hdr_checksum = 0;
    ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

    //icmphdr
    struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(ipv4_hdr + 1);
    icmp_hdr->icmp_type = RTE_IP_ICMP_ECHO_REPLY;
    icmp_hdr->icmp_code = 0;
    icmp_hdr->icmp_ident = id; // You may want to set this to a specific value
    icmp_hdr->icmp_seq_nb = seq_nb; // You may want to set this to a specific value

    icmp_hdr->icmp_cksum = 0;
    //icmp_hdr->icmp_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, icmp_hdr);错误
    icmp_hdr->icmp_cksum = ng_rcmp_checksum((uint16_t *)icmp_hdr, sizeof(struct rte_icmp_hdr));

    return 0;
}

static struct rte_mbuf *ng_send_icmp(struct rte_mempool *mempool, uint8_t *dst_mac,
    uint32_t src_ip, uint32_t dst_ip, uint16_t id, uint16_t seq_nb)
{
    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_icmp_hdr);
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
    if (mbuf == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf\n");
    }
    mbuf->data_len = total_len;
    mbuf->pkt_len = total_len;
    uint8_t *pktdata = rte_pktmbuf_mtod(mbuf, uint8_t *);
    ng_encode_icmp_pkt(pktdata, dst_mac, src_ip, dst_ip, id, seq_nb);

    return mbuf;
}

#endif
#ifdef ENABLE_TIMER
static void arp_request_timer_cb(__attribute__((unused)) struct rte_timer *tim, void *arg)
{
    struct rte_mempool *mbuf_pool = (struct rte_mempool *)arg;
    struct inout_ring *ring = ring_instance();
#if 0
    struct rte_mbuf *arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, arp_hdr->arp_data.arp_sha.addr_bytes, arp_hdr->arp_data.arp_sip, arp_hdr->arp_data.arp_tip);
    rte_eth_tx_burst(g_dpdk_port_id, 0, &arp_mbuf, 1);
    rte_pktmbuf_free(arp_mbuf);
#endif
    int i = 1;
    for (i = 1; i <= 254; i++) {
        // uint32_t target_ip = MAKE_IPV4_ADDR(192, 168, 196, i);
        uint32_t target_ip = (g_stack_ipv4_addr & 0x00FFFFFF) | ((i << 24) & 0xFF000000);
        uint8_t *dst_mac = ng_get_dst_macaddr(target_ip);
        // struct in_addr addr;
        // addr.s_addr = target_ip;
        // printf("arp ---> src: %s\n", inet_ntoa(addr));
        struct rte_mbuf *arp_mbuf = NULL;
        if (dst_mac == NULL) {
            // arphdr --> mac: FF:FF:FF:FF:FF:FF
            // ether  --> mac: 00:00:00:00:00:00
            arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, g_arp_broadcast_mac,
                                   g_stack_ipv4_addr, target_ip);
        } else {
            arp_mbuf =
                ng_send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, dst_mac, g_stack_ipv4_addr, target_ip);
        }
        // rte_eth_tx_burst(gdpdkportid, 0, &arp_mbuf, 1);
        // rte_pktmbuf_free(arp_mbuf);
        rte_ring_mp_enqueue_burst(ring->out, (void **)&arp_mbuf, 1, NULL);
    }
}

#endif

static inline void print_ether_addr(const char *what, struct rte_ether_addr *eth_addr)
{
    char buf[RTE_ETHER_ADDR_FMT_SIZE];
    rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, eth_addr);
    printf("%s%s", what, buf);
}

struct offload {
    uint32_t src_ip;
    uint32_t dst_ip;

    uint16_t src_port;
    uint16_t dst_port;

    uint8_t protocol;

    unsigned char *data;
    uint16_t data_len;
};

static int udp_process(struct rte_mbuf *udpmbufs)
{
    struct rte_ipv4_hdr *ipv4_hdr =
        rte_pktmbuf_mtod_offset(udpmbufs, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    struct rte_udp_hdr *udp_hdr =
        (struct rte_udp_hdr *)((unsigned char *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));

    struct localhost *host =
        get_hostinfo_fromip_port(ipv4_hdr->dst_addr, udp_hdr->dst_port, ipv4_hdr->next_proto_id);
    // printf("udp_process host: %p\n", (void*)host);
    if (host == NULL) {
        rte_pktmbuf_free(udpmbufs);
        return -1;
    }

    struct offload *offload_record = rte_malloc("OFFLOAD", sizeof(struct offload), 0);
    if (offload_record == NULL) {
        rte_pktmbuf_free(udpmbufs);
        rte_exit(EXIT_FAILURE, "Failed to allocate memory for offload\n");
        return -1;
    }

    offload_record->src_ip = ipv4_hdr->src_addr;
    offload_record->dst_ip = ipv4_hdr->dst_addr;
    offload_record->src_port = udp_hdr->src_port;
    offload_record->dst_port = udp_hdr->dst_port;
    offload_record->protocol = IPPROTO_UDP;
    offload_record->data_len = ntohs(udp_hdr->dgram_len);
    offload_record->data =
        rte_malloc("UDP_DATA", offload_record->data_len - sizeof(struct rte_udp_hdr), 0);
    if (offload_record->data == NULL) {
        rte_pktmbuf_free(udpmbufs);
        rte_free(offload_record);
        rte_exit(EXIT_FAILURE, "Failed to allocate memory for UDP data\n");
        return -1;
    }
    // en
    rte_memcpy(offload_record->data, (unsigned char *)(udp_hdr + 1),
               offload_record->data_len - sizeof(struct rte_udp_hdr));
    rte_ring_sp_enqueue(host->rcvbuf, (void *)offload_record);
    pthread_mutex_lock(&host->mutex);
    pthread_cond_signal(&host->cond);
    pthread_mutex_unlock(&host->mutex);
    // printf("udp_process ---> src: %s:%d \n", inet_ntoa(*(struct in_addr *)&ol->src_ip),
    // ntohs(ol->src_port));
    rte_pktmbuf_free(udpmbufs);
    return 0;
}

static int ng_encode_udp_apppkt(uint8_t *msg, uint32_t sip, uint32_t dip, uint16_t sport,
                                uint16_t dport, uint8_t *srcmac, uint8_t *dstmac, uint8_t *data,
                                uint16_t total_len)
{

    // ethhdr
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth_hdr->d_addr.addr_bytes, dstmac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth_hdr->s_addr.addr_bytes, srcmac, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    // iphdr
    // struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(msg + sizeof(struct rte_ether_hdr));
    ipv4_hdr->version_ihl = 0x45; //版本号是4 长度是5  一个字节没有大小端
    // ipv4_hdr->version = 0x4;
    // ipv4_hdr->ihl = 0x5;
    ipv4_hdr->type_of_service = 0;
    ipv4_hdr->total_length = htons(total_len - sizeof(struct rte_ether_hdr));
    ipv4_hdr->packet_id = 0;
    ipv4_hdr->fragment_offset = 0;
    ipv4_hdr->time_to_live = 64;
    ipv4_hdr->next_proto_id = IPPROTO_UDP;
    ipv4_hdr->src_addr = sip;
    ipv4_hdr->dst_addr = dip;

    ipv4_hdr->hdr_checksum = 0; //计算校验和前先置0，有脏值
    ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

    // udphdr
    // struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ipv4_hdr + 1);
    struct rte_udp_hdr *udp_hdr =
        (struct rte_udp_hdr *)(msg + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
    udp_hdr->src_port = sport;
    udp_hdr->dst_port = dport;
    uint16_t udp_len = total_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr);
    udp_hdr->dgram_len = htons(udp_len);

    // rte_memcpy((uint8_t *)(udp_hdr + 1), data, udp_len);
    rte_memcpy((uint8_t *)(udp_hdr + 1), data, udp_len - sizeof(struct rte_udp_hdr));

    udp_hdr->dgram_cksum = 0;
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);

    return 0;
}

int udp_out(struct rte_mempool *mbuf_pool)
{
    struct localhost *host;
    for (host = localhost_list; host != NULL; host = host->next) {

        struct offload *offload_record;
        int nb_snd = rte_ring_sc_dequeue(host->sndbuf, (void **)&offload_record);
        if (nb_snd < 0)
            continue;

        struct in_addr addr;
        addr.s_addr = offload_record->dst_ip;
        printf("udp_out ---> src: %s:%d \n", inet_ntoa(addr), ntohs(offload_record->dst_port));

        uint8_t *dstmac = ng_get_dst_macaddr(offload_record->dst_ip);
        if (dstmac == NULL) {
            // struct rte_mbuf *arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REPLY, gDstMac,
            // ol->src_ip, ol->dst_ip);
            struct rte_mbuf *arp_mbuf =
                ng_send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, g_arp_broadcast_mac,
                            offload_record->src_ip, offload_record->dst_ip);
            struct inout_ring *ring = ring_instance();
            rte_ring_mp_enqueue_burst(ring->out, (void **)&arp_mbuf, 1, NULL);

            rte_ring_sp_enqueue(host->sndbuf, (void *)offload_record);
        } else {
            struct rte_mbuf *udpbuf =
                ng_udp_pkt(mbuf_pool, offload_record->src_ip, offload_record->dst_ip,
                           offload_record->src_port, offload_record->dst_port, host->local_mac,
                           dstmac, offload_record->data, offload_record->data_len);

            struct inout_ring *ring = ring_instance();
            rte_ring_mp_enqueue_burst(ring->out, (void **)&udpbuf, 1, NULL);
        }
    }

    return 0;
}

#if ENABLE_TCP_APP // ngtcp

#include "tcp_core.h"
#include "udp_core.h"

#define BUFFER_SIZE 1024

#endif

static void forward_or_free(struct rte_mbuf *packet)
{
#if ENABLE_KNI_APP
    if (global_kni && rte_kni_tx_burst(global_kni, &packet, 1) == 1)
        return;
#endif
    rte_pktmbuf_free(packet);
}

static int pkt_process(void *arg)
{
    struct rte_mempool *packet_pool = arg;
    struct inout_ring *packet_rings = ring_instance();
    while (!__atomic_load_n(&g_stack_stop, __ATOMIC_ACQUIRE)) {
        struct rte_mbuf *packets[BURST_SIZE];
        unsigned received_count =
            rte_ring_mc_dequeue_burst(packet_rings->in, (void **)packets, BURST_SIZE, NULL);
        for (unsigned i = 0; i < received_count; ++i) {
            struct rte_mbuf *packet = packets[i];
            if (rte_pktmbuf_data_len(packet) < sizeof(struct rte_ether_hdr)) {
                rte_pktmbuf_free(packet);
                continue;
            }
            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(packet, struct rte_ether_hdr *);
            if (eth->ether_type == htons(RTE_ETHER_TYPE_ARP) &&
                rte_pktmbuf_data_len(packet) >= sizeof(*eth) + sizeof(struct rte_arp_hdr)) {
                struct rte_arp_hdr *arp_header = (void *)(eth + 1);
                if (arp_header->arp_hardware == htons(RTE_ARP_HRD_ETHER) &&
                    arp_header->arp_protocol == htons(RTE_ETHER_TYPE_IPV4) &&
                    arp_header->arp_hlen == 6 && arp_header->arp_plen == 4) {
                    ng_arp_entry_insert(arp_header->arp_data.arp_sip,
                                        arp_header->arp_data.arp_sha.addr_bytes);
                    if (arp_header->arp_opcode == htons(RTE_ARP_OP_REQUEST) &&
                        arp_header->arp_data.arp_tip == g_stack_ipv4_addr) {
                        struct rte_mbuf *reply = ng_send_arp(
                            packet_pool, RTE_ARP_OP_REPLY, arp_header->arp_data.arp_sha.addr_bytes,
                            g_stack_ipv4_addr, arp_header->arp_data.arp_sip);
                        if (reply && rte_ring_mp_enqueue(packet_rings->out, reply) < 0)
                            rte_pktmbuf_free(reply);
                    }
                }
                forward_or_free(packet);
            } else if (eth->ether_type == htons(RTE_ETHER_TYPE_IPV4) &&
                       rte_pktmbuf_data_len(packet) >= sizeof(*eth) + sizeof(struct rte_ipv4_hdr)) {
                struct rte_ipv4_hdr *ipv4_header = (void *)(eth + 1);
                if (ipv4_header->dst_addr == g_stack_ipv4_addr &&
                    ipv4_header->version_ihl == 0x45 && tcp_ipv4_valid(ipv4_header)) {
                    ng_arp_entry_insert(ipv4_header->src_addr, eth->s_addr.addr_bytes);
                    if (ipv4_header->next_proto_id == IPPROTO_TCP) {
                        ng_tcp_process(packet);
                        continue;
                    }
                    if (ipv4_header->next_proto_id == IPPROTO_UDP) {
                        ng_udp_process(packet);
                        continue;
                    }
                }
                forward_or_free(packet);
            } else
                forward_or_free(packet);
        }
#if ENABLE_KNI_APP
        if (global_kni)
            rte_kni_handle_request(global_kni);
#endif
        ng_tcp_out(packet_pool);
        ng_udp_out(packet_pool);
        /* Both cores have released socket_state_lock before readiness is inspected. */
        nepoll_notify();
    }
    return 0;
}

#if ENABLE_UDP_APP

#define UDP_APP_BUFFER_SIZE 128

/* Descriptor numbers are reserved with eventfd and tracked by protocol. */
static int get_fd_frombitmap(void)
{
    return tcp_fd_alloc();
}

static int set_fd_frombitmap(int fd)
{
    return tcp_fd_release(fd);
}

int netstack_fd_is_managed(int fd)
{
    return tcp_fd_managed(fd);
}

static struct ng_tcp_stream *get_accept_tcb(uint16_t dport)
{

    struct ng_tcp_stream *apt;
    struct ng_tcp_table *table = tcpInstance();
    for (apt = table->streams; apt != NULL; apt = apt->next) {
        if (dport == apt->local_port && apt->fd == -1 && apt->status == NG_TCP_STATUS_ESTABLISHED) {
            return apt;
        }
    }

    return NULL;
}

void *get_hostinfo_fromfd(int socket_fd)
{

    struct localhost *host;

    for (host = localhost_list; host != NULL; host = host->next) {
        if (host->fd == socket_fd) {
            return host;
        }
    }

    struct ng_tcp_stream *stream;

    struct ng_tcp_table *table = tcpInstance();
    for (stream = table->streams; stream != NULL; stream = stream->next) {
        if (socket_fd == stream->fd) {
            return stream;
        }
    }

    return NULL;
}

struct localhost *get_hostinfo_fromip_port(uint32_t ip, uint16_t port, uint8_t protocol)
{
    struct localhost *host;
    for (host = localhost_list; host != NULL; host = host->next) {
        printf("get_hostinfo_fromip_port ---> ip: %s: port %d protocol: %d\n",
               inet_ntoa(*(struct in_addr *)&host->local_ip), ntohs(host->local_port),
               host->protocol);
        if (host->local_ip == ip && host->local_port == port && host->protocol == protocol) {
            return host;
        }
    }
    return NULL;
}

int nsocket(int domain, int type, int protocol)
{
    return (type & 15) == SOCK_DGRAM ? udp_socket_create(domain, type, protocol)
                                     : tcp_socket(domain, type, protocol);
}

int nbind(int fd, const struct sockaddr *socket_addr, socklen_t address_len)
{
    return nisdgram(fd) ? udp_bind(fd, socket_addr, address_len)
                        : tcp_bind(fd, socket_addr, address_len);
}

int nlisten(int fd, int backlog)
{
    if (nisdgram(fd)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return tcp_listen(fd, backlog);
}

int naccept(int fd, struct sockaddr *socket_addr, socklen_t *address_len)
{
    if (nisdgram(fd)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return tcp_accept(fd, socket_addr, address_len);
}

ssize_t nsend(int fd, const void *buffer, size_t buffer_len, int flags)
{
    return nisdgram(fd) ? udp_sendto(fd, buffer, buffer_len, flags, NULL, 0)
                        : tcp_send(fd, buffer, buffer_len, flags);
}

// recv 32
// recv
ssize_t nrecv(int fd, void *buffer, size_t buffer_len, int flags)
{
    return nisdgram(fd) ? udp_recvfrom(fd, buffer, buffer_len, flags, NULL, NULL, NULL)
                        : tcp_recv(fd, buffer, buffer_len, flags);
}

ssize_t nrecvfrom(int fd, void *buffer, size_t buffer_len, int flags, struct sockaddr *peer_addr,
                  socklen_t *address_len)
{
    if (nisdgram(fd))
        return udp_recvfrom(fd, buffer, buffer_len, flags, peer_addr, address_len, NULL);
    if (peer_addr && !address_len) {
        errno = EFAULT;
        return -1;
    }
    ssize_t result = tcp_recv(fd, buffer, buffer_len, flags);
    if (result >= 0 && peer_addr && tcp_name(fd, peer_addr, address_len, 1) < 0)
        return -1;
    return result;
}

ssize_t nsendto(int fd, const void *buffer, size_t buffer_len, int flags,
                const struct sockaddr *peer_addr, socklen_t address_len)
{
    if (nisdgram(fd))
        return udp_sendto(fd, buffer, buffer_len, flags, peer_addr, address_len);
    if (peer_addr) {
        errno = EISCONN;
        return -1;
    }
    return tcp_send(fd, buffer, buffer_len, flags);
}

int nconnect(int fd, const struct sockaddr *socket_addr, socklen_t address_len)
{
    return nisdgram(fd) ? udp_connect(fd, socket_addr, address_len)
                        : tcp_connect(fd, socket_addr, address_len);
}

int nshutdown(int fd, int how)
{
    return nisdgram(fd) ? udp_shutdown(fd, how) : tcp_shutdown(fd, how);
}

int nfcntl(int fd, int command, long argument)
{
    return nisdgram(fd) ? udp_fcntl(fd, command, argument) : tcp_fcntl(fd, command, argument);
}

int nready(int fd)
{
    return nisdgram(fd) ? udp_ready(fd) : tcp_ready(fd);
}

int nioctl(int fd, unsigned long request, void *argument)
{
    return nisdgram(fd) ? udp_ioctl(fd, request, argument) : tcp_ioctl(fd, request, argument);
}

int nsetsockopt(int fd, int level, int option, const void *value, socklen_t option_len)
{
    return nisdgram(fd) ? udp_setsockopt(fd, level, option, value, option_len)
                        : tcp_setsockopt(fd, level, option, value, option_len);
}

int ngetsockopt(int fd, int level, int option, void *value, socklen_t *option_len)
{
    return nisdgram(fd) ? udp_getsockopt(fd, level, option, value, option_len)
                        : tcp_getsockopt(fd, level, option, value, option_len);
}

ssize_t ndgram_sendmsg(int fd, const struct msghdr *message, int flags)
{
    return udp_sendmsg(fd, message, flags);
}

ssize_t ndgram_recvmsg(int fd, struct msghdr *message, int flags)
{
    return udp_recvmsg(fd, message, flags);
}

int nclose(int fd)
{
    return nisdgram(fd) ? udp_close(fd) : tcp_close(fd);
}

/* 判断受管 fd 的类型并回填本地/远端四元组。
 * 注意：不能把 TCP 流强转成 struct localhost* 去读 ->protocol —— ng_tcp_stream 的
 * protocol 是 uint16_t 且后面紧跟 sport，按 int 读会把 sport 混进高位(bind 时
 * peer_port==0 才碰巧正确)。这里显式按列表归属区分 UDP/TCP。 */
static int managed_socket_protocol(int fd, uint32_t *local_ip, uint16_t *local_port,
                                   uint32_t *remote_ip, uint16_t *remote_port)
{

    struct localhost *legacy_endpoint;
    for (legacy_endpoint = localhost_list; legacy_endpoint != NULL;
         legacy_endpoint = legacy_endpoint->next) {
        if (legacy_endpoint->fd == fd) {
            if (local_ip)
                *local_ip = legacy_endpoint->local_ip;
            if (local_port)
                *local_port = legacy_endpoint->local_port;
            return IPPROTO_UDP;
        }
    }

    struct ng_tcp_table *table = tcpInstance();
    struct ng_tcp_stream *tcp_stream;
    for (tcp_stream = table->streams; tcp_stream != NULL; tcp_stream = tcp_stream->next) {
        if (tcp_stream->fd == fd) {
            if (local_ip)
                *local_ip = tcp_stream->local_ip;
            if (local_port)
                *local_port = tcp_stream->local_port;
            if (remote_ip)
                *remote_ip = tcp_stream->peer_ip;
            if (remote_port)
                *remote_port = tcp_stream->peer_port;
            return IPPROTO_TCP;
        }
    }

    return -1;
}

/* getsockname：返回本地地址。ip/port 在结构体里本就是网络字节序，直接拷贝。 */
int ngetsockname(int fd, struct sockaddr *socket_addr, socklen_t *address_len)
{
    return nisdgram(fd) ? udp_name(fd, socket_addr, address_len, 0)
                        : tcp_name(fd, socket_addr, address_len, 0);
}

/* getpeername：返回远端地址。TCP 监听 socket 没有对端，返回 ENOTCONN。 */
int ngetpeername(int fd, struct sockaddr *socket_addr, socklen_t *address_len)
{
    return nisdgram(fd) ? udp_name(fd, socket_addr, address_len, 1)
                        : tcp_name(fd, socket_addr, address_len, 1);
}

int udp_server_entry(__attribute__((unused)) void *arg)
{
    int connfd = nsocket(AF_INET, SOCK_DGRAM, 0);
    if (connfd < 0) {
        printf("nsocket error\n");
        return -1;
    }

    struct sockaddr_in localaddr, clientaddr;
    memset(&localaddr, 0, sizeof(struct sockaddr_in));

    localaddr.sin_family = AF_INET;
    // localaddr.sin_addr.s_addr = htonl(INADDR_ANY);//0.0.0.0
    localaddr.sin_addr.s_addr = inet_addr("192.168.196.132"); // 0.0.0.0
    localaddr.sin_port = htons(8889);

    nbind(connfd, (struct sockaddr *)&localaddr, sizeof(localaddr));
    socklen_t addrlen = sizeof(clientaddr);

    char buffer[UDP_APP_BUFFER_SIZE] = {0};
    while (1) {

        if (nrecvfrom(connfd, buffer, UDP_APP_BUFFER_SIZE, 0, (struct sockaddr *)&clientaddr,
                      &addrlen) < 0) {
            // printf("nrecvfrom error\n");
            continue;
        } else {
            printf("Received UDP packet from %s:%d, data: %s\n", inet_ntoa(clientaddr.sin_addr),
                   ntohs(clientaddr.sin_port), buffer);
            nsendto(connfd, buffer, strlen(buffer), 0, (struct sockaddr *)&clientaddr,
                    sizeof(clientaddr));
        }
    }
    nclose(connfd);
}

#endif

#if ENABLE_KNI_APP

// ifconfig vEth0 up/down

// config_network_if
// rte_kni_handle_request
static int ng_config_network_if(uint16_t port_id, uint8_t if_up)
{ //开关函数

    if (!rte_eth_dev_is_valid_port(port_id)) {
        return -EINVAL;
    }

    int ret = 0;
    if (if_up) {

        rte_eth_dev_stop(port_id);
        ret = rte_eth_dev_start(port_id);

    } else {

        rte_eth_dev_stop(port_id);
    }

    if (ret < 0) {
        printf("Failed to start port : %d\n", port_id);
    }

    return 0;
}

static struct rte_kni *ng_alloc_kni(struct rte_mempool *mbuf_pool)
{

    struct rte_kni *kni_hanlder = NULL;

    struct rte_kni_conf conf; // kni 配置参数
    memset(&conf, 0, sizeof(conf));

    snprintf(conf.name, RTE_KNI_NAMESIZE, "vEth%u", g_dpdk_port_id);
    conf.group_id = g_dpdk_port_id;
    conf.mbuf_size = MAX_PACKET_SIZE;
    rte_eth_macaddr_get(g_dpdk_port_id, (struct rte_ether_addr *)conf.mac_addr);
    rte_eth_dev_get_mtu(g_dpdk_port_id, &conf.mtu);

    print_ether_addr("ng_alloc_kni: ", (struct rte_ether_addr *)conf.mac_addr);

    /*
    struct rte_eth_dev_info dev_info;
    memset(&dev_info, 0, sizeof(dev_info));
    rte_eth_dev_info_get(gDpdkPortId, &dev_info);
    */

    struct rte_kni_ops ops; // kni 操作
    memset(&ops, 0, sizeof(ops));

    ops.port_id = g_dpdk_port_id;
    ops.config_network_if = ng_config_network_if;

    kni_hanlder = rte_kni_alloc(mbuf_pool, &conf, &ops);
    if (!kni_hanlder) {
        rte_exit(EXIT_FAILURE, "Failed to create kni for port : %d\n", g_dpdk_port_id);
    }

    return kni_hanlder;
}

#endif

#if ENABLE_DDOS_DETECT

#define CAPTURE_WINDOWS 256

static double tresh = 1200.0;

static uint32_t p_setbits[CAPTURE_WINDOWS] = {0};
static uint32_t p_totbits[CAPTURE_WINDOWS] = {0};
static double p_entropy[CAPTURE_WINDOWS] = {0};
static int pkt_idx = 0;

/*
 23651/17408, E(nan)
 1805328/4456448 Entropy(nan), Total_Entropy(4339971.575790)

  23526/17408, E(nan)
 1805203/4456448 Entropy(nan), Total_Entropy(4339902.272671)

  17847/17408, E(nan)
 1799524/4456448 Entropy(nan), Total_Entropy(4336731.547140)

  17670/17408, E(nan)
 1799347/4456448 Entropy(nan), Total_Entropy(4336632.027009)

  17774/17408, E(nan)
 1799451/4456448 Entropy(nan), Total_Entropy(4336690.507219)

 */
static double ddos_entropy(double set_bits, double total_bits)
{

    return (-set_bits) * (log2(set_bits) - log2(total_bits)) // 1
           - (total_bits - set_bits) * (log2(total_bits - set_bits) - log2(total_bits)) +
           log2(total_bits);
}

static uint32_t count_bit(uint8_t *msg, const uint32_t length)
{

#if 0
	uint32_t v; // count bits set in this (32-bit value)
	uint32_t c, set_bits = 0; // store the total here
	static const int S[5] = {1, 2, 4, 8, 16}; // Magic Binary Numbers
	static const int B[5] = {0x55555555, 0x33333333, 0x0F0F0F0F, 0x00FF00FF, 0x0000FFFF};


	uint32_t *ptr = (uint32_t *)msg;
	uint32_t *end = (uint32_t *)msg + length;

	

	while (ptr < end) {

		v = *ptr++;
		
		c = v - ((v >> S[0]) & B[0]);
		c = ((c >> S[1]) & B[1]) + (c & B[1]);
		c = ((c >> S[2]) + c) & B[2];
		c = ((c >> S[3]) + c) & B[3];
		c = ((c >> S[4]) + c) & B[4];

		set_bits += c;
	}
#else

    uint64_t v, set_bits = 0;
    const uint64_t *ptr = (uint64_t *)msg;
    const uint64_t *end = (uint64_t *)(msg + length);

    do {
        v = *(ptr++);
        v = v - ((v >> 1) & 0x5555555555555555);                        // reuse input as temporary
        v = (v & 0x3333333333333333) + ((v >> 2) & 0x3333333333333333); // temp
        v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0F;
        set_bits += (v * 0x0101010101010101) >> (sizeof(v) - 1) * 8; // count

    } while (end > ptr);

#endif
    return set_bits;
}

static int ddos_detect(struct rte_mbuf *pkt)
{

    static char flag = 0; // 1 ddos, 0 no attack

    uint8_t *msg = rte_pktmbuf_mtod(pkt, uint8_t *);
    uint32_t set_bits = count_bit(msg, pkt->buf_len);

    uint32_t tot_bits = pkt->buf_len * 8;

    p_setbits[pkt_idx % CAPTURE_WINDOWS] = set_bits;
    p_totbits[pkt_idx % CAPTURE_WINDOWS] = tot_bits;
    p_entropy[pkt_idx % CAPTURE_WINDOWS] = ddos_entropy(set_bits, tot_bits);

    // printf("\n %u/%u, E(%f)\n", set_bits, tot_bits, p_entropy[pkt_idx % CAPTURE_WINDOWS]);
    if (pkt_idx >= CAPTURE_WINDOWS) {

        int i = 0;
        uint32_t total_set = 0, total_bit = 0;
        double sum_entropy = 0.0;

        for (i = 0; i < CAPTURE_WINDOWS; i++) {
            total_set += p_setbits[i]; // set_bits
            total_bit += p_totbits[i]; // count_bits
            sum_entropy += p_entropy[i];
        }

        double entropy = ddos_entropy(total_set, total_bit);

        // nan -->
        printf("%u/%u Entropy(%f), Total_Entropy(%f)\n", total_set, total_bit, sum_entropy,
               entropy);
        if (tresh < sum_entropy - entropy) { // ddos attack

            if (!flag) { // detect
                //
                rte_exit(EXIT_FAILURE, "ddos attack !!! Entropy(%f) < Total_Entropy(%f)\n", entropy,
                         sum_entropy);
            }

            flag = 1;

        } else {

            if (flag) { //

                printf("no new !!! Entropy(%f) < Total_Entropy(%f)\n", entropy, sum_entropy);
            }

            flag = 0;
        }

        // sum(p_entropy[i])
        // ddos_entropy(sum(set_bits), sum(tot_bits));

        pkt_idx = (pkt_idx + 1) % CAPTURE_WINDOWS + CAPTURE_WINDOWS;
    } else {
        pkt_idx++;
    }

    return 0;
}

struct net_key {
    uint32_t sip;
    uint32_t dip;
    uint16_t sport;
    uint16_t dsport;
    char proto;
};

/*四种hash插入方式
key rte_hash_add_key
key+hash
key+data
key+hash+data
*/
static struct rte_hash *creat_hash_table(const char *name)
{
    struct rte_hash_parameters *params =
        (struct rte_hash_parameters *)malloc(sizeof(struct rte_hash_parameters));
    if (!params)
        return NULL;

    params->name = name;
    params->entries = 8192;
    params->key_len = sizeof(struct net_key);
    params->hash_func = rte_jhash;
    params->hash_func_init_val = 0;
    params->socket_id = rte_socket_id(); // numa socket id

    struct rte_hash *hash = rte_hash_create(params);

    return hash;
}

#endif

static struct rte_mempool *g_mbuf_pool = NULL;
static pthread_t g_protocol_thread, g_port_io_thread;
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
static int g_stack_initialized = 0;

/* Diagnostic only: epoll dispatch is based on object ownership. */
static pthread_t g_eal_init_thread;
static int g_eal_init_thread_recorded = 0;

int netstack_is_init_thread(void)
{
    return g_eal_init_thread_recorded && pthread_equal(pthread_self(), g_eal_init_thread);
}

static void *pkt_process_thread(void *arg);
static void *rx_tx_loop(void *arg);

static void netstack_init_once(void)
{
    const char *configured_ip = getenv("DPDKNET_IP");
    if (configured_ip && inet_pton(AF_INET, configured_ip, &g_stack_ipv4_addr) != 1) {
        fprintf(stderr, "Invalid DPDKNET_IP\n");
        return;
    }
    g_eal_init_thread = pthread_self();
    g_eal_init_thread_recorded = 1;

    /* LD_PRELOAD 拿不到 main 的 argv，EAL 参数改从环境变量读取 */
    const char *eal_arguments = getenv("DPDKNET_EAL_ARGS");
    char *argv[32];
    int argc = 1;
    argv[0] = "dpdknet";
    if (eal_arguments != NULL) {
        char *argument_storage = strdup(eal_arguments);
        char *parse_cursor = NULL, *argument_token = strtok_r(argument_storage, " ", &parse_cursor);
        while (argument_token != NULL && argc < 31) {
            argv[argc++] = argument_token;
            argument_token = strtok_r(NULL, " ", &parse_cursor);
        }
    }
    if (rte_eal_init(argc, argv) < 0) {
        fprintf(stderr, "[dpdknet] EAL initialization failed\n");
        return;
    }

    g_mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                          rte_socket_id());
    if (g_mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    }
#if ENABLE_KNI_APP

    if (-1 == rte_kni_init(1)) { //入参无所谓，没有用
        rte_exit(EXIT_FAILURE, "kni init failed\n");
    }
    ng_init_port(g_mbuf_pool);
    // kni_alloc
    global_kni = ng_alloc_kni(g_mbuf_pool);

#else
    ng_init_port(g_mbuf_pool);

#endif

    rte_eth_macaddr_get(g_dpdk_port_id, (struct rte_ether_addr *)&g_port_mac_addr);

#if ENABLE_TIMER

    /* init RTE timer library */
    rte_timer_subsystem_init();
    static struct rte_timer arptimer;
    /* init timer structures */
    rte_timer_init(&arptimer);
    /* load timer0, every second, on master lcore, reloaded automatically */
    uint64_t hz = rte_get_timer_hz();
    unsigned lcore_id = rte_lcore_id();
    rte_timer_reset(&arptimer, hz, PERIODICAL, lcore_id, arp_request_timer_cb, mbuf_pool);

#endif

#if ENABLE_RINGBUFFER

    struct inout_ring *packet_rings = ring_instance();
    if (packet_rings == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to allocate memory for ring instance\n");
    }
    if (packet_rings->in == NULL) {
        packet_rings->in =
            rte_ring_create("IN_RING", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    }
    if (packet_rings->out == NULL) {
        packet_rings->out =
            rte_ring_create("OUT_RING", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    }

#endif

    /* 示例应用(udp_server_entry/tcp_server_entry)不再以 lcore 启动；
     * 数据面改为下面的后台 pthread。 */

    if (pthread_create(&g_protocol_thread, NULL, pkt_process_thread, g_mbuf_pool) != 0)
        return;
    if (pthread_create(&g_port_io_thread, NULL, rx_tx_loop, NULL) != 0)
        return;
    g_stack_initialized = 1;
}

static void *pkt_process_thread(void *arg)
{
    pkt_process(arg);
    return NULL;
}

static void *rx_tx_loop(void *arg)
{
    (void)arg;
    struct inout_ring *packet_rings = ring_instance();
    while (!__atomic_load_n(&g_stack_stop, __ATOMIC_ACQUIRE)) {
        // rx
        struct rte_mbuf *packet[BURST_SIZE];
        unsigned received_count = rte_eth_rx_burst(g_dpdk_port_id, 0, packet, BURST_SIZE);
        if (received_count > BURST_SIZE) {
            rte_exit(EXIT_FAILURE, "Error receiving from ethdev\n");
        } else if (received_count > 0) {
#if ENABLE_DDOS_DETECT

            unsigned i = 0;
            for (i = 0; i < received_count; i++) {
                ddos_detect(packet[i]);
            }
#endif
            rte_ring_sp_enqueue_burst(packet_rings->in, (void **)packet, received_count,
                                      NULL); //入队
        }
        // tx
        struct rte_mbuf *other_packet[BURST_SIZE];
        unsigned outgoing_count = rte_ring_sc_dequeue_burst(
            packet_rings->out, (void **)other_packet, BURST_SIZE, NULL); //出队
        if (outgoing_count > 0) {
            uint16_t sent_count = rte_eth_tx_burst(g_dpdk_port_id, 0, other_packet, outgoing_count);
            if (sent_count < outgoing_count) {
                unsigned i;
                for (i = sent_count; i < outgoing_count; i++) {
                    rte_pktmbuf_free(other_packet[i]);
                }
            }
        }

#if ENABLE_TIMER
        static uint64_t prev_tsc = 0, cur_tsc;
        uint64_t diff_tsc;
        cur_tsc = rte_rdtsc();
        diff_tsc = cur_tsc - prev_tsc;
        if (diff_tsc > TIMER_RESOLUTION_CYCLES) {
            rte_timer_manage();
            prev_tsc = cur_tsc;
        }
#endif
    }
    return NULL;
}

void netstack_fini(void)
{
    if (!g_stack_initialized)
        return;
    pthread_mutex_lock(&socket_state_lock);
    struct ng_tcp_stream *tcp_stream;
    for (tcp_stream = tcp_table.streams; tcp_stream; tcp_stream = tcp_stream->next) {
        if (tcp_stream->status == NG_TCP_STATUS_ESTABLISHED ||
            tcp_stream->status == NG_TCP_STATUS_CLOSE_WAIT)
            tcp_finish(tcp_stream, SHUT_WR);
    }
    pthread_mutex_unlock(&socket_state_lock);
    uint64_t deadline = tcp_now() + 2000;
    while (tcp_now() < deadline) {
        int has_pending_output = 0;
        pthread_mutex_lock(&socket_state_lock);
        for (tcp_stream = tcp_table.streams; tcp_stream; tcp_stream = tcp_stream->next)
            if (tcp_stream->status != NG_TCP_STATUS_CLOSED &&
                (tcp_stream->send_head || tcp_stream->ack_pending)) {
                has_pending_output = 1;
                break;
            }
        if (!has_pending_output) {
            struct udp_socket *udp_endpoint;
            for (udp_endpoint = udp_sockets; udp_endpoint; udp_endpoint = udp_endpoint->next)
                if (udp_endpoint->send_head) {
                    has_pending_output = 1;
                    break;
                }
        }
        pthread_mutex_unlock(&socket_state_lock);
        if (!has_pending_output && rte_ring_empty(rInst->out))
            break;
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    __atomic_store_n(&g_stack_stop, 1, __ATOMIC_RELEASE);
    pthread_join(g_protocol_thread, NULL);
    pthread_join(g_port_io_thread, NULL);
    g_stack_initialized = 0;
}

int netstack_init(void)
{
    pthread_once(&g_init_once, netstack_init_once);
    return g_stack_initialized ? 0 : -1;
}
