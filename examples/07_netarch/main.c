#include <stdio.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_timer.h>

#include "arp.h"

#define ENABLE_SEND 1
#define ENABLE_ARP 1
#define ENABLE_ICMP 1
#define ENABLE_ARP_REPLY 1
#define ENABLE_DPDK_DUG 1
#define ENABLE_TIMER    1
#define ENABLE_RINGBUFFER 1
#define ENABLE_MULTHREAD 1
#define ENABLE_UDP_APP 1

#define ARP_ENTRY_STATUS_DYNAMIC 0
#define ARP_ENTRY_STATUS_STATIC 1

#define NUM_MBUFS (4096 - 1)
#define BURST_SIZE 128


#define MAKE_IPV4_ADDR(a, b, c, d) \
    ((uint32_t)(((a) & 0xff)) | (((b) & 0xff) << 8) | (((c) & 0xff) << 16) | (((d) & 0xff) << 24))
static const uint32_t glocalIp = MAKE_IPV4_ADDR(192, 168, 196, 132);


#define TIMER_RESOLUTION_CYCLES 2000000000ULL /* around 10ms  at 2 Ghz   10ms * 100 */
#define RING_SIZE 1024

#if ENABLE_SEND

//static uint32_t gSrcIp;
//static uint32_t gDstIp;

static uint8_t gDstMac[RTE_ETHER_ADDR_LEN];
static uint8_t gSrcMac[RTE_ETHER_ADDR_LEN];

//static uint16_t gSrcPort;
//static uint16_t gDstPort;

#endif

#ifdef ENABLE_ARP_REPLY

    uint8_t gDefaultArpMac[RTE_ETHER_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#endif

struct localhost {
    int fd;

    unsigned int status;//阻塞与非阻塞
    uint32_t local_ip;
    uint16_t local_port;
    uint8_t local_mac[RTE_ETHER_ADDR_LEN];

    int protocol; // 0: UDP, 1: TCP

    struct rte_ring *snd_ring;
    struct rte_ring *rcv_ring;

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

static struct inout_ring* ring_instance(void) {
    if (rInst == NULL) {
        rInst = (struct inout_ring*)rte_malloc("RING_INSTANCE", sizeof(struct inout_ring), 0);
        if (rInst == NULL) {
            rte_exit(EXIT_FAILURE, "Failed to allocate memory for ring instance\n");
        }
        memset(rInst, 0, sizeof(struct inout_ring));
    }
    return rInst;
}

#endif

int gdpdkportid = 0;
static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
    },
};

struct localhost *get_hostinfo_fromip_port(uint32_t ip, uint16_t port, uint8_t protocol);
static int ng_encode_udp_apppkt(uint8_t *msg, uint32_t sip, uint32_t dip,
    uint16_t sport, uint16_t dport, uint8_t *srcmac, uint8_t *dstmac, uint8_t *data, uint16_t total_len);
static int udp_out(struct rte_mempool *mbuf_pool);
static int get_fd_frombitmap(void);
static struct localhost *get_hostinfo_fromfd(int socket_fd);
int nclose(int fd);
int udp_server_entry(void *arg);
static int nsocket(int domain, int type, int protocol);
static int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen);

static void ng_init_port(struct rte_mempool *mempool) {
    uint16_t nb_sys_ports = rte_eth_dev_count_avail();
    if (nb_sys_ports == 0) {
        rte_exit(EXIT_FAILURE, "No supported Ethernet device found\n");
    }
    struct rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(gdpdkportid, &dev_info);//获取设备信息eth原生信息

    const int rx_rings = 1, tx_rings = 1;
    struct rte_eth_conf port_conf = port_conf_default;
    rte_eth_dev_configure(gdpdkportid, rx_rings, tx_rings, &port_conf);//配置队列数量

    if (rte_eth_rx_queue_setup(gdpdkportid, 0, 1024,
        rte_eth_dev_socket_id(gdpdkportid), NULL, mempool) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot setup rx queue\n");
    }

#if ENABLE_SEND
    struct rte_eth_txconf txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.rxmode.offloads;
    if (rte_eth_tx_queue_setup(gdpdkportid, 0, 1024,
        rte_eth_dev_socket_id(gdpdkportid), &txconf) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot setup tx queue\n");
    }
#endif

    if (rte_eth_dev_start(gdpdkportid) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot start device\n");
    }

    //rte_eth_promiscuous_enable(gdpdkportid);//开启混杂模式，接收所有数据包

}
/*
static void creat_eth_ip_udp(uint8_t *msg, size_t total_len, uint8_t *dst_mac, uint32_t src_ip,
    uint32_t dst_ip, uint16_t src_port, uint16_t dst_port) {
    
    struct rte_ether_hdr *src_mac;

    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth_hdr->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);

    rte_eth_macaddr_get(gdpdkportid, &src_mac->s_addr);
    rte_memcpy(eth_hdr->s_addr.addr_bytes, src_mac, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    ipv4_hdr->version_ihl = 0x45;//版本号4，首部长度5  5*4=20字节   实际IP头长度（字节） = 该字段的值 × 4
    ipv4_hdr->type_of_service = 0;
    ipv4_hdr->total_length = htons(total_len - sizeof(struct rte_ether_hdr));
    ipv4_hdr->src_addr = src_ip;
    ipv4_hdr->dst_addr = dst_ip;
    ipv4_hdr->next_proto_id = IPPROTO_UDP;

}
*/
/*
static int ng_encode_udp_pkt(uint8_t *msg, uint8_t *data, uint16_t total_len) {

    //ethhdr
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth_hdr->d_addr.addr_bytes, gDstMac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth_hdr->s_addr.addr_bytes, gSrcMac, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    //iphdr
    //struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(msg + sizeof(struct rte_ether_hdr));
    ipv4_hdr->version_ihl = 0x45;//版本号是4 长度是5  一个字节没有大小端
    //ipv4_hdr->version = 0x4;
    //ipv4_hdr->ihl = 0x5;
    ipv4_hdr->type_of_service = 0;
    ipv4_hdr->total_length = htons(total_len - sizeof(struct rte_ether_hdr));
    ipv4_hdr->packet_id = 0;
    ipv4_hdr->fragment_offset = 0;
    ipv4_hdr->time_to_live = 64;
    ipv4_hdr->next_proto_id = IPPROTO_UDP;
    ipv4_hdr->src_addr = gSrcIp;
    ipv4_hdr->dst_addr = gDstIp;

    ipv4_hdr->hdr_checksum = 0;//计算校验和前先置0，有脏值
    ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

    //udphdr
    //struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ipv4_hdr + 1);
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(msg + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
    udp_hdr->src_port = gSrcPort;
    udp_hdr->dst_port = gDstPort;
    uint16_t udp_len = total_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr);
    udp_hdr->dgram_len = htons(udp_len);

    rte_memcpy((uint8_t *)(udp_hdr + 1), data, udp_len);
    
    udp_hdr->dgram_cksum = 0;
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);

    return 0;
}*/

static struct rte_mbuf *ng_udp_pkt(struct rte_mempool *mempool, uint32_t sip, uint32_t dip,
    uint16_t sport, uint16_t dport, uint8_t *srcmac, uint8_t *dstmac, uint8_t *data, uint16_t length) {
    
    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + length;//14 + 20 + 8
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
    if (mbuf == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf\n");
    }
    mbuf->data_len = total_len;
    mbuf->pkt_len = total_len;

    uint8_t *pktdata = rte_pktmbuf_mtod(mbuf, uint8_t *);
    ng_encode_udp_apppkt(pktdata, sip, dip,
        sport, dport, srcmac, dstmac, data, total_len);

    return mbuf;
}

static int ng_encode_arp_pkt(uint8_t *msg, uint8_t *dst_mac, uint32_t src_ip, uint32_t dst_ip, uint16_t opcode) {
    //ethhdr
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)msg;
    if (strncmp((char *)dst_mac, (char *)gDefaultArpMac, RTE_ETHER_ADDR_LEN) != 0) {
        rte_memcpy(eth_hdr->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    } else {
        uint8_t mac[RTE_ETHER_ADDR_LEN] = {0x0};
        rte_memcpy(eth_hdr->d_addr.addr_bytes, mac, RTE_ETHER_ADDR_LEN);
    }
    rte_memcpy(eth_hdr->s_addr.addr_bytes, gSrcMac, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = htons(RTE_ETHER_TYPE_ARP);

    //arphdr
    struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
    arp_hdr->arp_hardware = htons(RTE_ARP_HRD_ETHER);
    arp_hdr->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);
    arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;//硬件地址长度 即MAC地址长度
    arp_hdr->arp_plen = sizeof(uint32_t);//协议地址长度 即IP地址长度
    arp_hdr->arp_opcode = htons(opcode);
    rte_memcpy(arp_hdr->arp_data.arp_sha.addr_bytes, gSrcMac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(arp_hdr->arp_data.arp_tha.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    arp_hdr->arp_data.arp_sip = dst_ip;
    arp_hdr->arp_data.arp_tip = src_ip;

    return 0;
}


static struct rte_mbuf *ng_send_arp(struct rte_mempool *mempool,uint16_t opcode, uint8_t *dst_mac, uint32_t src_ip, uint32_t dst_ip) {
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
    rte_memcpy(eth_hdr->s_addr.addr_bytes, gSrcMac, RTE_ETHER_ADDR_LEN);
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

#ifdef ENABLE_TIMER
static void
arp_request_timer_cb(__attribute__((unused)) struct rte_timer *tim,
	  void *arg) {
    struct rte_mempool *mbuf_pool = (struct rte_mempool *)arg;
    struct inout_ring *ring = ring_instance();
#if 0
    struct rte_mbuf *arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, arp_hdr->arp_data.arp_sha.addr_bytes, arp_hdr->arp_data.arp_sip, arp_hdr->arp_data.arp_tip);
    rte_eth_tx_burst(gdpdkportid, 0, &arp_mbuf, 1);
    rte_pktmbuf_free(arp_mbuf);
#endif
    int i = 1;
    for (i = 1; i <= 254; i++) {
        //uint32_t target_ip = MAKE_IPV4_ADDR(192, 168, 196, i);
        uint32_t target_ip = (glocalIp & 0xFFFFFF00) | ((i << 24) & 0xFF000000);
        uint8_t* dst_mac = ng_get_dst_macaddr(target_ip);
        struct in_addr addr;
        addr.s_addr = target_ip;
        printf("arp ---> src: %s\n", inet_ntoa(addr));
        struct rte_mbuf *arp_mbuf = NULL;
        if (dst_mac == NULL) {
            //arphdr --> mac: FF:FF:FF:FF:FF:FF
            //ether  --> mac: 00:00:00:00:00:00
            arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, gDefaultArpMac, glocalIp, target_ip);
        } else {
            arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, dst_mac, glocalIp, target_ip);
        }
        //rte_eth_tx_burst(gdpdkportid, 0, &arp_mbuf, 1);
        //rte_pktmbuf_free(arp_mbuf);
        rte_ring_mp_enqueue_burst(ring->out, (void **)&arp_mbuf, 1, NULL);
    }
}
#endif

static inline void
print_ether_addr(const char *what, struct rte_ether_addr *eth_addr)
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
    struct rte_ipv4_hdr *ipv4_hdr = rte_pktmbuf_mtod_offset(udpmbufs, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)((unsigned char *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));

    struct localhost *host = get_hostinfo_fromip_port(ipv4_hdr->dst_addr, udp_hdr->dst_port, ipv4_hdr->next_proto_id);
    if (host == NULL) {
        rte_pktmbuf_free(udpmbufs);
        return -1;
    }

    struct offload *ol = rte_malloc("OFFLOAD", sizeof(struct offload), 0);
    if (ol == NULL) {
        rte_pktmbuf_free(udpmbufs);
        rte_exit(EXIT_FAILURE, "Failed to allocate memory for offload\n");
        return -1;
    }

    ol->src_ip = ipv4_hdr->src_addr;
    ol->dst_ip = ipv4_hdr->dst_addr;
    ol->src_port = udp_hdr->src_port;
    ol->dst_port = udp_hdr->dst_port;
    ol->protocol = IPPROTO_UDP;
    ol->data_len = ntohs(udp_hdr->dgram_len);
    ol->data = rte_malloc("UDP_DATA", ol->data_len - sizeof(struct rte_udp_hdr), 0);
    if (ol->data == NULL) {
        rte_pktmbuf_free(udpmbufs);
        rte_free(ol);
        rte_exit(EXIT_FAILURE, "Failed to allocate memory for UDP data\n");
        return -1;
    }
    // en

    rte_ring_mp_enqueue(host->rcv_ring, (void*)ol);
    pthread_mutex_lock(&host->mutex);
    pthread_cond_signal(&host->cond);
    pthread_mutex_unlock(&host->mutex);

    rte_pktmbuf_free(udpmbufs);
    return 0;

}

static int ng_encode_udp_apppkt(uint8_t *msg, uint32_t sip, uint32_t dip,
    uint16_t sport, uint16_t dport, uint8_t *srcmac, uint8_t *dstmac, uint8_t *data, uint16_t total_len) {

    //ethhdr
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth_hdr->d_addr.addr_bytes, dstmac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth_hdr->s_addr.addr_bytes, srcmac, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = htons(RTE_ETHER_TYPE_IPV4);

    //iphdr
    //struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(msg + sizeof(struct rte_ether_hdr));
    ipv4_hdr->version_ihl = 0x45;//版本号是4 长度是5  一个字节没有大小端
    //ipv4_hdr->version = 0x4;
    //ipv4_hdr->ihl = 0x5;
    ipv4_hdr->type_of_service = 0;
    ipv4_hdr->total_length = htons(total_len - sizeof(struct rte_ether_hdr));
    ipv4_hdr->packet_id = 0;
    ipv4_hdr->fragment_offset = 0;
    ipv4_hdr->time_to_live = 64;
    ipv4_hdr->next_proto_id = IPPROTO_UDP;
    ipv4_hdr->src_addr = sip;
    ipv4_hdr->dst_addr = dip;

    ipv4_hdr->hdr_checksum = 0;//计算校验和前先置0，有脏值
    ipv4_hdr->hdr_checksum = rte_ipv4_cksum(ipv4_hdr);

    //udphdr
    //struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ipv4_hdr + 1);
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(msg + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
    udp_hdr->src_port = sport;
    udp_hdr->dst_port = dport;
    uint16_t udp_len = total_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr);
    udp_hdr->dgram_len = htons(udp_len);

    rte_memcpy((uint8_t *)(udp_hdr + 1), data, udp_len);
    
    udp_hdr->dgram_cksum = 0;
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);

    return 0;
}


int udp_out(struct rte_mempool *mbuf_pool)
{
    struct localhost *host;
    for (host = localhost_list; host != NULL; host = host->next) {

        struct offload *ol;
        int nb_snd = rte_ring_mc_dequeue(host->snd_ring, (void**)&ol);
        if (nb_snd < 0) continue;

        uint8_t *dstmac = ng_get_dst_macaddr(ol->dst_ip);
        if (dstmac == NULL) {
            struct rte_mbuf *arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REPLY, gDstMac, ol->src_ip, ol->dst_ip);
            
            struct inout_ring *ring = ring_instance();
            rte_ring_mp_enqueue_burst(ring->out, (void **)&arp_mbuf, 1, NULL);

            rte_ring_mp_enqueue(host->snd_ring, (void*)ol);
        } else {
            struct rte_mbuf *udpbuf = ng_udp_pkt(mbuf_pool, ol->src_ip, ol->dst_ip, ol->src_port, ol->dst_port, gSrcMac, dstmac, ol->data, ol->data_len);

            struct inout_ring *ring = ring_instance();
            rte_ring_mp_enqueue_burst(ring->out, (void **)&udpbuf, 1, NULL);
        }
    }

    return 0;    
}

static int pkt_process(void *arg)
{
	struct rte_mempool *mbuf_pool = (struct rte_mempool *)arg;
    struct inout_ring *ring = ring_instance();
    while(1) {
        struct rte_mbuf *bufs[BURST_SIZE];
        unsigned num_recv = rte_ring_mc_dequeue_burst(ring->in, (void **)bufs, BURST_SIZE, NULL);

        unsigned i;
        for (i = 0; i < num_recv; i++) {
            struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *);
#if ENABLE_ARP
            if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                //struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
                struct rte_arp_hdr *arp_hdr = rte_pktmbuf_mtod_offset(bufs[i], struct rte_arp_hdr *, sizeof(struct rte_ether_hdr));
                
                struct in_addr addr;
                addr.s_addr = arp_hdr->arp_data.arp_sip;
                printf("arp ---> src: %s", inet_ntoa(addr));
                addr.s_addr = glocalIp;
                printf(" local: %s", inet_ntoa(addr));
                addr.s_addr = arp_hdr->arp_data.arp_tip;
                printf("arp ---> target: %s\n", inet_ntoa(addr));

                if (arp_hdr->arp_data.arp_tip == glocalIp) {
                    if(arp_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST)) {
                        struct rte_mbuf *arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REPLY, arp_hdr->arp_data.arp_sha.addr_bytes, arp_hdr->arp_data.arp_sip, arp_hdr->arp_data.arp_tip);
                        //rte_eth_tx_burst(gdpdkportid, 0, &arp_mbuf, 1);
                        //rte_pktmbuf_free(arp_mbuf);
                        rte_ring_mp_enqueue_burst(ring->out, (void **)&arp_mbuf, 1, NULL);

                    } else if(arp_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY)) {
                        uint8_t *hwaddr = ng_get_dst_macaddr(arp_hdr->arp_data.arp_sip);
                        if (hwaddr == NULL) {
                            struct arp_table *table = arp_table_instance();
                            struct arp_table_entry *entry = (struct arp_table_entry *)rte_malloc("ARP_ENTRY", sizeof(struct arp_table_entry), 0);
                            if (entry) {
                                memset(entry, 0, sizeof(struct arp_table_entry));
                                entry->ip_addr = arp_hdr->arp_data.arp_sip;
                                rte_memcpy(entry->mac_addr, arp_hdr->arp_data.arp_sha.addr_bytes, RTE_ETHER_ADDR_LEN);
                                LL_ADD(entry, table->entries);
                                entry->type = ARP_ENTRY_STATUS_DYNAMIC;
                                table->count++;
                            }
                        }
#if ENABLE_DPDK_DUG
                    struct arp_table_entry *iter = arpt->entries;
                    while (iter != NULL) {
                        print_ether_addr("arp entry --> mac: ", (struct rte_ether_addr *)iter->mac_addr);
                        printf(" arp entry --> ip: %s\n", inet_ntoa(*(struct in_addr *)&iter->ip_addr));
                        iter = iter->next;
                    }
#endif
                    }
                }
                rte_pktmbuf_free(bufs[i]);
                continue;
            }
#endif

            if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                rte_pktmbuf_free(bufs[i]);
                continue;
            }
            struct rte_ipv4_hdr *ipv4_hdr = rte_pktmbuf_mtod_offset(bufs[i], struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
            if (ipv4_hdr->next_proto_id == IPPROTO_UDP) {
                udp_process(bufs[i]);
            }
#if ENABLE_ICMP
            if (ipv4_hdr->next_proto_id == IPPROTO_ICMP) {
                struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)((unsigned char *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));
                struct in_addr addr;
                addr.s_addr = ipv4_hdr->src_addr;
                printf("icmp ---> src: %s", inet_ntoa(addr));
                addr.s_addr = glocalIp;
                printf(" local: %s", inet_ntoa(addr));
                addr.s_addr = ipv4_hdr->dst_addr;
                printf("icmp ---> target: %s\n", inet_ntoa(addr));
                if (icmp_hdr->icmp_type == RTE_IP_ICMP_ECHO_REQUEST) {
                    struct rte_mbuf *icmp_mbuf = ng_send_icmp(mbuf_pool, eth_hdr->s_addr.addr_bytes,
                        ipv4_hdr->dst_addr, ipv4_hdr->src_addr, icmp_hdr->icmp_ident, icmp_hdr->icmp_seq_nb);
                    //rte_eth_tx_burst(gdpdkportid, 0, &icmp_mbuf, 1);
                    //rte_pktmbuf_free(icmp_mbuf);
                    rte_ring_mp_enqueue_burst(ring->out, (void **)&icmp_mbuf, 1, NULL);
                    rte_pktmbuf_free(bufs[i]);
                }
            }
#endif
        }
#if ENABLE_UDP_APP
        udp_out(mbuf_pool);
#endif
    }
    return 0;
}

#if ENABLE_UDP_APP

#define UDP_APP_BUFFER_SIZE 128



#define DEFAULT_FD_NUM 3
int get_fd_frombitmap(void) {
    int fd = DEFAULT_FD_NUM;
    return fd;
}


struct localhost *get_hostinfo_fromfd(int socket_fd) {
    for (struct localhost *host = localhost_list; host != NULL; host = host->next) {
        if (host->fd == socket_fd) {
            return host;
        }
    }
    return NULL;
}

struct localhost *get_hostinfo_fromip_port(uint32_t ip, uint16_t port, uint8_t protocol) {
    for (struct localhost *host = localhost_list; host != NULL; host = host->next) {
        if (host->local_ip == ip && host->local_port == port && host->protocol == protocol) {
            return host;
        }
    }
    return NULL;
}

int nsocket(int domain, int type, int protocol) {
    (void)domain;
    (void)protocol;
    int fd = get_fd_frombitmap();//0,1,2是标准输入输出错误

    struct localhost *host = (struct localhost *)rte_malloc("LOCALHOST", sizeof(struct localhost), 0);
    if (host == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to allocate memory for localhost\n");
        return -1;
    }
    memset(host, 0, sizeof(struct localhost));
    host->fd = fd;
    if (type == SOCK_DGRAM) {
        host->protocol = IPPROTO_UDP;
    } else if (type == SOCK_STREAM) {
        host->protocol = IPPROTO_TCP;
    } else {
        rte_exit(EXIT_FAILURE, "Unsupported nsocket type\n");
        return -1;
    }
    host->snd_ring = rte_ring_create("SND_RING", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (host->snd_ring == NULL) {
        rte_free(host);
        rte_exit(EXIT_FAILURE, "Failed to create send ring\n");
        return -1;
    }
    host->rcv_ring = rte_ring_create("RCV_RING", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (host->rcv_ring == NULL) {
        rte_ring_free(host->snd_ring);
        rte_free(host);
        rte_exit(EXIT_FAILURE, "Failed to create receive ring\n");
        return -1;
    }
    pthread_cond_t blank_cond = PTHREAD_COND_INITIALIZER;
    rte_memcpy(&host->cond, &blank_cond, sizeof(pthread_cond_t));

    pthread_mutex_t blank_mutex = PTHREAD_MUTEX_INITIALIZER;
    rte_memcpy(&host->mutex, &blank_mutex, sizeof(pthread_mutex_t));

    LL_ADD(host, localhost_list);

    return fd;
}

int nbind(int sockfd, const struct sockaddr *addr,
                socklen_t addrlen) {
    (void)addrlen;
    struct localhost *host = get_hostinfo_fromfd(sockfd);
    if (host == NULL) {
        printf("Invalid nsocket file descriptor\n");
        return -1;
    }
    const struct sockaddr_in *local_addr = (const struct sockaddr_in *)addr;
    host->local_port = local_addr->sin_port;
    host->local_ip = local_addr->sin_addr.s_addr;
    rte_memcpy(host->local_mac, gSrcMac, RTE_ETHER_ADDR_LEN);

    return 0;
}

ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags,
                        struct sockaddr *src_addr, socklen_t *addrlen) {
    (void)flags;
    (void)addrlen;
    struct localhost *host = get_hostinfo_fromfd(sockfd);
    if (host == NULL) {
        printf("Invalid nsocket file descriptor\n");
        return -1;
    }
    struct offload *ol = NULL;
    unsigned char *ptr = NULL;
    struct sockaddr_in *saddr = (struct sockaddr_in *)src_addr;
    int nb = -1;
    pthread_mutex_lock(&host->mutex);
    while ( (nb = rte_ring_mc_dequeue(host->rcv_ring, (void**)&ol)) < 0) {
        pthread_cond_wait(&host->cond, &host->mutex);
    }
    pthread_mutex_unlock(&host->mutex);

    saddr->sin_port = ol->dst_port;
    rte_memcpy(&saddr->sin_addr.s_addr, &ol->src_ip, sizeof(uint32_t));

    if (len < ol->data_len) {

        rte_memcpy(buf, ol->data, len);

        ptr = rte_malloc("unsigned char*", ol->data_len - len, 0);
        rte_memcpy(ptr, ol->data + len, ol->data_len - len);

        ol->data_len -= len;
        rte_free(ol->data);
        ol->data = ptr;

        rte_ring_mp_enqueue(host->rcv_ring, (void*)ol);
        return len;
    } else {
        rte_memcpy(buf, ol->data, ol->data_len);
        rte_free(ol->data);
        rte_free(ol);
        return ol->data_len;
    }
}

ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                      const struct sockaddr *dest_addr, socklen_t addrlen)
{
    (void)flags;
    (void)addrlen;
    struct localhost *host = get_hostinfo_fromfd(sockfd);
    if (host == NULL) {
        printf("Invalid nsocket file descriptor\n");
        return -1;
    }

    const struct sockaddr_in *daddr = (const struct sockaddr_in *)dest_addr;
    struct offload *ol = rte_malloc("offload", sizeof(struct offload), 0);
    if (ol == NULL) return -1;

    ol->dst_ip = daddr->sin_addr.s_addr;
    ol->dst_port = daddr->sin_port;
    ol->src_ip = host->local_ip;
    ol->src_port = host->local_port;
    ol->data_len = len;

    ol->data = rte_malloc("unsigned char*",len, 0);
    if (ol->data == NULL) {
        rte_free(ol);
        return -1;
    }
    rte_memcpy(ol->data, buf, len);

    rte_ring_mp_enqueue(host->snd_ring, (void*)ol);

    return len;
}

int nclose(int fd) {
    struct localhost *host = get_hostinfo_fromfd(fd);
    if (host == NULL) {
        printf("Invalid nsocket file descriptor\n");
        return -1;
    }
    if (host->snd_ring) {
        rte_ring_free(host->snd_ring);
    }
    if (host->rcv_ring) {
        rte_ring_free(host->rcv_ring);
    }
    LL_REMOVE(host, localhost_list);
    rte_free(host);
    return 0;
}

int udp_server_entry(void *arg) {
    (void)arg;
    int connfd = nsocket(AF_INET, SOCK_DGRAM, 0);
    if (connfd < 0) {
        printf("nsocket error\n");
        return -1;
    }

    struct sockaddr_in localaddr, clientaddr;
    memset(&localaddr, 0, sizeof(localaddr));

    localaddr.sin_family = AF_INET;
    localaddr.sin_addr.s_addr = htonl(INADDR_ANY);//0.0.0.0
    localaddr.sin_port = htons(8889);

    nbind(connfd, (struct sockaddr *)&localaddr, sizeof(localaddr));
    socklen_t addrlen = sizeof(clientaddr);

    char buffer[UDP_APP_BUFFER_SIZE] = {0};
    while (1) {

        if (nrecvfrom(connfd, buffer, UDP_APP_BUFFER_SIZE, 0,
            (struct sockaddr *)&clientaddr, &addrlen) < 0) {
            printf("nrecvfrom error\n");
            continue;
        } else {
            printf("Received UDP packet from %s:%d, data: %s\n",
                inet_ntoa(clientaddr.sin_addr), ntohs(clientaddr.sin_port), buffer);
            nsendto(connfd, buffer, strlen(buffer), 0,
                (struct sockaddr *)&clientaddr, sizeof(clientaddr));
        }
    }
    nclose(connfd);
}

#endif

int main(int argc, char *argv[])
{
    if(rte_eal_init(argc, argv) < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
        0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if(mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    }
    ng_init_port(mbuf_pool);

    rte_eth_macaddr_get(gdpdkportid, (struct rte_ether_addr*)&gSrcMac);
#ifdef ENABLE_TIMER

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

#ifdef ENABLE_RINGBUFFER

    struct inout_ring *ring = ring_instance();
    if(ring == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to allocate memory for ring instance\n");
    }
    if(ring->in == NULL) {
        ring->in = rte_ring_create("IN_RING", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    }
    if(ring->out == NULL) {
        ring->out = rte_ring_create("OUT_RING", RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    }

#endif

#if ENABLE_MULTHREAD
    int local_id;
    local_id = rte_get_next_lcore(lcore_id, 1, 0);
    rte_eal_remote_launch(pkt_process, mbuf_pool, local_id);
#endif

#if ENABLE_UDP_APP
    local_id = rte_get_next_lcore(lcore_id, 1, 0);
    rte_eal_remote_launch(udp_server_entry, NULL, local_id);

#endif
    while(1) {
        //rx
        struct rte_mbuf *rx[BURST_SIZE];
        unsigned num_recv = rte_eth_rx_burst(gdpdkportid, 0, rx, BURST_SIZE);
        if (num_recv > BURST_SIZE) {
            rte_exit(EXIT_FAILURE, "Error receiving from ethdev\n");
        } else if (num_recv > 0) {
            rte_ring_sp_enqueue_burst(ring->in, (void **)rx, num_recv, NULL);//入队
        }
        //tx
        struct rte_mbuf *tx[BURST_SIZE];
        unsigned nb_tx = rte_ring_sc_dequeue_burst(ring->out, (void **)tx, BURST_SIZE, NULL);//出队
        if (nb_tx > 0) {
            rte_eth_tx_burst(gdpdkportid, 0, tx, nb_tx);
            unsigned i;
            for (i = 0; i < nb_tx; i++) {
                rte_pktmbuf_free(tx[i]);
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
}