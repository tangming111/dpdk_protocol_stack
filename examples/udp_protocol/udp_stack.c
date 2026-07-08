

#include <stdio.h>
#include <errno.h>
#include <getopt.h>

#include <netinet/in.h>
#include <signal.h>
#include <arpa/inet.h>


#include <rte_ethdev.h>
#include <rte_string_fns.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_kni.h>
#include <rte_ether.h>

#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_udp.h>



#define MEMPOOL_CACHE_SZ		256
#define NB_MBUF                 8192
#define KNI_MAX_KTHREAD 		32
#define MAX_PACKET_SZ           2048
#define RING_SIZE               (1024 * 8)


#define NB_RXD                  1024
#define NB_TXD                  1024
#define PKT_BURST_SZ            32


#define KNI_ENET_HEADER_SIZE    14
#define KNI_ENET_FCS_SIZE       4

#define ETHER_TYPE_IPv4			0x0800
#define PROTO_UDP	17

#define CMDLINE_OPT_CONFIG  "config"



#define RTE_CPU_TO_BE_16(cpu_16_v) \
    (uint16_t) ((((cpu_16_v) & 0xFF) << 8) | ((cpu_16_v) >> 8))
	


struct kni_port_params {
	uint16_t port_id;
	unsigned lcore_rx; /* lcore ID for RX */
	unsigned lcore_tx; /* lcore ID for TX */
	unsigned lcore_work; /* lcore ID for RX */
	unsigned lcore_send; /* lcore ID for TX */

	struct rte_ring *rx_ring;
	struct rte_ring *tx_ring;
	struct rte_ring *kni_ring;
	
	uint32_t nb_lcore_k; 	/* Number of lcores for KNI multi kernel threads */
	uint32_t nb_kni; 		/* Number of KNI devices to be created */
	unsigned lcore_k[KNI_MAX_KTHREAD]; /* lcore ID list for kthreads */
	
	struct rte_kni *kni; //[KNI_MAX_KTHREAD]; /* KNI context pointers */
} __rte_cache_aligned;


#pragma pack(1) 

#define ETH_LENGTH		6

struct eth_hdr {

	unsigned char h_dest[ETH_LENGTH]; // mac
	unsigned char h_src[ETH_LENGTH];
	unsigned short h_proto;

};

struct ip_hdr {

	unsigned char version:4,
				  hdrlen:4;

	unsigned char tos;
	unsigned short totlen;

	unsigned short id;
	unsigned short flag:3,
				   offset:13;
	
	unsigned char ttl;
	unsigned char proto; //

	unsigned short check;
	
	unsigned int sip;
	unsigned int dip;
};


struct udp_hdr {

	unsigned short sport;
	unsigned short dport;

	unsigned short length; //
	unsigned short crc;

};

// udp packet = ethhdr + iphdr + udphdr + body
struct udp_pkt {

	struct eth_hdr eh;  // sizeof(ethhdr) = 14
	struct ip_hdr ip;   // 20
	struct udp_hdr udp; // 8

	unsigned char body[0]; // sizeof(body) = 0;

};



static struct rte_mempool *pktmbuf_pool = NULL;
static struct kni_port_params *kni_port_params_array[RTE_MAX_ETHPORTS];
volatile int force_quit;
static uint32_t ports_mask = 0;
static uint64_t total_send_out;
uint32_t localIP;


static struct rte_eth_conf port_conf = {
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

//static rte_atomic32_t kni_stop = RTE_ATOMIC32_INIT(0);
static rte_atomic32_t kni_pause = RTE_ATOMIC32_INIT(0);


unsigned process_pkts(struct rte_mbuf *pkt);


static void signal_handler(int signum) {

	if (signum == SIGRTMIN || signum == SIGINT) {
		force_quit = 1;
	}

}


static void print_ethaddr(const char *name, uint8_t *addr_bytes)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];
	snprintf(buf, RTE_ETHER_ADDR_FMT_SIZE, "%02X:%02X:%02X:%02X:%02X:%02X",
		 addr_bytes[0],
		 addr_bytes[1],
		 addr_bytes[2],
		 addr_bytes[3],
		 addr_bytes[4],
		 addr_bytes[5]);
	
	printf("%s%s", name, buf);
}


static inline uint32_t get_netorder_ip(const char *ip) {
  return inet_addr(ip);
}


static void check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status\n");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf(
					"Port%d Link Up - speed %uMbps - %s\n",
						portid, link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n", portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

#if 0
static void print_ethaddr(const char *name, struct rte_ether_addr *mac_addr)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];
	rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, mac_addr);
	printf("\t%s%s\n", name, buf);
}
#endif

static int kni_change_mtu(uint16_t port_id, unsigned int new_mtu)
{
	int ret;
	uint16_t nb_rxd = NB_RXD;
	struct rte_eth_conf conf;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_rxconf rxq_conf;

	if (!rte_eth_dev_is_valid_port(port_id)) {
		printf("Invalid port id %d\n", port_id);
		return -EINVAL;
	}

	printf("Change MTU of port %d to %u\n", port_id, new_mtu);

	/* Stop specific port */
	rte_eth_dev_stop(port_id);

	memcpy(&conf, &port_conf, sizeof(conf));
	/* Set new MTU */
	if (new_mtu > RTE_ETHER_MAX_LEN)
		conf.rxmode.offloads |= DEV_RX_OFFLOAD_JUMBO_FRAME;
	else
		conf.rxmode.offloads &= ~DEV_RX_OFFLOAD_JUMBO_FRAME;

	/* mtu + length of header + length of FCS = max pkt length */
	conf.rxmode.max_rx_pkt_len = new_mtu + KNI_ENET_HEADER_SIZE +
							KNI_ENET_FCS_SIZE;
	ret = rte_eth_dev_configure(port_id, 1, 1, &conf);
	if (ret < 0) {
		printf("Fail to reconfigure port %d\n", port_id);
		return ret;
	}

	ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, NULL);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not adjust number of descriptors "
				"for port%u (%d)\n", (unsigned int)port_id,
				ret);

	rte_eth_dev_info_get(port_id, &dev_info);
	rxq_conf = dev_info.default_rxconf;
	rxq_conf.offloads = conf.rxmode.offloads;
	ret = rte_eth_rx_queue_setup(port_id, 0, nb_rxd,
		rte_eth_dev_socket_id(port_id), &rxq_conf, pktmbuf_pool);
	if (ret < 0) {
		printf("Fail to setup Rx queue of port %d\n",
				port_id);
		return ret;
	}

	/* Restart specific port */
	ret = rte_eth_dev_start(port_id);
	if (ret < 0) {
		printf("Fail to restart port %d\n", port_id);
		return ret;
	}

	return 0;
}

static int kni_config_network_interface(uint16_t port_id, uint8_t if_up)
{
	int ret = 0;

	if (!rte_eth_dev_is_valid_port(port_id)) {
		printf("Invalid port id %d\n", port_id);
		return -EINVAL;
	}

	printf("Configure network interface of %d %s\n",
					port_id, if_up ? "up" : "down");

	rte_atomic32_inc(&kni_pause);

	if (if_up != 0) { /* Configure network interface up */
		rte_eth_dev_stop(port_id);
		ret = rte_eth_dev_start(port_id);
	} else /* Configure network interface down */
		rte_eth_dev_stop(port_id);

	rte_atomic32_dec(&kni_pause);

	if (ret < 0)
		printf("Failed to start port %d\n", port_id);

	return ret;
}

#if 1
static int kni_config_mac_address(uint16_t port_id, uint8_t mac_addr[])
{
	int ret = 0;

	if (!rte_eth_dev_is_valid_port(port_id)) {
		printf( "Invalid port id %d\n", port_id);
		return -EINVAL;
	}

	printf("Configure mac address of %d\n", port_id);
	print_ethaddr("Address:", mac_addr);

	ret = rte_eth_dev_default_mac_addr_set(port_id,
					(struct rte_ether_addr *)mac_addr);
	if (ret < 0)
		printf("Failed to config mac_addr for port %d\n",
			port_id);

	return ret;
}
#endif


static int kni_alloc(uint16_t port_id)
{
	struct rte_kni *kni;
	struct rte_kni_conf conf;
	struct kni_port_params **params = kni_port_params_array;

	if (port_id >= RTE_MAX_ETHPORTS || !params[port_id])
		return -1;
#if 1

	memset(&conf, 0, sizeof(conf));

	snprintf(conf.name, RTE_KNI_NAMESIZE, "vEth%u", port_id);
	conf.group_id = (uint16_t) port_id;
	conf.mbuf_size = MAX_PACKET_SZ;

	struct rte_kni_ops ops;
	struct rte_eth_dev_info dev_info;

	memset(&dev_info, 0, sizeof(dev_info));
	rte_eth_dev_info_get(port_id, &dev_info);
	rte_eth_macaddr_get(port_id,
				(struct rte_ether_addr *)&conf.mac_addr);
	rte_eth_dev_get_mtu(port_id, &conf.mtu);
	

	memset(&ops, 0, sizeof(ops));
	ops.port_id = port_id;
	ops.config_mac_address = kni_config_mac_address;
	ops.change_mtu = kni_change_mtu;
	ops.config_network_if = kni_config_network_interface;

	kni = rte_kni_alloc(pktmbuf_pool, &conf, &ops);
	if (!kni)
	rte_exit(EXIT_FAILURE, "Fail to create kni for "
	    "port: %d\n", port_id);
	params[port_id]->kni = kni;
	
	rte_kni_handle_request(kni);

#else

	params[port_id]->nb_kni = params[port_id]->nb_lcore_k ?
				params[port_id]->nb_lcore_k : 1;

	for (i = 0; i < params[port_id]->nb_kni; i++) {
		/* Clear conf at first */
		memset(&conf, 0, sizeof(conf));
		if (params[port_id]->nb_lcore_k) {
			snprintf(conf.name, RTE_KNI_NAMESIZE,
					"vEth%u_%u", port_id, i);
			conf.core_id = params[port_id]->lcore_k[i];
			conf.force_bind = 1;
		} else
			snprintf(conf.name, RTE_KNI_NAMESIZE,
						"vEth%u", port_id);
		conf.group_id = port_id;
		conf.mbuf_size = MAX_PACKET_SZ;
		/*
		 * The first KNI device associated to a port
		 * is the master, for multiple kernel thread
		 * environment.
		 */
		if (i == 0) {
			struct rte_kni_ops ops;
			struct rte_eth_dev_info dev_info;

			memset(&dev_info, 0, sizeof(dev_info));
			rte_eth_dev_info_get(port_id, &dev_info);

			/* Get the interface default mac address */
			rte_eth_macaddr_get(port_id,
				(struct rte_ether_addr *)&conf.mac_addr);

			rte_eth_dev_get_mtu(port_id, &conf.mtu);

			memset(&ops, 0, sizeof(ops));
			ops.port_id = port_id;
			ops.change_mtu = kni_change_mtu;
			ops.config_network_if = kni_config_network_interface;
			ops.config_mac_address = kni_config_mac_address;

			kni = rte_kni_alloc(pktmbuf_pool, &conf, &ops);
		} else
			kni = rte_kni_alloc(pktmbuf_pool, &conf, NULL);

		if (!kni)
			rte_exit(EXIT_FAILURE, "Fail to create kni for "
						"port: %d\n", port_id);
		params[port_id]->kni[i] = kni;
	}
#endif
	return 0;
}

static int
kni_free_kni(uint16_t port_id)
{
	struct kni_port_params **p = kni_port_params_array;

	if (port_id >= RTE_MAX_ETHPORTS || !p[port_id])
		return -1;

	if (rte_kni_release(p[port_id]->kni))
			printf("Fail to release kni\n");

	p[port_id]->kni = NULL;
	rte_eth_dev_stop(port_id);

	return 0;
}

static void kni_burst_free_mbufs(struct rte_mbuf **pkts, unsigned num)
{
	unsigned i;

	if (pkts == NULL)
		return;

	for (i = 0; i < num; i++) {
		rte_pktmbuf_free(pkts[i]);
		pkts[i] = NULL;
	}
}


static void init_kni(void)
{
	unsigned int num_of_kni_ports = 0, i;
	struct kni_port_params **params = kni_port_params_array;
	
	for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
		if (kni_port_params_array[i]) {
			num_of_kni_ports += (params[i]->nb_lcore_k ?
				params[i]->nb_lcore_k : 1);
		}
	}

	rte_kni_init(num_of_kni_ports);

}


static void init_port(uint16_t port)
{
	int ret;
	uint16_t nb_rxd = NB_RXD;
	uint16_t nb_txd = NB_TXD;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_rxconf rxq_conf;
	struct rte_eth_txconf txq_conf;
	struct rte_eth_conf local_port_conf = port_conf;

	/* Initialise device and RX/TX queues */
	printf("Initialising port %u ...\n", (unsigned)port);
	fflush(stdout);
	
	rte_eth_dev_info_get(port, &dev_info);
	if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
		local_port_conf.txmode.offloads |=
			DEV_TX_OFFLOAD_MBUF_FAST_FREE;
	ret = rte_eth_dev_configure(port, 1, 1, &local_port_conf);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not configure port%u (%d)\n",
		            (unsigned)port, ret);

	ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not adjust number of descriptors "
				"for port%u (%d)\n", (unsigned)port, ret);

	rxq_conf = dev_info.default_rxconf;
	rxq_conf.offloads = local_port_conf.rxmode.offloads;
	ret = rte_eth_rx_queue_setup(port, 0, nb_rxd,
		rte_eth_dev_socket_id(port), &rxq_conf, pktmbuf_pool);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not setup up RX queue for "
				"port%u (%d)\n", (unsigned)port, ret);

	txq_conf = dev_info.default_txconf;
	txq_conf.offloads = local_port_conf.txmode.offloads;
	ret = rte_eth_tx_queue_setup(port, 0, nb_txd,
		rte_eth_dev_socket_id(port), &txq_conf);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not setup up TX queue for "
				"port%u (%d)\n", (unsigned)port, ret);

	ret = rte_eth_dev_start(port);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not start port%u (%d)\n",
						(unsigned)port, ret);

	rte_eth_promiscuous_enable(port);
	
}

static void init_ring(void) {
  int i;

  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (kni_port_params_array[i]) {
      struct rte_ring *rx_ring, *tx_ring, *kni_ring;
      char name[32] = {0};

      snprintf(name, sizeof(name), "ring_rx_%u", i);
      rx_ring = rte_ring_create(name, RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
      if (rx_ring == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create RX ring, %s, %s():%d\n",
                 rte_strerror(rte_errno), __func__, __LINE__);
      kni_port_params_array[i]->rx_ring = rx_ring;

      snprintf(name, sizeof(name), "ring_tx_%u", i);
      tx_ring = rte_ring_create(name, RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
      if (tx_ring == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create TX ring, %s, %s():%d\n",
                 rte_strerror(rte_errno), __func__, __LINE__);
      kni_port_params_array[i]->tx_ring = tx_ring;

      snprintf(name, sizeof(name), "ring_kni_%u", i);
      kni_ring = rte_ring_create(name, RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
      if (kni_ring == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create KNI ring, %s, %s():%d\n",
                 rte_strerror(rte_errno), __func__, __LINE__);

      kni_port_params_array[i]->kni_ring = kni_ring;
    }
  }
}


static void rx_thread(struct kni_port_params *p) {
  unsigned nb_rx;//, sent;

  struct timespec nano;
  nano.tv_sec = 0;
  nano.tv_nsec = 1000;

  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];
  

  if (unlikely(p == NULL))
    return;

  // struct rte_ring *rx_ring = p->rx_ring;
  
  
  while (!force_quit) {
    /* Burst rx from eth */
    nb_rx = rte_eth_rx_burst(0, 0, pkts_burst, PKT_BURST_SZ);
    if (unlikely(nb_rx == 0)) {
      nanosleep(&nano, NULL);
      continue;
    }
	rte_kni_handle_request(p->kni);

	printf("rx_thread --> %d\n", nb_rx);
#if 0
	
    sent = rte_ring_sp_enqueue_burst(rx_ring, (void **) pkts_burst, nb_rx, NULL);
    if (unlikely(sent < nb_rx)) {
      while (sent < nb_rx)
        rte_pktmbuf_free(pkts_burst[sent++]);
    }

#else

	struct rte_ether_hdr *ehdr = rte_pktmbuf_mtod(pkts_burst[0], struct rte_ether_hdr*);
	if (ehdr->ether_type != rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
		continue;
	}

	printf("process_pkts --> ");
	print_ethaddr("src: ", ehdr->s_addr.addr_bytes);
	print_ethaddr("\t\t dest: ", ehdr->d_addr.addr_bytes);
	printf("\n\n");
	
	fflush(stdout);

	struct ip_hdr *ihdr = rte_pktmbuf_mtod_offset(pkts_burst[0], struct ip_hdr *, sizeof(struct ip_hdr));

	printf("protocol: %d, ihdr->dip: %x, localIP: %x\n", ihdr->proto, ihdr->dip, localIP);
	

#endif

  }
}

static void kni_thread(struct kni_port_params *p) {
  unsigned nb_rx, num;

  struct timespec nano;
  nano.tv_sec = 0;
  nano.tv_nsec = 1000;

  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];
  struct rte_ring *kni_ring;

  if (unlikely(p == NULL))
    return;

  kni_ring = p->kni_ring;

  while (!force_quit) {
    nb_rx = rte_ring_sc_dequeue_burst(kni_ring, (void **) pkts_burst, PKT_BURST_SZ, NULL);
    if (unlikely(nb_rx == 0)) {
      nanosleep(&nano, NULL);
      continue;
    }

    num = rte_kni_tx_burst(p->kni, pkts_burst, nb_rx);
    rte_kni_handle_request(p->kni);
    if (unlikely(num < nb_rx)) {
      kni_burst_free_mbufs(&pkts_burst[num], nb_rx - num);
    }
  }
}


unsigned process_pkts(struct rte_mbuf *pkt) {

	unsigned txpkts = 0;

	struct rte_ether_addr *ehdr = rte_pktmbuf_mtod(pkt, struct rte_ether_addr*);
	if (*(uint16_t*)(ehdr->addr_bytes+12) != rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
		return txpkts;
	}

	printf("process_pkts --> ");
	print_ethaddr("src: ", &ehdr->addr_bytes[0]);
	print_ethaddr("\t\t dest: ", &ehdr->addr_bytes[6]);
	printf("\n\n");
	
	fflush(stdout);
	
	struct rte_ipv4_hdr *ihdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ipv4_hdr));

	printf("protocol: %d, ihdr->dip: %x, localIP: %x\n", ntohs(ihdr->next_proto_id), ihdr->dst_addr, localIP);
	if (ihdr->dst_addr != localIP)
    	return txpkts;

	
	switch (ntohs(ihdr->next_proto_id)) {

		case IPPROTO_UDP: {
			struct rte_udp_hdr *uhdr = (struct rte_udp_hdr *) ((unsigned char *) ihdr + sizeof(struct rte_udp_hdr));

			//struct rte_udp_hdr *upkt = rte_pktmbuf_mtod(pkt, struct rte_udp_hdr*);
			
			
			unsigned short udplen = ntohs(uhdr->dgram_len);
			*(char*)(((char *)uhdr) + udplen - 8) = '\0';
			//upkt->body[uhdr->dgram_len-8] = '\0';

			printf("udp--> %s\n", (char*)(uhdr+1));
			
		}

	}

	return 0;
}

static void worker_thread(void *arg) {
  unsigned j, ret, sent;
  unsigned nb_rx;

  struct timespec nano;
  nano.tv_sec = 0;
  nano.tv_nsec = 1000;

  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];
  struct rte_ring *rx_ring, *tx_ring, *kni_ring;

  struct kni_port_params *p = (struct kni_port_params *) arg;

  rx_ring = p->rx_ring;
  tx_ring = p->tx_ring;
  kni_ring = p->kni_ring;

  while (!force_quit) {
    nb_rx = rte_ring_sc_dequeue_burst(rx_ring, (void **) pkts_burst, PKT_BURST_SZ, NULL);
    if (unlikely(nb_rx == 0)) {
      nanosleep(&nano, NULL);
      continue;
    }

	printf("worker_thread --> %d\n", nb_rx);
    for (j = 0; j < nb_rx; j++) {
      ret = process_pkts(pkts_burst[j]);
      if (ret)
        sent = rte_ring_sp_enqueue_burst(tx_ring, (void **) &pkts_burst[j], 1, NULL);
      else
        sent = rte_ring_sp_enqueue_burst(kni_ring, (void **) &pkts_burst[j], 1, NULL);

      if (unlikely(sent < 1))
        rte_pktmbuf_free(pkts_burst[sent]);
    }
  }
}

static void tx_thread(void *arg) {
  uint8_t port_id;

  struct timespec nano;
  nano.tv_sec = 0;
  nano.tv_nsec = 1000;

  uint16_t nb_tx, num;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];
  struct rte_ring *tx_ring;

  struct kni_port_params *p = (struct kni_port_params *) arg;
  port_id = p->port_id;
  tx_ring = p->tx_ring;

  while (!force_quit) {
    /* Burst rx from kni */
    num = rte_kni_rx_burst(p->kni, pkts_burst, PKT_BURST_SZ);
    if (likely(num)) {
      nb_tx = rte_eth_tx_burst(port_id, 0, pkts_burst, num);
      if (unlikely(nb_tx < num))
        kni_burst_free_mbufs(&pkts_burst[nb_tx], num - nb_tx);
    }

    num = rte_ring_sc_dequeue_burst(tx_ring, (void **) pkts_burst, PKT_BURST_SZ, NULL);
    if (unlikely(num == 0)) {
      nanosleep(&nano, NULL);
      continue;
    }

    nb_tx = rte_eth_tx_burst(port_id, 0, pkts_burst, num);
    total_send_out += num;
    if (unlikely(nb_tx < num)) {
      kni_burst_free_mbufs(&pkts_burst[nb_tx], num - nb_tx);
    }
  }
}


static int main_loop(__rte_unused void *arg) {
  uint8_t i, nb_ports = rte_eth_dev_count_avail();
  const unsigned lcore_id = rte_lcore_id();
  enum lcore_rxtx {
    LCORE_NONE,
    LCORE_RX,
    LCORE_TX,
    LCORE_WORK,
    LCORE_SEND,
    LCORE_MAX
  };
  enum lcore_rxtx flag = LCORE_NONE;

  for (i = 0; i < nb_ports; i++) {
    if (!kni_port_params_array[i])
      continue;
    if (kni_port_params_array[i]->lcore_rx == (uint8_t) lcore_id) {
      flag = LCORE_RX;
      break;
    } else if (kni_port_params_array[i]->lcore_tx == (uint8_t) lcore_id) {
      flag = LCORE_TX;
      break;
    } else if (kni_port_params_array[i]->lcore_work == (uint8_t) lcore_id) {
      flag = LCORE_WORK;
      break;
    } else if (kni_port_params_array[i]->lcore_send == (uint8_t) lcore_id) {
      flag = LCORE_SEND;
      break;
    }
  }

  if (flag == LCORE_RX) {
    printf("Lcore %u is reading from port %d\n",
            kni_port_params_array[i]->lcore_rx,
            kni_port_params_array[i]->port_id);
    rx_thread(kni_port_params_array[i]);
  } else if (flag == LCORE_TX) {
    printf("Lcore %u is writing to port %d\n",
            kni_port_params_array[i]->lcore_tx,
            kni_port_params_array[i]->port_id);
    kni_thread(kni_port_params_array[i]);
  } else if (flag == LCORE_WORK) {
    printf("Lcore %u is working to port %d\n",
            kni_port_params_array[i]->lcore_work,
            kni_port_params_array[i]->port_id);
    worker_thread(kni_port_params_array[i]);
  } else if (flag == LCORE_SEND) {
    printf("Lcore %u is sending to port %d\n",
            kni_port_params_array[i]->lcore_send,
            kni_port_params_array[i]->port_id);
    tx_thread(kni_port_params_array[i]);
  } else
    printf("Lcore %u has nothing to do\n", lcore_id);

  return 0;
}

static void print_usage(const char *prgname) {
  printf("\nUsage: %s [EAL options] -- -p PORTMASK"
      "[--config (port,lcore_rx,lcore_tx,lcore_work,lcore_send)"
      "[,(port,lcore_rx,lcore_tx,lcore_work,lcore_send)]]\n"
      "    -p PORTMASK: hex bitmask of ports to use\n"
      "    --config (port,lcore_rx,lcore_tx,lcore_work,lcore_send): "
      "port and lcore configurations\n",
          prgname);
}


static void print_config(void) {
  uint32_t i;
  struct kni_port_params **p = kni_port_params_array;

  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (!p[i])
      continue;
    printf("Port ID: %d\n", p[i]->port_id);
    printf("Rx lcore ID: %u, Tx lcore ID: %u\n",
            p[i]->lcore_rx, p[i]->lcore_tx);
  }
}


static int validate_parameters(uint32_t portmask) {
  uint32_t i;

  if (!portmask) {
    printf("No port configured in port mask\n");
    return -1;
  }

  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (((portmask & (1 << i)) && !kni_port_params_array[i]) ||
        (!(portmask & (1 << i)) && kni_port_params_array[i]))
      rte_exit(EXIT_FAILURE, "portmask is not consistent "
          "to port ids specified in --config\n");

    if (kni_port_params_array[i] && !rte_lcore_is_enabled(\
            (unsigned) (kni_port_params_array[i]->lcore_rx)))
      rte_exit(EXIT_FAILURE, "lcore id %u for "
                   "port %d receiving not enabled\n",
               kni_port_params_array[i]->lcore_rx,
               kni_port_params_array[i]->port_id);

    if (kni_port_params_array[i] && !rte_lcore_is_enabled(\
            (unsigned) (kni_port_params_array[i]->lcore_tx)))
      rte_exit(EXIT_FAILURE, "lcore id %u for "
                   "port %d transmitting not enabled\n",
               kni_port_params_array[i]->lcore_tx,
               kni_port_params_array[i]->port_id);

    if (kni_port_params_array[i] && !rte_lcore_is_enabled(\
            (unsigned) (kni_port_params_array[i]->lcore_work)))
      rte_exit(EXIT_FAILURE, "lcore id %u for "
                   "port %d working not enabled\n",
               kni_port_params_array[i]->lcore_work,
               kni_port_params_array[i]->port_id);

    if (kni_port_params_array[i] && !rte_lcore_is_enabled(\
            (unsigned) (kni_port_params_array[i]->lcore_send)))
      rte_exit(EXIT_FAILURE, "lcore id %u for "
                   "port %d sending not enabled\n",
               kni_port_params_array[i]->lcore_send,
               kni_port_params_array[i]->port_id);
  }

  return 0;
}


static int parse_config(const char *arg) {
  const char *p, *p0 = arg;
  char s[256], *end;
  unsigned size;
  enum fieldnames {
    FLD_PORT = 0,
    FLD_LCORE_RX,
    FLD_LCORE_TX,
    _NUM_FLD = KNI_MAX_KTHREAD + 3,
  };
  int i, nb_token;
  char *str_fld[_NUM_FLD];
  unsigned long int_fld[_NUM_FLD];
  uint8_t port_id, nb_kni_port_params = 0;

  memset(&kni_port_params_array, 0, sizeof(kni_port_params_array));
  while (((p = strchr(p0, '(')) != NULL) &&
      nb_kni_port_params < RTE_MAX_ETHPORTS) {
    p++;
    if ((p0 = strchr(p, ')')) == NULL)
      goto fail;
    size = p0 - p;
    if (size >= sizeof(s)) {
      printf("Invalid config parameters\n");
      goto fail;
    }
    snprintf(s, sizeof(s), "%.*s", size, p);
    nb_token = rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',');
    if (nb_token <= FLD_LCORE_TX) {
      printf("Invalid config parameters\n");
      goto fail;
    }
    for (i = 0; i < nb_token; i++) {
      errno = 0;
      int_fld[i] = strtoul(str_fld[i], &end, 0);
      if (errno != 0 || end == str_fld[i]) {
        printf("Invalid config parameters\n");
        goto fail;
      }
    }

    i = 0;
    port_id = (uint8_t) int_fld[i++];
    if (port_id >= RTE_MAX_ETHPORTS) {
      printf("Port ID %d could not exceed the maximum %d\n",
             port_id, RTE_MAX_ETHPORTS);
      goto fail;
    }
    if (kni_port_params_array[port_id]) {
      printf("Port %d has been configured\n", port_id);
      goto fail;
    }

    kni_port_params_array[port_id] = (struct kni_port_params *)
        rte_zmalloc("KNI_port_params",
                    sizeof(struct kni_port_params), RTE_CACHE_LINE_SIZE);
    if (kni_port_params_array[port_id] == NULL)
      return -ENOMEM;

    kni_port_params_array[port_id]->port_id = port_id;
    kni_port_params_array[port_id]->lcore_rx = (uint8_t) int_fld[i++];
    kni_port_params_array[port_id]->lcore_tx = (uint8_t) int_fld[i++];
    kni_port_params_array[port_id]->lcore_work = (uint8_t) int_fld[i++];
    kni_port_params_array[port_id]->lcore_send = (uint8_t) int_fld[i++];

    if (kni_port_params_array[port_id]->lcore_rx >= RTE_MAX_LCORE ||
        kni_port_params_array[port_id]->lcore_tx >= RTE_MAX_LCORE ||
        kni_port_params_array[port_id]->lcore_work >= RTE_MAX_LCORE ||
        kni_port_params_array[port_id]->lcore_send >= RTE_MAX_LCORE) {
      printf("lcore_rx %u or lcore_tx %u ID could not "
                 "exceed the maximum %u\n",
             kni_port_params_array[port_id]->lcore_rx,
             kni_port_params_array[port_id]->lcore_tx,
             (unsigned) RTE_MAX_LCORE);
      goto fail;
    }
  }
  print_config();

  return 0;

  fail:
  for (i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (kni_port_params_array[i]) {
      rte_free(kni_port_params_array[i]);
      kni_port_params_array[i] = NULL;
    }
  }

  return -1;
}

static uint32_t
parse_unsigned(const char *portmask) {
  char *end = NULL;
  unsigned long num;

  num = strtoul(portmask, &end, 16);
  if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
    return 0;

  return (uint32_t) num;
}


static int parse_args(int argc, char **argv) {
  int opt, longindex, ret = 0;
  const char *prgname = argv[0];
  static struct option longopts[] = {
      {CMDLINE_OPT_CONFIG, required_argument, NULL, 0},
      {NULL, 0, NULL, 0}
  };

  /* Disable printing messages within getopt() */
  opterr = 0;

  /* Parse command line */
  while ((opt = getopt_long(argc, argv, "p:", longopts,
                            &longindex)) != EOF) {
    switch (opt) {
      case 'p': ports_mask = parse_unsigned(optarg);
        break;

      case 0:
        if (!strncmp(longopts[longindex].name,
                     CMDLINE_OPT_CONFIG,
                     sizeof(CMDLINE_OPT_CONFIG))) {
          ret = parse_config(optarg);
          if (ret) {
            printf("Invalid config\n");
            print_usage(prgname);
            return -1;
          }
        }
        break;

      default: print_usage(prgname);
        rte_exit(EXIT_FAILURE, "Invalid option specified\n");
    }
  }

  /* Check that options were parsed ok */
  if (validate_parameters(ports_mask) < 0) {
    print_usage(prgname);
    rte_exit(EXIT_FAILURE, "Invalid parameters\n");
  }

  return ret;
}


int main(int argc, char **argv) {

	int ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Could not initialise EAL (%d)\n", ret);
	}
	argc -= ret;
	argv += ret;

	force_quit = 0;
	signal(SIGUSR1, signal_handler);
  	signal(SIGINT, signal_handler);

	localIP = get_netorder_ip("192.168.2.114");

	ret = parse_args(argc, argv);
  	if (ret < 0)
    	rte_exit(EXIT_FAILURE, "Could not parse input parameters\n");
	

	pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NB_MBUF, 
		MEMPOOL_CACHE_SZ, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (pktmbuf_pool == NULL) {
		rte_exit(EXIT_FAILURE, "Could not initialise mbuf pool\n");
	}

	
	uint8_t nb_sys_ports = rte_eth_dev_count_avail();
	if (nb_sys_ports == 0) {
		rte_exit(EXIT_FAILURE, "No supported Ethernet device found\n");
	}

	init_kni();

	init_ring();

	uint8_t port;
	RTE_ETH_FOREACH_DEV(port) {
		/* Skip ports that are not enabled */
		if (!(ports_mask & (1 << port)))
			continue;
		init_port(port);

		if (port >= RTE_MAX_ETHPORTS)
			rte_exit(EXIT_FAILURE, "Can not use more than "
				"%d ports for kni\n", RTE_MAX_ETHPORTS);

		kni_alloc(port);
	}
	check_all_ports_link_status(ports_mask);

	unsigned i = 0;
	rte_eal_mp_remote_launch(main_loop, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(i) {
		if (rte_eal_wait_lcore(i) < 0)
			return -1;
	}

	/* Release resources */
	RTE_ETH_FOREACH_DEV(port) {
		if (!(ports_mask & (1 << port)))
			continue;
		kni_free_kni(port);
	}
	for (i = 0; i < RTE_MAX_ETHPORTS; i++)
		if (kni_port_params_array[i]) {
			rte_free(kni_port_params_array[i]);
			kni_port_params_array[i] = NULL;
		}

	return 0;
	

}




