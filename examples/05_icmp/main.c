#include <stdio.h>
#include <arpa/inet.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_mbuf.h>

#define ENABLE_SEND 1
#define ENABLE_ARP 1
#define ENABLE_ICMP 1

#define NUM_MBUFS (4096 - 1)
#define BURST_SIZE 128


#define MAKE_IPV4_ADDR(a, b, c, d) \
    ((uint32_t)(((a) & 0xff)) | (((b) & 0xff) << 8) | (((c) & 0xff) << 16) | (((d) & 0xff) << 24))
static const uint32_t glocalIp = MAKE_IPV4_ADDR(192, 168, 196, 132);

#if ENABLE_SEND

static uint32_t gSrcIp;
static uint32_t gDstIp;

static uint8_t gDstMac[RTE_ETHER_ADDR_LEN];
static uint8_t gSrcMac[RTE_ETHER_ADDR_LEN];

static uint16_t gSrcPort;
static uint16_t gDstPort;

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

static int ng_encode_arp_pkt(uint8_t *msg, uint8_t *dst_mac, uint32_t src_ip, uint32_t dst_ip) {
    //ethhdr
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth_hdr->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth_hdr->s_addr.addr_bytes, gSrcMac, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = htons(RTE_ETHER_TYPE_ARP);

    //arphdr
    struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);
    arp_hdr->arp_hardware = htons(RTE_ARP_HRD_ETHER);
    arp_hdr->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);
    arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;//硬件地址长度 即MAC地址长度
    arp_hdr->arp_plen = sizeof(uint32_t);//协议地址长度 即IP地址长度
    arp_hdr->arp_opcode = htons(RTE_ARP_OP_REPLY);//arp应答 2
    rte_memcpy(arp_hdr->arp_data.arp_sha.addr_bytes, gSrcMac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(arp_hdr->arp_data.arp_tha.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    arp_hdr->arp_data.arp_sip = dst_ip;
    arp_hdr->arp_data.arp_tip = src_ip;

    return 0;
}


static struct rte_mbuf *ng_send_arp(struct rte_mempool *mempool, uint8_t *dst_mac, uint32_t src_ip, uint32_t dst_ip) {
    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mempool);
    if (mbuf == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mbuf\n");
    }
    mbuf->data_len = total_len;
    mbuf->pkt_len = total_len;
    uint8_t *pktdata = rte_pktmbuf_mtod(mbuf, uint8_t *);
    ng_encode_arp_pkt(pktdata, dst_mac, src_ip, dst_ip);

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
                    struct rte_mbuf *arp_mbuf = ng_send_arp(mbuf_pool, arp_hdr->arp_data.arp_sha.addr_bytes, arp_hdr->arp_data.arp_sip, arp_hdr->arp_data.arp_tip);
                    rte_eth_tx_burst(gdpdkportid, 0, &arp_mbuf, 1);
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
    }
}