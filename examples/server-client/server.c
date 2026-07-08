

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>
#include <stdarg.h>
#include <errno.h>

#include <inttypes.h>
#include <sys/queue.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>



#include <rte_memory.h>
#include <rte_string_fns.h>

#include <rte_common.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_byteorder.h>
#include <rte_atomic.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_debug.h>
#include <rte_ring.h>
#include <rte_log.h>
#include <rte_mempool.h>
#include <rte_memcpy.h>
#include <rte_mbuf.h>
#include <rte_interrupts.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_string_fns.h>
#include <rte_cycles.h>


#define MP_CLIENT_RXQ_NAME 	"MProc_Client_%u_RX"

#define MZ_PORT_INFO 		"MProc_port_info"
#define PKTMBUF_POOL_NAME	"MProc_pktmbuf_pool"


#define MAX_CLIENTS		16
#define CLIENT_QUEUE_RINGSIZE		128
#define PACKET_READ_SIZE	32

#define RTE_MP_TX_DESC_DEFAULT		1024
#define RTE_MP_RX_DESC_DEFAULT		1024
#define MBUF_CACHE_SIZE			512

struct rx_stats {
	uint64_t rx[RTE_MAX_ETHPORTS];
} __rte_cache_aligned;

struct tx_stats {
	uint64_t tx[RTE_MAX_ETHPORTS];
	uint64_t tx_drop[RTE_MAX_ETHPORTS];
} __rte_cache_aligned;

struct port_info {
	uint16_t num_ports;
	uint16_t id[RTE_MAX_ETHPORTS];

	volatile struct rx_stats rx_stats;
	volatile struct tx_stats tx_stats[MAX_CLIENTS];
};

struct client {

	struct rte_ring *rx_q;
	unsigned client_id;

	struct {
		volatile uint64_t rx;
		volatile uint64_t rx_drop;
	} stats;
	
};

struct client_rx_buf {
	struct rte_mbuf *buffer[PACKET_READ_SIZE];
	uint16_t count;
};


struct rte_mempool *pktmbuf_pool;
struct port_info *ports;
struct client *clients;
struct client_rx_buf *cl_rx_buf;

uint8_t num_clients;
static const char *progname;

static void usage(void) {
	printf(
	    "%s [EAL options] -- -p PORTMASK -n NUM_CLIENTS [-s NUM_SOCKETS]\n"
	    " -p PORTMASK: hexadecimal bitmask of ports to use\n"
	    " -n NUM_CLIENTS: number of client processes to use\n"
	    , progname);
}

static inline const char *get_rx_queue_name(unsigned id)
{
	/* buffer for return value. Size calculated by %u being replaced
	 * by maximum 3 digits (plus an extra byte for safety) */
	static char buffer[sizeof(MP_CLIENT_RXQ_NAME) + 2];

	snprintf(buffer, sizeof(buffer), MP_CLIENT_RXQ_NAME, id);
	return buffer;
}


static int parse_portmask(uint8_t max_ports, const char *portmask) {
	char *end = NULL;
	unsigned long pm;
	uint16_t count = 0;

	if (portmask == NULL || *portmask == '\0')
		return -1;

	/* convert parameter to a number and verify */
	pm = strtoul(portmask, &end, 16);
	if (end == NULL || *end != '\0' || pm == 0)
		return -1;

	/* loop through bits of the mask and mark ports */
	while (pm != 0){
		if (pm & 0x01){ /* bit is set in mask, use port */
			if (count >= max_ports)
				printf("WARNING: requested port %u not present"
				" - ignoring\n", (unsigned)count);
			else
			    ports->id[ports->num_ports++] = count;
		}
		pm = (pm >> 1);
		count++;
	}

	return 0;
}


static int parse_num_clients(const char *clients)
{
	char *end = NULL;
	unsigned long temp;

	if (clients == NULL || *clients == '\0')
		return -1;

	temp = strtoul(clients, &end, 10);
	if (end == NULL || *end != '\0' || temp == 0)
		return -1;

	num_clients = (uint8_t)temp;
	return 0;
}


static int parse_app_args(uint16_t max_ports, int argc, char *argv[]) {

	int option_index, opt;
	char **argvopt = argv;

	struct option lgopts[] = {
		{NULL, 0, 0, 0}
	};

	progname = argv[0];

	while ((opt = getopt_long(argc, argvopt, "n:p:", lgopts, &option_index)) != EOF) {

		switch (opt) {

			case 'p':
				if (parse_portmask(max_ports, optarg) != 0) {
					usage();
					return -1;
				}
				break;

			case 'n':
				if (parse_num_clients(optarg) != 0) {
					usage();
					return -1;
				}
				break;

			default: 
				printf("ERROR: Unknown option '%c'\n", opt);
				usage();
				return -1;

		}
		
	}

	return 0;
}


static int init_port(uint16_t port_num) {

	const struct rte_eth_conf port_conf = {
		.rxmode = {
			.mq_mode = ETH_MQ_RX_RSS
		}
	};

	const uint16_t rx_rings = 1, tx_rings = num_clients;
	uint16_t rx_ring_size = RTE_MP_RX_DESC_DEFAULT;
	uint16_t tx_ring_size = RTE_MP_RX_DESC_DEFAULT;

	printf("Port %u init ...\n", port_num);
	fflush(stdout);

	int retval = rte_eth_dev_configure(port_num, rx_rings, tx_rings, &port_conf);
	if (retval != 0) return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port_num, &rx_ring_size, &tx_ring_size);
	if (retval != 0) return retval;

	uint16_t q;
	for (q = 0;q < rx_rings;q ++) {
		
		retval = rte_eth_rx_queue_setup(port_num, q, rx_ring_size, 
			rte_eth_dev_socket_id(port_num), NULL, pktmbuf_pool);
		if (retval < 0) return retval;
	}

	for (q = 0;q < tx_rings;q ++) {
		retval = rte_eth_tx_queue_setup(port_num, q, tx_ring_size, 
			rte_eth_dev_socket_id(port_num), NULL);
		
		if (retval < 0) return retval;
	}

	rte_eth_promiscuous_enable(port_num);
	retval = rte_eth_dev_start(port_num);
	if (retval < 0) return retval;

	printf("done: \n");
	return 0;
	
}


static int init_mbuf_pools(void) {

	const unsigned int num_mbufs_server = RTE_MP_RX_DESC_DEFAULT * ports->num_ports;
	const unsigned int num_mbufs_client = num_clients * (CLIENT_QUEUE_RINGSIZE + RTE_MP_TX_DESC_DEFAULT * ports->num_ports);

	const unsigned int num_mbufs_mp_cache = (num_clients + 1) * MBUF_CACHE_SIZE;
	const unsigned int num_mbufs = num_mbufs_server + num_mbufs_client + num_mbufs_mp_cache;

	printf("Creating mbuf pool '%s' [%u mbufs] ...\n",
		PKTMBUF_POOL_NAME, num_mbufs);

	pktmbuf_pool = rte_pktmbuf_pool_create(PKTMBUF_POOL_NAME, num_mbufs, MBUF_CACHE_SIZE, 0,
		RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	
	return pktmbuf_pool == NULL;
}

static void check_all_ports_link_status(uint16_t port_num, uint32_t port_mask) {

#define CHECK_INTERVAL	100
#define MAX_CHECK_TIME	90

	uint8_t print_flag = 0;
	printf("\nChecking link status\n");
	fflush(stdout);

	int count = 0;
	for (count = 0;count <= MAX_CHECK_TIME;count ++) {

		uint8_t all_ports_up;
		
		uint16_t portid;
		for (portid = 0;portid < port_num;portid ++) {
			if ((port_mask & (1 << ports->id[portid])) == 0) continue;

			struct rte_eth_link link;
			memset(&link, 0, sizeof(link));

			rte_eth_link_get_nowait(ports->id[portid], &link);

			if (print_flag == 1) {
				if (link.link_status) {
					printf("Port %d Link up - speed %u Mbps - %s\n",
						ports->id[portid], (unsigned)link.link_speed,
						(link.link_duplex == ETH_LINK_FULL_DUPLEX) ? ("fll-duplex") : ("half-duplex\n"));
				} else {
					printf("Port %d Link Down\n", (uint8_t)ports->id[portid]);
				}
				continue;
			}

			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}

		if (print_flag == 1) break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}

}

static int init_shm_rings(void) {

	const unsigned ringsize = CLIENT_QUEUE_RINGSIZE;

	clients = rte_malloc("client details", sizeof(*clients) * num_clients, 0);
	if (clients == NULL) {
		rte_exit(EXIT_FAILURE, "Cannot allocate memory for client program details\n");
	}

	int i;
	for (i = 0;i < num_clients;i ++) {
		unsigned socket_id = rte_socket_id();
		const char *q_name = get_rx_queue_name(i);

		clients[i].rx_q = rte_ring_create(q_name, ringsize, socket_id, RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (clients[i].rx_q == NULL) {
			rte_exit(EXIT_FAILURE, "Cannot create rx ring queue for client %u\n", i);
		}
	}

	return 0;
	
}


static int init_server(int argc, char *argv[]) {

	int retval;

	retval = rte_eal_init(argc, argv);
	if (retval < 0) {
		return -1;
	}

	argc -= retval;
	argv += retval;

	uint16_t total_ports = rte_eth_dev_count_total();

	const struct rte_memzone *mz = rte_memzone_reserve(MZ_PORT_INFO, sizeof(*ports), rte_socket_id(), 0);
	if (mz == NULL) {
		rte_exit(EXIT_FAILURE, "Cannot reserve memory zone for port information\n");	
	}

	memset(mz->addr, 0, sizeof(*ports));
	ports = mz->addr;

	retval = parse_app_args(total_ports, argc, argv);
	if (retval != 0) return -1;

	retval = init_mbuf_pools();
	if (retval != 0) 
		rte_exit(EXIT_FAILURE, "Cannot create needed mbuf pools\n");

	int i = 0;
	for (i = 0;i < ports->num_ports;i ++) {
		retval = init_port(ports->id[i]);
		if (retval != 0) 
			rte_exit(EXIT_FAILURE, "Cannot initialize port %u\n", (unsigned)i);
	}

	check_all_ports_link_status(ports->num_ports, (~0x0));

	init_shm_rings();

	return 0;

} 

static void flush_rx_queue(uint16_t cli) {

	uint16_t j;
	struct client *cl;

	if (cl_rx_buf[cli].count == 0) return ;

	cl = &clients[cli];
	if (rte_ring_enqueue_bulk(cl->rx_q, (void **)cl_rx_buf[cli].buffer,
		cl_rx_buf[cli].count, NULL) == 0) {

		for (j = 0;j < cl_rx_buf[cli].count;j ++) {
			rte_pktmbuf_free(cl_rx_buf[cli].buffer[j]);
		}
		cl->stats.rx_drop += cl_rx_buf[cli].count;

	} else {
		cl->stats.rx += cl_rx_buf[cli].count;
	}

	cl_rx_buf[cli].count = 0;

}

static inline void enqueue_rx_packet(uint8_t client, struct rte_mbuf *buf) {
	cl_rx_buf[client].buffer[cl_rx_buf[client].count ++] = buf;
}

static void process_packet(uint32_t port_num __rte_unused, struct rte_mbuf *pkts[], uint16_t rx_count) {

	uint16_t i;
	uint8_t client = 0;

	for (i = 0;i < rx_count;i ++) {
		enqueue_rx_packet(client, pkts[i]);

		if (++client == num_clients) client = 0;
	}

	for (i = 0;i < num_clients;i ++) {
		flush_rx_queue(i);
	}

}

static void do_packet_forwarding(void) {

	unsigned port_num = 0;

	for (;;) {
		struct rte_mbuf *buf[PACKET_READ_SIZE];

		uint16_t rx_count = rte_eth_rx_burst(ports->id[port_num], 0, buf, PACKET_READ_SIZE);
		ports->rx_stats.rx[port_num] += rx_count;

		if (likely(rx_count > 0)) {
			process_packet(port_num, buf, rx_count);
		}

		if (++port_num == ports->num_ports) 
			port_num = 0;
	}

}


static const char *get_printable_mac_addr(uint16_t port)
{
	static const char err_address[] = "00:00:00:00:00:00";
	static char addresses[RTE_MAX_ETHPORTS][sizeof(err_address)];

	if (unlikely(port >= RTE_MAX_ETHPORTS))
		return err_address;
	if (unlikely(addresses[port][0]=='\0')){
		struct rte_ether_addr mac;
		rte_eth_macaddr_get(port, &mac);
		snprintf(addresses[port], sizeof(addresses[port]),
				"%02x:%02x:%02x:%02x:%02x:%02x\n",
				mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2],
				mac.addr_bytes[3], mac.addr_bytes[4], mac.addr_bytes[5]);
	}
	return addresses[port];
}


static void do_stats_display(void)
{
	unsigned i, j;
	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };
	uint64_t port_tx[RTE_MAX_ETHPORTS], port_tx_drop[RTE_MAX_ETHPORTS];
	uint64_t client_tx[MAX_CLIENTS], client_tx_drop[MAX_CLIENTS];

	/* to get TX stats, we need to do some summing calculations */
	memset(port_tx, 0, sizeof(port_tx));
	memset(port_tx_drop, 0, sizeof(port_tx_drop));
	memset(client_tx, 0, sizeof(client_tx));
	memset(client_tx_drop, 0, sizeof(client_tx_drop));

	for (i = 0; i < num_clients; i++){
		const volatile struct tx_stats *tx = &ports->tx_stats[i];
		for (j = 0; j < ports->num_ports; j++){
			/* assign to local variables here, save re-reading volatile vars */
			const uint64_t tx_val = tx->tx[ports->id[j]];
			const uint64_t drop_val = tx->tx_drop[ports->id[j]];
			port_tx[j] += tx_val;
			port_tx_drop[j] += drop_val;
			client_tx[i] += tx_val;
			client_tx_drop[i] += drop_val;
		}
	}

	/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("PORTS\n");
	printf("-----\n");
	for (i = 0; i < ports->num_ports; i++)
		printf("Port %u: '%s'\t", (unsigned)ports->id[i],
				get_printable_mac_addr(ports->id[i]));
	printf("\n\n");
	for (i = 0; i < ports->num_ports; i++){
		printf("Port %u - rx: %9"PRIu64"\t"
				"tx: %9"PRIu64"\n",
				(unsigned)ports->id[i], ports->rx_stats.rx[i],
				port_tx[i]);
	}

	printf("\nCLIENTS\n");
	printf("-------\n");
	for (i = 0; i < num_clients; i++){
		const unsigned long long rx = clients[i].stats.rx;
		const unsigned long long rx_drop = clients[i].stats.rx_drop;
		printf("Client %2u - rx: %9llu, rx_drop: %9llu\n"
				"            tx: %9"PRIu64", tx_drop: %9"PRIu64"\n",
				i, rx, rx_drop, client_tx[i], client_tx_drop[i]);
	}

	printf("\n");
}


static void clear_stats(void) {
	unsigned i;

	for (i = 0;i < num_clients;i ++) {
		clients[i].stats.rx = clients[i].stats.rx_drop = 0;
	}
}


static int sleep_lcore(__attribute__((unused)) void *dummy) {
	static rte_atomic32_t display_stats;

	if (rte_atomic32_test_and_set(&display_stats)) {
		const unsigned sleeptime = 1;
		printf("Core %u displaying statistics\n", rte_lcore_id());

		sleep(sleeptime * 3);

		while (sleep(sleeptime) <= sleeptime) {
			do_stats_display();
		}
	}

	return 0;
}

static void signal_handler(int signal) {

	uint16_t port_id;

	if (signal == SIGINT) {

		RTE_ETH_FOREACH_DEV(port_id) {
			rte_eth_dev_stop(port_id);
			rte_eth_dev_close(port_id);
		}

	}

	exit(0);

}



int main(int argc, char *argv[]) {

	signal(SIGINT, signal_handler);

	if (init_server(argc, argv) < 0) {
		return -1;
	}

	printf("Finished Process Init.\n");

	cl_rx_buf = calloc(num_clients, sizeof(struct client_rx_buf));

	clear_stats();
	rte_eal_mp_remote_launch(sleep_lcore, NULL, SKIP_MASTER);

	do_packet_forwarding();
	return 0;

}


