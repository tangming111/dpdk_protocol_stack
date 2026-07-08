#include <stdio.h>
#include <arpa/inet.h>

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

#define ARP_ENTRY_STATUS_DYNAMIC 0
#define ARP_ENTRY_STATUS_STATIC 1

#define NUM_MBUFS (4096 - 1)
#define BURST_SIZE 128


#define MAKE_IPV4_ADDR(a, b, c, d) \
    ((uint32_t)(((a) & 0xff)) | (((b) & 0xff) << 8) | (((c) & 0xff) << 16) | (((d) & 0xff) << 24))
static const uint32_t glocalIp = MAKE_IPV4_ADDR(192, 168, 196, 132);


#define TIMER_RESOLUTION_CYCLES 2000000000ULL /* around 10ms  at 2 Ghz   10ms * 100 */

#if ENABLE_SEND

static uint32_t gSrcIp;
static uint32_t gDstIp;

static uint8_t gDstMac[RTE_ETHER_ADDR_LEN];
static uint8_t gSrcMac[RTE_ETHER_ADDR_LEN];

static uint16_t gSrcPort;
static uint16_t gDstPort;

#endif

#ifdef ENABLE_ARP_REPLY

    uint8_t gDefaultArpMac[RTE_ETHER_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#endif



int gdpdkportid = 0;
static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
    },
};

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
}

static struct rte_mbuf *ng_send_udp(struct rte_mempool *mempool, uint8_t *data, uint16_t length) {
    
    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + length;//14 + 20 + 8
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
    if (mbuf == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf\n");
    }
    mbuf->data_len = total_len;
    mbuf->pkt_len = total_len;

    uint8_t *pktdata = rte_pktmbuf_mtod(mbuf, uint8_t *);
    ng_encode_udp_pkt(pktdata, data, total_len);

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
        rte_eth_tx_burst(gdpdkportid, 0, &arp_mbuf, 1);
        rte_pktmbuf_free(arp_mbuf);
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

    rte_eth_macaddr_get(gdpdkportid, &gSrcMac);
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
    while(1) {
        struct rte_mbuf *bufs[BURST_SIZE];
        unsigned num_recv = rte_eth_rx_burst(gdpdkportid, 0, bufs, BURST_SIZE);
        if (num_recv > BURST_SIZE) {
            rte_exit(EXIT_FAILURE, "Error receiving from ethdev\n");
        }

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
                        rte_eth_tx_burst(gdpdkportid, 0, &arp_mbuf, 1);
                        rte_pktmbuf_free(arp_mbuf);
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
                struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)((unsigned char *)ipv4_hdr + sizeof(struct rte_ipv4_hdr));
#if ENABLE_SEND
                rte_memcpy(gDstMac, eth_hdr->s_addr.addr_bytes, RTE_ETHER_ADDR_LEN);

                rte_memcpy(&gDstIp, &ipv4_hdr->src_addr, sizeof(uint32_t));
                rte_memcpy(&gSrcIp, &ipv4_hdr->dst_addr, sizeof(uint32_t));

                rte_memcpy(&gDstPort, &udp_hdr->src_port, sizeof(uint16_t));
                rte_memcpy(&gSrcPort, &udp_hdr->dst_port, sizeof(uint16_t));
#endif
                          
                uint16_t legth = ntohs(udp_hdr->dgram_len);//两个字节以上都转
                *((char *)udp_hdr + legth) = '\0';

                struct in_addr addr;
                addr.s_addr = ipv4_hdr->src_addr;
                printf("Received packet from %s:%d", inet_ntoa(addr), ntohs(udp_hdr->src_port));

                addr.s_addr = ipv4_hdr->dst_addr;
                printf(" to %s:%d, length:%d ---> %s\n", inet_ntoa(addr), ntohs(udp_hdr->dst_port), legth, (char*)(udp_hdr + 1));
#if ENABLE_SEND
                struct rte_mbuf *tx_mbuf = ng_send_udp(mbuf_pool, (uint8_t *)(udp_hdr + 1), legth);
                rte_eth_tx_burst(gdpdkportid, 0, &tx_mbuf, 1);
                rte_pktmbuf_free(tx_mbuf);
#endif
                rte_pktmbuf_free(bufs[i]);
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
                    rte_eth_tx_burst(gdpdkportid, 0, &icmp_mbuf, 1);
                    rte_pktmbuf_free(icmp_mbuf);
                    rte_pktmbuf_free(bufs[i]);
                }
            }
#endif

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