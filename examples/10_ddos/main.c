#include <stdio.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#include <rte_kni.h>

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
#define ENABLE_TCP_APP 1
#define ENABLE_KNI_APP 1

#define ARP_ENTRY_STATUS_DYNAMIC 0
#define ARP_ENTRY_STATUS_STATIC 1

#define NUM_MBUFS (4096 - 1)
#define BURST_SIZE 128
#define MAX_FD_COUNT	1024
#define MAX_PACKET_SIZE		2048

#define MAKE_IPV4_ADDR(a, b, c, d) \
    ((uint32_t)(((a) & 0xff)) | (((b) & 0xff) << 8) | (((c) & 0xff) << 16) | (((d) & 0xff) << 24))
static const uint32_t glocalIp = MAKE_IPV4_ADDR(192, 168, 196, 132);

#define TIMER_RESOLUTION_CYCLES 2000000000ULL /* around 10ms  at 2 Ghz   10ms * 100 */
#define RING_SIZE 1024

#if ENABLE_SEND

//static uint32_t gSrcIp;
//static uint32_t gDstIp;

//static uint8_t gDstMac[RTE_ETHER_ADDR_LEN];
static uint8_t gSrcMac[RTE_ETHER_ADDR_LEN];

//static uint16_t gSrcPort;
//static uint16_t gDstPort;

#endif

#ifdef ENABLE_ARP_REPLY

    uint8_t gDefaultArpMac[RTE_ETHER_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#endif
#if ENABLE_KNI_APP

struct rte_kni *global_kni = NULL;

#endif
struct localhost {
    int fd;

    //unsigned int status;//阻塞与非阻塞
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

static int ng_arp_entry_insert(uint32_t ip, uint8_t *mac) {

	struct arp_table *table = arp_table_instance();

	uint8_t *hwaddr = ng_get_dst_macaddr(ip);
	if (hwaddr == NULL) {

		struct arp_table_entry *entry = rte_malloc("arp_entry",sizeof(struct arp_table_entry), 0);
		if (entry) {
			memset(entry, 0, sizeof(struct arp_table_entry));

			entry->ip_addr = ip;
			rte_memcpy(entry->mac_addr, mac, RTE_ETHER_ADDR_LEN);
			entry->type = 0;

			pthread_spin_lock(&table->spinlock);
			LL_ADD(entry, table->entries);
			table->count ++;
			pthread_spin_unlock(&table->spinlock);
			
		}

		return 1; //
	}

	return 0;
}

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
void* get_hostinfo_fromfd(int socket_fd);
static int nclose(int fd);
int udp_server_entry(void *arg);
static int nsocket(int domain, int type, int protocol);
static int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen);
static int nlisten(int sockfd, __attribute__((unused)) int backlog);

#if ENABLE_TCP_APP
static int naccept(int sockfd, struct sockaddr *addr, __attribute__((unused)) socklen_t *addrlen);
static int ng_tcp_process(struct rte_mbuf *tcpmbuf);
static int ng_tcp_out(struct rte_mempool *mbuf_pool);
static ssize_t nsend(int sockfd, const void *buf, size_t len,__attribute__((unused)) int flags);
static ssize_t nrecv(int sockfd, void *buf, size_t len, __attribute__((unused)) int flags);

#endif


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
/*
    if (strncmp((char *)dst_mac, (char *)gDefaultArpMac, RTE_ETHER_ADDR_LEN) != 0) {
        rte_memcpy(eth_hdr->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    } else {
        uint8_t mac[RTE_ETHER_ADDR_LEN] = {0x0};
        rte_memcpy(eth_hdr->d_addr.addr_bytes, mac, RTE_ETHER_ADDR_LEN);
    }
*/

    rte_memcpy(eth_hdr->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
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
    //rte_memcpy(arp_hdr->arp_data.arp_tha.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    if (strncmp((char *)dst_mac, (char *)gDefaultArpMac, RTE_ETHER_ADDR_LEN) != 0) {
        rte_memcpy(arp_hdr->arp_data.arp_tha.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    } else {
        uint8_t mac[RTE_ETHER_ADDR_LEN] = {0x0};
        rte_memcpy(arp_hdr->arp_data.arp_tha.addr_bytes, mac, RTE_ETHER_ADDR_LEN);
    }

    arp_hdr->arp_data.arp_tip = dst_ip;
    arp_hdr->arp_data.arp_sip = src_ip;

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
#endif
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
        uint32_t target_ip = (glocalIp & 0x00FFFFFF) | ((i << 24) & 0xFF000000);
        uint8_t* dst_mac = ng_get_dst_macaddr(target_ip);
        //struct in_addr addr;
        //addr.s_addr = target_ip;
        //printf("arp ---> src: %s\n", inet_ntoa(addr));
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
    //printf("udp_process host: %p\n", (void*)host);
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
    rte_memcpy(ol->data, (unsigned char*)(udp_hdr + 1), ol->data_len - sizeof(struct rte_udp_hdr));
    rte_ring_sp_enqueue(host->rcvbuf, (void*)ol);
    pthread_mutex_lock(&host->mutex);
    pthread_cond_signal(&host->cond);
    pthread_mutex_unlock(&host->mutex);
    //printf("udp_process ---> src: %s:%d \n", inet_ntoa(*(struct in_addr *)&ol->src_ip), ntohs(ol->src_port));
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

    //rte_memcpy((uint8_t *)(udp_hdr + 1), data, udp_len);
    rte_memcpy((uint8_t *)(udp_hdr + 1), data, udp_len - sizeof(struct rte_udp_hdr));
    
    udp_hdr->dgram_cksum = 0;
    udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4_hdr, udp_hdr);

    return 0;
}


int udp_out(struct rte_mempool *mbuf_pool)
{
    struct localhost *host;
    for (host = localhost_list; host != NULL; host = host->next) {

        struct offload *ol;
        int nb_snd = rte_ring_sc_dequeue(host->sndbuf, (void**)&ol);
        if (nb_snd < 0) continue;

        struct in_addr addr;
		addr.s_addr = ol->dst_ip;
		printf("udp_out ---> src: %s:%d \n", inet_ntoa(addr), ntohs(ol->dst_port));
    
        uint8_t *dstmac = ng_get_dst_macaddr(ol->dst_ip);
        if (dstmac == NULL) {
            //struct rte_mbuf *arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REPLY, gDstMac, ol->src_ip, ol->dst_ip);
            struct rte_mbuf *arp_mbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, gDefaultArpMac, ol->src_ip, ol->dst_ip);
            struct inout_ring *ring = ring_instance();
            rte_ring_mp_enqueue_burst(ring->out, (void **)&arp_mbuf, 1, NULL);

            rte_ring_sp_enqueue(host->sndbuf, (void*)ol);
        } else {
            struct rte_mbuf *udpbuf = ng_udp_pkt(mbuf_pool, ol->src_ip, ol->dst_ip, ol->src_port, ol->dst_port, host->local_mac, dstmac, ol->data, ol->data_len);

            struct inout_ring *ring = ring_instance();
            rte_ring_mp_enqueue_burst(ring->out, (void **)&udpbuf, 1, NULL);
        }
    }

    return 0;    
}


#if ENABLE_TCP_APP // ngtcp


#define TCP_OPTION_LENGTH	10

#define TCP_MAX_SEQ		4294967295

#define TCP_INITIAL_WINDOW  14600

typedef enum _NG_TCP_STATUS {

	NG_TCP_STATUS_CLOSED = 0,
	NG_TCP_STATUS_LISTEN,
	NG_TCP_STATUS_SYN_RCVD,
	NG_TCP_STATUS_SYN_SENT,
	NG_TCP_STATUS_ESTABLISHED,

	NG_TCP_STATUS_FIN_WAIT_1,
	NG_TCP_STATUS_FIN_WAIT_2,
	NG_TCP_STATUS_CLOSING,
	NG_TCP_STATUS_TIME_WAIT,

	NG_TCP_STATUS_CLOSE_WAIT,
	NG_TCP_STATUS_LAST_ACK

} NG_TCP_STATUS;


struct ng_tcp_stream { // tcb control block

	int fd; //

	uint32_t dip;
    uint8_t localmac[RTE_ETHER_ADDR_LEN];
	uint16_t dport;

	uint16_t protocol;

    uint16_t sport;
    uint32_t sip;

	uint32_t snd_nxt; // seqnum
	uint32_t rcv_nxt; // acknum

	NG_TCP_STATUS status;

	struct rte_ring *sndbuf;
	struct rte_ring *rcvbuf;

	struct ng_tcp_stream *prev;
	struct ng_tcp_stream *next;

    pthread_cond_t cond;
	pthread_mutex_t mutex;

};

struct ng_tcp_table {
	int count;
	struct ng_tcp_stream *tcb_set;
};

struct ng_tcp_fragment { 

	uint16_t sport;  
	uint16_t dport;  
	uint32_t seqnum;  
	uint32_t acknum;  
	uint8_t  hdrlen_off;  
	uint8_t  tcp_flags; 
	uint16_t windows;   
	uint16_t cksum;     
	uint16_t tcp_urp;  

	int optlen;
	uint32_t option[TCP_OPTION_LENGTH];

	unsigned char *data;
	int length;

};


struct ng_tcp_table *tInst = NULL;

static struct ng_tcp_table *tcpInstance(void) {

	if (tInst == NULL) {

		tInst = rte_malloc("ng_tcp_table", sizeof(struct ng_tcp_table), 0);
		memset(tInst, 0, sizeof(struct ng_tcp_table));
		
	}
	return tInst;
}


static struct ng_tcp_stream * ng_tcp_stream_search(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport) { // proto

	struct ng_tcp_table *table = tcpInstance();

	struct ng_tcp_stream *iter;
	for (iter = table->tcb_set;iter != NULL; iter = iter->next) { // established

		if (iter->sip == sip && iter->dip == dip && 
			iter->sport == sport && iter->dport == dport) {
			return iter;
		}

	}

	for (iter = table->tcb_set;iter != NULL; iter = iter->next) {

		if (iter->dport == dport && iter->status == NG_TCP_STATUS_LISTEN) { // listen
			return iter;
		}

	}

	return NULL;
}

static struct ng_tcp_stream * ng_tcp_stream_create(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport) { // proto

	// tcp --> status
	struct ng_tcp_stream *stream = rte_malloc("ng_tcp_stream", sizeof(struct ng_tcp_stream), 0);
	if (stream == NULL) return NULL;

	stream->sip = sip;
	stream->dip = dip;
	stream->sport = sport;
	stream->dport = dport;
	stream->protocol = IPPROTO_TCP;
	stream->fd = -1; //unused

	// 
	stream->status = NG_TCP_STATUS_LISTEN;

	printf("ng_tcp_stream_create\n");
	//
	static uint32_t stream_cnt = 0;
	char sndname[32], rcvname[32];
	snprintf(sndname, sizeof(sndname), "sndbuf_%u", stream_cnt);
	snprintf(rcvname, sizeof(rcvname), "rcvbuf_%u", stream_cnt);
	stream_cnt++;

	stream->sndbuf = rte_ring_create(sndname, RING_SIZE, rte_socket_id(), 0);
	if (stream->sndbuf == NULL) {
		rte_free(stream);
		return NULL;
	}
	stream->rcvbuf = rte_ring_create(rcvname, RING_SIZE, rte_socket_id(), 0);
	if (stream->rcvbuf == NULL) {
		rte_ring_free(stream->sndbuf);
		rte_free(stream);
		return NULL;
	}
	
	// seq num
	uint32_t next_seed = time(NULL);
	stream->snd_nxt = rand_r(&next_seed) % TCP_MAX_SEQ;
	rte_memcpy(stream->localmac, gSrcMac, RTE_ETHER_ADDR_LEN);

	pthread_cond_t blank_cond = PTHREAD_COND_INITIALIZER;
	rte_memcpy(&stream->cond, &blank_cond, sizeof(pthread_cond_t));

	pthread_mutex_t blank_mutex = PTHREAD_MUTEX_INITIALIZER;
	rte_memcpy(&stream->mutex, &blank_mutex, sizeof(pthread_mutex_t));

	//struct ng_tcp_table *table = tcpInstance();
	//LL_ADD(stream, table->tcb_set);

	return stream;
}



static int ng_tcp_handle_listen(struct ng_tcp_stream *stream, struct rte_tcp_hdr *tcphdr,struct rte_ipv4_hdr *iphdr) {
    printf("ng_tcp_handle_listen: %d, %d\n", stream->rcv_nxt, ntohs(tcphdr->sent_seq));
	if (tcphdr->tcp_flags & RTE_TCP_SYN_FLAG)  {

		if (stream->status == NG_TCP_STATUS_LISTEN) {

            struct ng_tcp_table *table = tcpInstance();
			struct ng_tcp_stream *syn = ng_tcp_stream_create(iphdr->src_addr, iphdr->dst_addr, tcphdr->src_port, tcphdr->dst_port);
			LL_ADD(syn, table->tcb_set);

			struct ng_tcp_fragment *fragment = rte_malloc("ng_tcp_fragment", sizeof(struct ng_tcp_fragment), 0);
			if (fragment == NULL) return -1;
			memset(fragment, 0, sizeof(struct ng_tcp_fragment));

			fragment->sport = tcphdr->dst_port;
			fragment->dport = tcphdr->src_port;

			struct in_addr addr;
			addr.s_addr = syn->sip;
			printf("tcp ---> src: %s:%d ", inet_ntoa(addr), ntohs(tcphdr->src_port));

			
			addr.s_addr = syn->dip;
			printf("  ---> dst: %s:%d \n", inet_ntoa(addr), ntohs(tcphdr->dst_port));

			fragment->seqnum = syn->snd_nxt;
			fragment->acknum = ntohl(tcphdr->sent_seq) + 1;
			syn->rcv_nxt = fragment->acknum;
			
			fragment->tcp_flags = (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG);
			fragment->windows = TCP_INITIAL_WINDOW;
			fragment->hdrlen_off = 0x50;
			
			fragment->data = NULL;
			fragment->length = 0;

			rte_ring_mp_enqueue(syn->sndbuf, fragment);
			
			syn->status = NG_TCP_STATUS_SYN_RCVD;
		}

	}

	return 0;
}


static int ng_tcp_handle_syn_rcvd(struct ng_tcp_stream *stream, struct rte_tcp_hdr *tcphdr) {

	if (tcphdr->tcp_flags & RTE_TCP_ACK_FLAG) {

		if (stream->status == NG_TCP_STATUS_SYN_RCVD) {

			uint32_t acknum = ntohl(tcphdr->recv_ack);
			if (acknum == stream->snd_nxt + 1) {
				// 
			}

			stream->status = NG_TCP_STATUS_ESTABLISHED;

			// accept
			struct ng_tcp_stream *listener = ng_tcp_stream_search(0, 0, 0, stream->dport);
			if (listener == NULL) {
				rte_exit(EXIT_FAILURE, "ng_tcp_stream_search failed\n");
			}

			pthread_mutex_lock(&listener->mutex);
			pthread_cond_signal(&listener->cond);
			pthread_mutex_unlock(&listener->mutex);
			

		}

	}
	return 0;
}

static int ng_tcp_enqueue_recvbuffer(struct ng_tcp_stream *stream, struct rte_tcp_hdr *tcphdr, int tcplen) {

	// recv buffer
	struct ng_tcp_fragment *rfragment = rte_malloc("ng_tcp_fragment", sizeof(struct ng_tcp_fragment), 0);
	if (rfragment == NULL) return -1;
	memset(rfragment, 0, sizeof(struct ng_tcp_fragment));

	rfragment->dport = ntohs(tcphdr->dst_port);
	rfragment->sport = ntohs(tcphdr->src_port);

	uint8_t hdrlen = tcphdr->data_off >> 4;
	int payloadlen = tcplen - hdrlen * 4; //
	if (payloadlen > 0) {
		
		uint8_t *payload = (uint8_t*)tcphdr + hdrlen * 4;

		rfragment->data = rte_malloc("unsigned char *", payloadlen+1, 0);
		if (rfragment->data == NULL) {
			rte_free(rfragment);
			return -1;
		}
		memset(rfragment->data, 0, payloadlen+1);

		rte_memcpy(rfragment->data, payload, payloadlen);
		rfragment->length = payloadlen;

	} else if (payloadlen == 0) {

		rfragment->length = 0;
		rfragment->data = NULL;

	}
	rte_ring_mp_enqueue(stream->rcvbuf, rfragment);

	pthread_mutex_lock(&stream->mutex);
	pthread_cond_signal(&stream->cond);
	pthread_mutex_unlock(&stream->mutex);

	return 0;
}

static int ng_tcp_send_ackpkt(struct ng_tcp_stream *stream, struct rte_tcp_hdr *tcphdr) {

	struct ng_tcp_fragment *ackfrag = rte_malloc("ng_tcp_fragment", sizeof(struct ng_tcp_fragment), 0);
	if (ackfrag == NULL) return -1;
	memset(ackfrag, 0, sizeof(struct ng_tcp_fragment));

	ackfrag->dport = tcphdr->src_port;
	ackfrag->sport = tcphdr->dst_port;

	// remote
	
	printf("ng_tcp_send_ackpkt: %d, %d\n", stream->rcv_nxt, ntohs(tcphdr->sent_seq));
	

	ackfrag->acknum = stream->rcv_nxt;
	ackfrag->seqnum = stream->snd_nxt;

	ackfrag->tcp_flags = RTE_TCP_ACK_FLAG;
	ackfrag->windows = TCP_INITIAL_WINDOW;
	ackfrag->hdrlen_off = 0x50;
	ackfrag->data = NULL;
	ackfrag->length = 0;
	
	rte_ring_mp_enqueue(stream->sndbuf, ackfrag);

	return 0;
}

static int ng_tcp_handle_established(struct ng_tcp_stream *stream, struct rte_tcp_hdr *tcphdr, int tcplen) {

	if (tcphdr->tcp_flags & RTE_TCP_SYN_FLAG) {
		//
	} 
	if (tcphdr->tcp_flags & RTE_TCP_PSH_FLAG) { //

		// recv buffer
#if 0
		struct ng_tcp_fragment *rfragment = rte_malloc("ng_tcp_fragment", sizeof(struct ng_tcp_fragment), 0);
		if (rfragment == NULL) return -1;
		memset(rfragment, 0, sizeof(struct ng_tcp_fragment));

		rfragment->dport = ntohs(tcphdr->dst_port);
		rfragment->sport = ntohs(tcphdr->src_port);

		uint8_t hdrlen = tcphdr->data_off >> 4;
		int payloadlen = tcplen - hdrlen * 4;
		if (payloadlen > 0) {
			
			uint8_t *payload = (uint8_t*)tcphdr + hdrlen * 4;

			rfragment->data = rte_malloc("unsigned char *", payloadlen+1, 0);
			if (rfragment->data == NULL) {
				rte_free(rfragment);
				return -1;
			}
			memset(rfragment->data, 0, payloadlen+1);

			rte_memcpy(rfragment->data, payload, payloadlen);
			rfragment->length = payloadlen;

			printf("tcp : %s\n", rfragment->data);
		}
		rte_ring_mp_enqueue(stream->rcvbuf, rfragment);
#else

		ng_tcp_enqueue_recvbuffer(stream, tcphdr, tcplen);

#endif


#if 0
		// ack pkt
		struct ng_tcp_fragment *ackfrag = rte_malloc("ng_tcp_fragment", sizeof(struct ng_tcp_fragment), 0);
		if (ackfrag == NULL) return -1;
		memset(ackfrag, 0, sizeof(struct ng_tcp_fragment));

		ackfrag->dport = tcphdr->src_port;
		ackfrag->sport = tcphdr->dst_port;

		// remote
		
		printf("ng_tcp_handle_established: %d, %d\n", stream->rcv_nxt, ntohs(tcphdr->sent_seq));
		
		
		stream->rcv_nxt = stream->rcv_nxt + payloadlen;
		// local 
		stream->snd_nxt = ntohl(tcphdr->recv_ack);
		//ackfrag->

		ackfrag->acknum = stream->rcv_nxt;
		ackfrag->seqnum = stream->snd_nxt;

		ackfrag->tcp_flags = RTE_TCP_ACK_FLAG;
		ackfrag->windows = TCP_INITIAL_WINDOW;
		ackfrag->hdrlen_off = 0x50;
		ackfrag->data = NULL;
		ackfrag->length = 0;
		
		rte_ring_mp_enqueue(stream->sndbuf, ackfrag);

#else

		uint8_t hdrlen = tcphdr->data_off >> 4;
		int payloadlen = tcplen - hdrlen * 4;
		
		stream->rcv_nxt = stream->rcv_nxt + payloadlen;
		stream->snd_nxt = ntohl(tcphdr->recv_ack);
		
		ng_tcp_send_ackpkt(stream, tcphdr);
		
#endif
		// echo pkt
#if 0
		struct ng_tcp_fragment *echofrag = rte_malloc("ng_tcp_fragment", sizeof(struct ng_tcp_fragment), 0);
		if (echofrag == NULL) return -1;
		memset(echofrag, 0, sizeof(struct ng_tcp_fragment));

		echofrag->dport = tcphdr->src_port;
		echofrag->sport = tcphdr->dst_port;

		echofrag->acknum = stream->rcv_nxt;
		echofrag->seqnum = stream->snd_nxt;

		echofrag->tcp_flags = RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG;
		echofrag->windows = TCP_INITIAL_WINDOW;
		echofrag->hdrlen_off = 0x50;

		uint8_t *payload = (uint8_t*)tcphdr + hdrlen * 4;

		echofrag->data = rte_malloc("unsigned char *", payloadlen, 0);
		if (echofrag->data == NULL) {
			rte_free(echofrag);
			return -1;
		}
		memset(echofrag->data, 0, payloadlen);

		rte_memcpy(echofrag->data, payload, payloadlen);
		echofrag->length = payloadlen;

		rte_ring_mp_enqueue(stream->sndbuf, echofrag);
#endif

	}
	if (tcphdr->tcp_flags & RTE_TCP_ACK_FLAG) {

	}
	if (tcphdr->tcp_flags & RTE_TCP_FIN_FLAG) {

		stream->status = NG_TCP_STATUS_CLOSE_WAIT;

#if 0

		struct ng_tcp_fragment *rfragment = rte_malloc("ng_tcp_fragment", sizeof(struct ng_tcp_fragment), 0);
		if (rfragment == NULL) return -1;
		memset(rfragment, 0, sizeof(struct ng_tcp_fragment));

		rfragment->dport = ntohs(tcphdr->dst_port);
		rfragment->sport = ntohs(tcphdr->src_port);

		uint8_t hdrlen = tcphdr->data_off >> 4;
		int payloadlen = tcplen - hdrlen * 4;

		rfragment->length = 0;
		rfragment->data = NULL;
		
		rte_ring_mp_enqueue(stream->rcvbuf, rfragment);
		
#else

		ng_tcp_enqueue_recvbuffer(stream, tcphdr, tcphdr->data_off >> 4);

#endif
		// send ack ptk
		stream->rcv_nxt = stream->rcv_nxt + 1;
		stream->snd_nxt = ntohl(tcphdr->recv_ack);
		
		ng_tcp_send_ackpkt(stream, tcphdr);

	}

	return 0;
}

static int ng_tcp_handle_close_wait(struct ng_tcp_stream *stream, struct rte_tcp_hdr *tcphdr) {

	if (tcphdr->tcp_flags & RTE_TCP_FIN_FLAG) { //

		if (stream->status == NG_TCP_STATUS_CLOSE_WAIT) {

			

		}

	}

	
	return 0;

}

static int ng_tcp_handle_last_ack(struct ng_tcp_stream *stream, struct rte_tcp_hdr *tcphdr) {

	if (tcphdr->tcp_flags & RTE_TCP_ACK_FLAG) {

		if (stream->status == NG_TCP_STATUS_LAST_ACK) {

			stream->status = NG_TCP_STATUS_CLOSED;

			printf("ng_tcp_handle_last_ack\n");
			struct ng_tcp_table *table = tcpInstance();
			LL_REMOVE(stream, table->tcb_set);

			rte_ring_free(stream->sndbuf);
			rte_ring_free(stream->rcvbuf);

			rte_free(stream);

		}

	}

	return 0;
}

static int ng_tcp_process(struct rte_mbuf *tcpmbuf) {

	struct rte_ipv4_hdr *iphdr =  rte_pktmbuf_mtod_offset(tcpmbuf, struct rte_ipv4_hdr *, 
				sizeof(struct rte_ether_hdr));
	struct rte_tcp_hdr *tcphdr = (struct rte_tcp_hdr *)(iphdr + 1);	

	uint16_t tcpcksum = tcphdr->cksum;
	tcphdr->cksum = 0;
	uint16_t cksum = rte_ipv4_udptcp_cksum(iphdr, tcphdr);
	printf("ng_tcp_process ---> src: %s:%d ", inet_ntoa(*(struct in_addr *)&iphdr->src_addr), ntohs(tcphdr->src_port));
#if 1 //
	if (cksum != tcpcksum) {
		printf("cksum: %x, tcp cksum: %x\n", cksum, tcpcksum);
        rte_pktmbuf_free(tcpmbuf);
		return -1;
	}
#endif

	struct ng_tcp_stream *stream = ng_tcp_stream_search(iphdr->src_addr, iphdr->dst_addr, 
		tcphdr->src_port, tcphdr->dst_port);
	if (stream == NULL) {
		rte_pktmbuf_free(tcpmbuf);
		return -1;
	}

	switch (stream->status) {

		case NG_TCP_STATUS_CLOSED: //client 
			break;
			
		case NG_TCP_STATUS_LISTEN: // server
			ng_tcp_handle_listen(stream, tcphdr, iphdr);
			break;

		case NG_TCP_STATUS_SYN_RCVD: // server  
			ng_tcp_handle_syn_rcvd(stream, tcphdr);
			break;

		case NG_TCP_STATUS_SYN_SENT: // client
			break;

		case NG_TCP_STATUS_ESTABLISHED: {// server | client
			int tcplen = ntohs(iphdr->total_length) - sizeof(struct rte_ipv4_hdr);
			
			ng_tcp_handle_established(stream, tcphdr, tcplen);		
			break;
        }
		case NG_TCP_STATUS_FIN_WAIT_1: //  ~client
			break;
			
		case NG_TCP_STATUS_FIN_WAIT_2: // ~client
			break;
			
		case NG_TCP_STATUS_CLOSING: // ~client
			break;
			
		case NG_TCP_STATUS_TIME_WAIT: // ~client
			break;

		case NG_TCP_STATUS_CLOSE_WAIT: // ~server
			ng_tcp_handle_close_wait(stream, tcphdr);
			break;
			
		case NG_TCP_STATUS_LAST_ACK:  // ~server
			ng_tcp_handle_last_ack(stream, tcphdr);
			break;

	}
    rte_pktmbuf_free(tcpmbuf);
	return 0;
}


static int ng_encode_tcp_apppkt(uint8_t *msg, uint32_t sip, uint32_t dip,
	uint8_t *srcmac, uint8_t *dstmac, struct ng_tcp_fragment *fragment) {

	// encode 
	const unsigned total_len = fragment->length + sizeof(struct rte_ether_hdr) +
							sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr) + 
							fragment->optlen * sizeof(uint32_t);

	// 1 ethhdr
	struct rte_ether_hdr *eth = (struct rte_ether_hdr *)msg;
	rte_memcpy(eth->s_addr.addr_bytes, srcmac, RTE_ETHER_ADDR_LEN);
	rte_memcpy(eth->d_addr.addr_bytes, dstmac, RTE_ETHER_ADDR_LEN);
	eth->ether_type = htons(RTE_ETHER_TYPE_IPV4);
	

	// 2 iphdr 
	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(msg + sizeof(struct rte_ether_hdr));
	ip->version_ihl = 0x45;
	ip->type_of_service = 0;
	ip->total_length = htons(total_len - sizeof(struct rte_ether_hdr));
	ip->packet_id = 0;
	ip->fragment_offset = 0;
	ip->time_to_live = 64; // ttl = 64
	ip->next_proto_id = IPPROTO_TCP;
	ip->src_addr = sip;
	ip->dst_addr = dip;
	
	ip->hdr_checksum = 0;
	ip->hdr_checksum = rte_ipv4_cksum(ip);

	// 3 udphdr 

	struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(msg + sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
	tcp->src_port = fragment->sport;
	tcp->dst_port = fragment->dport;
	tcp->sent_seq = htonl(fragment->seqnum);
	tcp->recv_ack = htonl(fragment->acknum);

	tcp->data_off = fragment->hdrlen_off;
	tcp->rx_win = htons(fragment->windows);
	tcp->tcp_urp = htons(fragment->tcp_urp);
	tcp->tcp_flags = fragment->tcp_flags;

	if (fragment->data != NULL) {
		uint8_t *payload = (uint8_t*)(tcp+1) + fragment->optlen * sizeof(uint32_t);
		rte_memcpy(payload, fragment->data, fragment->length);
	}

	tcp->cksum = 0;
	tcp->cksum = rte_ipv4_udptcp_cksum(ip, tcp);

	return 0;
}


static struct rte_mbuf * ng_tcp_pkt(struct rte_mempool *mbuf_pool, uint32_t sip, uint32_t dip,
	uint8_t *srcmac, uint8_t *dstmac, struct ng_tcp_fragment *fragment) {

	// mempool --> mbuf

	const unsigned total_len = fragment->length + sizeof(struct rte_ether_hdr) +
							sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr) + 
							fragment->optlen * sizeof(uint32_t);

	struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
	if (!mbuf) {
		rte_exit(EXIT_FAILURE, "rte_pktmbuf_alloc\n");
	}
	mbuf->pkt_len = total_len;
	mbuf->data_len = total_len;

	uint8_t *pktdata = rte_pktmbuf_mtod(mbuf, uint8_t*);

	ng_encode_tcp_apppkt(pktdata, sip, dip, srcmac, dstmac, fragment);

	return mbuf;

}


// struct localhost , struct tcp_stream

static int ng_tcp_out(struct rte_mempool *mbuf_pool) {

	struct ng_tcp_table *table = tcpInstance();
	
	struct ng_tcp_stream *stream;
	for (stream = table->tcb_set;stream != NULL;stream = stream->next) {

        if (stream->sndbuf == NULL) continue; // listener

		struct ng_tcp_fragment *fragment = NULL;
		int nb_snd = rte_ring_mc_dequeue(stream->sndbuf, (void**)&fragment);
		if (nb_snd < 0) continue;

		uint8_t *dstmac = ng_get_dst_macaddr(stream->sip); // 
		if (dstmac == NULL) {
            printf("ng_tcp_out ---> dstmac is NULL, send arp request\n");
			struct rte_mbuf *arpbuf = ng_send_arp(mbuf_pool, RTE_ARP_OP_REQUEST, gDefaultArpMac, 
				stream->dip, stream->sip);

			struct inout_ring *ring = ring_instance();
			rte_ring_mp_enqueue_burst(ring->out, (void **)&arpbuf, 1, NULL);

			rte_ring_mp_enqueue(stream->sndbuf, fragment);

		} else {
            printf("ng_tcp_out ---> send tcp packet to dstmac\n");
			struct rte_mbuf *tcpbuf = ng_tcp_pkt(mbuf_pool, stream->dip, stream->sip, stream->localmac, dstmac, fragment);

			struct inout_ring *ring = ring_instance();
			rte_ring_mp_enqueue_burst(ring->out, (void **)&tcpbuf, 1, NULL);

			if (fragment->data != NULL)
				rte_free(fragment->data);
			
			rte_free(fragment);
		}

	}

	return 0;
}

#define BUFFER_SIZE	1024
// hook
static int tcp_server_entry(__attribute__((unused))  void *arg)  {

	int listenfd = nsocket(AF_INET, SOCK_STREAM, 0);
	if (listenfd == -1) {
		return -1;
	}

	struct sockaddr_in servaddr;
	memset(&servaddr, 0, sizeof(struct sockaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port = htons(8889);
	nbind(listenfd, (struct sockaddr*)&servaddr, sizeof(servaddr));

	nlisten(listenfd, 10);

	while (1) {
		
		struct sockaddr_in client;
		socklen_t len = sizeof(client);
		int connfd = naccept(listenfd, (struct sockaddr*)&client, &len);

		char buff[BUFFER_SIZE] = {0};
		while (1) {

			int n = nrecv(connfd, buff, BUFFER_SIZE, 0); //block
			if (n > 0) {
				printf("recv: %s\n", buff);
				nsend(connfd, buff, n, 0);

			} else if (n == 0) {

				nclose(connfd);
				break;
			} else { //nonblock

			}
		}

	}
	nclose(listenfd);
	

}

#endif




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

            if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                struct rte_ipv4_hdr *ipv4_hdr = rte_pktmbuf_mtod_offset(bufs[i], struct rte_ipv4_hdr *,
					sizeof(struct rte_ether_hdr));
#if 1
					ng_arp_entry_insert(ipv4_hdr->src_addr, eth_hdr->s_addr.addr_bytes);
#endif

				if (ipv4_hdr->next_proto_id == IPPROTO_UDP) {
					udp_process(bufs[i]);
				} else if (ipv4_hdr->next_proto_id == IPPROTO_TCP) {
					ng_tcp_process(bufs[i]);
				} else {
					rte_kni_tx_burst(global_kni, &bufs[i], 1);
					printf("tcp/udp --> rte_kni_handle_request\n");
				}
			} else {
				rte_kni_tx_burst(global_kni, &bufs[i], 1);
				printf("ip --> rte_kni_handle_request\n");
			}

        }
		rte_kni_handle_request(global_kni);//回调ng_config_network_if
        udp_out(mbuf_pool);
		ng_tcp_out(mbuf_pool);

    }
    return 0;
}

#if ENABLE_UDP_APP

#define UDP_APP_BUFFER_SIZE 128

#define DEFAULT_FD_NUM 3

static unsigned char fd_table[MAX_FD_COUNT] = {0};

static int get_fd_frombitmap(void) {

	int fd = DEFAULT_FD_NUM;
	for ( ;fd < MAX_FD_COUNT;fd ++) {
		if ((fd_table[fd/8] & (0x1 << (fd % 8))) == 0) {
			fd_table[fd/8] |= (0x1 << (fd % 8));
			return fd;
		}
	}

	return -1;
	
}

static int set_fd_frombitmap(int fd) {

	if (fd >= MAX_FD_COUNT) return -1;

	fd_table[fd/8] &= ~(0x1 << (fd % 8));

	return 0;
}

static struct ng_tcp_stream *get_accept_tcb(uint16_t dport) {

	struct ng_tcp_stream *apt;
	struct ng_tcp_table *table = tcpInstance();
	for (apt = table->tcb_set;apt != NULL;apt = apt->next) {
		if (dport == apt->dport && apt->fd == -1) {
			return apt;
		}
	}

	return NULL;
}

void* get_hostinfo_fromfd(int socket_fd) {
    
    struct localhost *host;

    for (host = localhost_list; host != NULL; host = host->next) {
        if (host->fd == socket_fd) {
            return host;
        }
    }

    struct ng_tcp_stream *stream;

	struct ng_tcp_table *table = tcpInstance();
	for (stream = table->tcb_set;stream != NULL;stream = stream->next) {
		if (socket_fd == stream->fd) {
			return stream;
		}
	}

    return NULL;
}

struct localhost *get_hostinfo_fromip_port(uint32_t ip, uint16_t port, uint8_t protocol) {
    struct localhost *host;
    for (host = localhost_list; host != NULL; host = host->next) {
        printf("get_hostinfo_fromip_port ---> ip: %s: port %d protocol: %d\n", inet_ntoa(*(struct in_addr *)&host->local_ip), ntohs(host->local_port), host->protocol);
        if (host->local_ip == ip && host->local_port == port && host->protocol == protocol) {
            return host;
        }
    }
    return NULL;
}

static int nsocket(__attribute__((unused)) int domain, int type, __attribute__((unused))  int protocol) {

	int fd = get_fd_frombitmap(); //

	if (type == SOCK_DGRAM) {

		struct localhost *host = rte_malloc("localhost", sizeof(struct localhost), 0);
		if (host == NULL) {
			return -1;
		}
		memset(host, 0, sizeof(struct localhost));

		host->fd = fd;
		
		host->protocol = IPPROTO_UDP;

		char rcvname[32], sndname[32];
		snprintf(rcvname, sizeof(rcvname), "recv buffer %d", fd);
		snprintf(sndname, sizeof(sndname), "send buffer %d", fd);

		host->rcvbuf = rte_ring_create(rcvname, RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (host->rcvbuf == NULL) {

			rte_free(host);
			return -1;
		}

	
		host->sndbuf = rte_ring_create(sndname, RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (host->sndbuf == NULL) {

			rte_ring_free(host->rcvbuf);

			rte_free(host);
			return -1;
		}

		pthread_cond_t blank_cond = PTHREAD_COND_INITIALIZER;
		rte_memcpy(&host->cond, &blank_cond, sizeof(pthread_cond_t));

		pthread_mutex_t blank_mutex = PTHREAD_MUTEX_INITIALIZER;
		rte_memcpy(&host->mutex, &blank_mutex, sizeof(pthread_mutex_t));

		LL_ADD(host, localhost_list);
		
	} else if (type == SOCK_STREAM) {


		struct ng_tcp_stream *stream = rte_malloc("ng_tcp_stream", sizeof(struct ng_tcp_stream), 0);
		if (stream == NULL) {
			return -1;
		}
		memset(stream, 0, sizeof(struct ng_tcp_stream));

		stream->fd = fd;
		stream->protocol = IPPROTO_TCP;
		stream->next = stream->prev = NULL;

		char rcvname[32], sndname[32];
		snprintf(rcvname, sizeof(rcvname), "tcp recv buffer %d", fd);
		snprintf(sndname, sizeof(sndname), "tcp send buffer %d", fd);

		stream->rcvbuf = rte_ring_create(rcvname, RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (stream->rcvbuf == NULL) {

			rte_free(stream);
			return -1;
		}

	
		stream->sndbuf = rte_ring_create(sndname, RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (stream->sndbuf == NULL) {

			rte_ring_free(stream->rcvbuf);

			rte_free(stream);
			return -1;
		}

		pthread_cond_t blank_cond = PTHREAD_COND_INITIALIZER;
		rte_memcpy(&stream->cond, &blank_cond, sizeof(pthread_cond_t));

		pthread_mutex_t blank_mutex = PTHREAD_MUTEX_INITIALIZER;
		rte_memcpy(&stream->mutex, &blank_mutex, sizeof(pthread_mutex_t));

		struct ng_tcp_table *table = tcpInstance();
		LL_ADD(stream, table->tcb_set);
		// get_stream_from_fd();
	}

	return fd;
}

static int nbind(int sockfd, const struct sockaddr *addr,
                __attribute__((unused))socklen_t addrlen) {
    struct localhost *host = get_hostinfo_fromfd(sockfd);
    if (host == NULL) {
        printf("Invalid nsocket file descriptor\n");
        return -1;
    }
    if ( host->protocol == IPPROTO_UDP) {
        const struct sockaddr_in *local_addr = (const struct sockaddr_in *)addr;
        host->local_port = local_addr->sin_port;
        host->local_ip = local_addr->sin_addr.s_addr;
        rte_memcpy(host->local_mac, gSrcMac, RTE_ETHER_ADDR_LEN);

    } else if (host->protocol == IPPROTO_TCP) {
        struct ng_tcp_stream *stream = (struct ng_tcp_stream *)host;
        const struct sockaddr_in *laddr = (const struct sockaddr_in *)addr;
		stream->dport = laddr->sin_port;
		rte_memcpy(&stream->dip, &laddr->sin_addr.s_addr, sizeof(uint32_t));
		rte_memcpy(stream->localmac, gSrcMac, RTE_ETHER_ADDR_LEN);

		stream->status = NG_TCP_STATUS_CLOSED;
    }

    return 0;
}

static int nlisten(int sockfd, __attribute__((unused)) int backlog) { //

	void *hostinfo =  get_hostinfo_fromfd(sockfd);
	if (hostinfo == NULL) return -1;

	
	struct ng_tcp_stream *stream = (struct ng_tcp_stream *)hostinfo;
	if (stream->protocol == IPPROTO_TCP) {
		stream->status = NG_TCP_STATUS_LISTEN;
	}

	return 0;
}

static int naccept(int sockfd, struct sockaddr *addr, __attribute__((unused)) socklen_t *addrlen) {

	void *hostinfo =  get_hostinfo_fromfd(sockfd);
	if (hostinfo == NULL) return -1;

	struct ng_tcp_stream *stream = (struct ng_tcp_stream *)hostinfo;
	if (stream->protocol == IPPROTO_TCP) {

		struct ng_tcp_stream *apt = NULL;

		pthread_mutex_lock(&stream->mutex);
		while((apt = get_accept_tcb(stream->dport)) == NULL) {
			pthread_cond_wait(&stream->cond, &stream->mutex);
		} 
		pthread_mutex_unlock(&stream->mutex);

		apt->fd = get_fd_frombitmap();

		struct sockaddr_in *saddr = (struct sockaddr_in *)addr;
		saddr->sin_port = apt->sport;
		rte_memcpy(&saddr->sin_addr.s_addr, &apt->sip, sizeof(uint32_t));

		return apt->fd;
	}

	return -1;
}

static ssize_t nsend(int sockfd, const void *buf, size_t len,__attribute__((unused)) int flags) {

	ssize_t length = 0;

	void *hostinfo =  get_hostinfo_fromfd(sockfd);
	if (hostinfo == NULL) return -1;

	struct ng_tcp_stream *stream = (struct ng_tcp_stream *)hostinfo;
	if (stream->protocol == IPPROTO_TCP) {

		struct ng_tcp_fragment *fragment = rte_malloc("ng_tcp_fragment", sizeof(struct ng_tcp_fragment), 0);
		if (fragment == NULL) {
			return -2;
		}

		memset(fragment, 0, sizeof(struct ng_tcp_fragment));

		fragment->dport = stream->sport;
		fragment->sport = stream->dport;

		fragment->acknum = stream->rcv_nxt;
		fragment->seqnum = stream->snd_nxt;

		fragment->tcp_flags = RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG;
		fragment->windows = TCP_INITIAL_WINDOW;
		fragment->hdrlen_off = 0x50;


		fragment->data = rte_malloc("unsigned char *", len+1, 0);
		if (fragment->data == NULL) {
			rte_free(fragment);
			return -1;
		}
		memset(fragment->data, 0, len+1);

		rte_memcpy(fragment->data, buf, len);
		fragment->length = len;
		length = fragment->length;

		// int nb_snd = 0;
		rte_ring_mp_enqueue(stream->sndbuf, fragment);

	}

	
	return length;
}

// recv 32
// recv 
static ssize_t nrecv(int sockfd, void *buf, size_t len, __attribute__((unused)) int flags) {
	
	ssize_t length = 0;

	void *hostinfo =  get_hostinfo_fromfd(sockfd);
	if (hostinfo == NULL) return -1;

	struct ng_tcp_stream *stream = (struct ng_tcp_stream *)hostinfo;
	if (stream->protocol == IPPROTO_TCP) {

		struct ng_tcp_fragment *fragment = NULL;
		int nb_rcv = 0;

		printf("rte_ring_mc_dequeue before\n");
		pthread_mutex_lock(&stream->mutex);
		while ((nb_rcv = rte_ring_mc_dequeue(stream->rcvbuf, (void **)&fragment)) < 0) {
			pthread_cond_wait(&stream->cond, &stream->mutex);
		}
		pthread_mutex_unlock(&stream->mutex);
		printf("rte_ring_mc_dequeue after\n");

		if (fragment->length > (int)len) {

			rte_memcpy(buf, fragment->data, len);

			uint32_t i = 0;
			for(i = 0;i < fragment->length-len;i ++) {
				fragment->data[i] = fragment->data[len+i];
			}
			fragment->length = fragment->length-len;
			length = fragment->length;

			rte_ring_mp_enqueue(stream->rcvbuf, fragment);

		} else if (fragment->length == 0) {

			rte_free(fragment);
			return 0;
		
		} else {

			rte_memcpy(buf, fragment->data, fragment->length);
			length = fragment->length;

			rte_free(fragment->data);
			fragment->data = NULL;

			rte_free(fragment);
			
		}

	}

	return length;
}

ssize_t nrecvfrom(int sockfd, void *buf, size_t len,__attribute__((unused)) int flags,
                        struct sockaddr *src_addr,__attribute__((unused)) socklen_t *addrlen) {

    struct localhost *host = (struct localhost *)get_hostinfo_fromfd(sockfd);
    if (host == NULL) {
        printf("Invalid nsocket file descriptor\n");
        return -1;
    }
    struct offload *ol = NULL;
    unsigned char *ptr = NULL;
    struct sockaddr_in *saddr = (struct sockaddr_in *)src_addr;
    int nb = -1;
    pthread_mutex_lock(&host->mutex);
    while ( (nb = rte_ring_sc_dequeue(host->rcvbuf, (void**)&ol)) < 0) {
        pthread_cond_wait(&host->cond, &host->mutex);
    }
    pthread_mutex_unlock(&host->mutex);

    saddr->sin_port = ol->src_port;
    rte_memcpy(&saddr->sin_addr.s_addr, &ol->src_ip, sizeof(uint32_t));

    if (len < ol->data_len) {

        rte_memcpy(buf, ol->data, len);

        ptr = rte_malloc("unsigned char*", ol->data_len - len, 0);
        rte_memcpy(ptr, ol->data + len, ol->data_len - len);

        ol->data_len -= len;
        rte_free(ol->data);
        ol->data = ptr;

        rte_ring_sp_enqueue(host->rcvbuf, (void*)ol);
        return len;
    } else {
        rte_memcpy(buf, ol->data, ol->data_len);
        rte_free(ol->data);
        rte_free(ol);
        return ol->data_len;
    }
}

ssize_t nsendto(int sockfd, const void *buf, size_t len, __attribute__((unused)) int flags,
                      const struct sockaddr *dest_addr, __attribute__((unused)) socklen_t addrlen)
{

    struct localhost *host = (struct localhost *)get_hostinfo_fromfd(sockfd);
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

    struct in_addr addr;
	addr.s_addr = ol->dst_ip;
	printf("nsendto ---> src: %s:%d \n", inet_ntoa(addr), ntohs(ol->dst_port));

    ol->data = rte_malloc("unsigned char*",len, 0);
    if (ol->data == NULL) {
        rte_free(ol);
        return -1;
    }
    rte_memcpy(ol->data, buf, len);

    rte_ring_sp_enqueue(host->sndbuf, (void*)ol);

    return len;
}

static int nclose(int fd) {

	
	void *hostinfo =  get_hostinfo_fromfd(fd);
	if (hostinfo == NULL) return -1;

	struct localhost *host = (struct localhost*)hostinfo;
	if (host->protocol == IPPROTO_UDP) {

		LL_REMOVE(host, localhost_list);

		if (host->rcvbuf) {
			rte_ring_free(host->rcvbuf);
		}
		if (host->sndbuf) {
			rte_ring_free(host->sndbuf);
		}

		rte_free(host);

		set_fd_frombitmap(fd);
		
	} else if (host->protocol == IPPROTO_TCP) { 

		struct ng_tcp_stream *stream = (struct ng_tcp_stream*)hostinfo;

		if (stream->status != NG_TCP_STATUS_LISTEN) {
			
			struct ng_tcp_fragment *fragment = rte_malloc("ng_tcp_fragment", sizeof(struct ng_tcp_fragment), 0);
			if (fragment == NULL) return -1;

			printf("nclose --> enter last ack\n");
			fragment->data = NULL;
			fragment->length = 0;
			fragment->sport = stream->dport;
			fragment->dport = stream->sport;

			fragment->seqnum = stream->snd_nxt;
			fragment->acknum = stream->rcv_nxt;

			fragment->tcp_flags = RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG;
			fragment->windows = TCP_INITIAL_WINDOW;
			fragment->hdrlen_off = 0x50;

			rte_ring_mp_enqueue(stream->sndbuf, fragment);
			stream->status = NG_TCP_STATUS_LAST_ACK;

			
			set_fd_frombitmap(fd);

		} else { // nsocket

			struct ng_tcp_table *table = tcpInstance();
			LL_REMOVE(stream, table->tcb_set);	

			rte_free(stream);

		}
	}

	return 0;
}

int udp_server_entry(__attribute__((unused)) void *arg) {
    int connfd = nsocket(AF_INET, SOCK_DGRAM, 0);
    if (connfd < 0) {
        printf("nsocket error\n");
        return -1;
    }

    struct sockaddr_in localaddr, clientaddr;
    memset(&localaddr, 0, sizeof(struct sockaddr_in));

    localaddr.sin_family = AF_INET;
    //localaddr.sin_addr.s_addr = htonl(INADDR_ANY);//0.0.0.0
    localaddr.sin_addr.s_addr = inet_addr("192.168.196.132");//0.0.0.0
    localaddr.sin_port = htons(8889);

    nbind(connfd, (struct sockaddr *)&localaddr, sizeof(localaddr));
    socklen_t addrlen = sizeof(clientaddr);

    char buffer[UDP_APP_BUFFER_SIZE] = {0};
    while (1) {

        if (nrecvfrom(connfd, buffer, UDP_APP_BUFFER_SIZE, 0,
            (struct sockaddr *)&clientaddr, &addrlen) < 0) {
            //printf("nrecvfrom error\n");
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

#if ENABLE_KNI_APP

// ifconfig vEth0 up/down

// config_network_if
// rte_kni_handle_request
static int ng_config_network_if(uint16_t port_id, uint8_t if_up) { //开关函数

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

static struct rte_kni *ng_alloc_kni(struct rte_mempool *mbuf_pool) {

	struct rte_kni *kni_hanlder = NULL;
	
	struct rte_kni_conf conf;//kni 配置参数
	memset(&conf, 0, sizeof(conf));

	snprintf(conf.name, RTE_KNI_NAMESIZE, "vEth%u", gdpdkportid);
	conf.group_id = gdpdkportid;
	conf.mbuf_size = MAX_PACKET_SIZE;
	rte_eth_macaddr_get(gdpdkportid, (struct rte_ether_addr *)conf.mac_addr);
	rte_eth_dev_get_mtu(gdpdkportid, &conf.mtu);

	print_ether_addr("ng_alloc_kni: ", (struct rte_ether_addr *)conf.mac_addr);

	/*
	struct rte_eth_dev_info dev_info;
	memset(&dev_info, 0, sizeof(dev_info));
	rte_eth_dev_info_get(gDpdkPortId, &dev_info);
	*/


	struct rte_kni_ops ops;//kni 操作
	memset(&ops, 0, sizeof(ops));

	ops.port_id = gdpdkportid;
	ops.config_network_if = ng_config_network_if;
	

	kni_hanlder = rte_kni_alloc(mbuf_pool, &conf, &ops);	
	if (!kni_hanlder) {
		rte_exit(EXIT_FAILURE, "Failed to create kni for port : %d\n", gdpdkportid);
	}
	
	return kni_hanlder;
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
#if ENABLE_KNI_APP

	if (-1 == rte_kni_init(gdpdkportid)) {//入参无所谓，没有用
		rte_exit(EXIT_FAILURE, "kni init failed\n");
	}
	ng_init_port(mbuf_pool);
	// kni_alloc
	global_kni = ng_alloc_kni(mbuf_pool);

#else	
	ng_init_port(mbuf_pool);

#endif

    rte_eth_macaddr_get(gdpdkportid, (struct rte_ether_addr*)&gSrcMac);

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

#if ENABLE_UDP_APP
    lcore_id = rte_get_next_lcore(lcore_id, 1, 0);
    rte_eal_remote_launch(udp_server_entry, NULL, lcore_id);

#endif

#if ENABLE_TCP_APP
    lcore_id = rte_get_next_lcore(lcore_id, 1, 0);
    rte_eal_remote_launch(tcp_server_entry, NULL, lcore_id);

#endif

#if ENABLE_MULTHREAD
    lcore_id = rte_get_next_lcore(lcore_id, 1, 0);
    rte_eal_remote_launch(pkt_process, mbuf_pool, lcore_id);
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
            uint16_t nb_sent = rte_eth_tx_burst(gdpdkportid, 0, tx, nb_tx);
            if (nb_sent < nb_tx) {
                unsigned i;
                for (i = nb_sent; i < nb_tx; i++) {
                    rte_pktmbuf_free(tx[i]);
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
}